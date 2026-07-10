#include "common.h"
#include "config.h"
#include "frame.h"
#include "gpio.h"
#include "handlers.h"
#include "hwdef.h"
#include "infocenter.h"
#include "logging.h"
#include "mcu_proto.h"
#include "mem_util.h"
#include "pages.h"
#include "requests.h"
#include "serial_port.h"
#include "signals.h"
#include "firmware_upgrade.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERIAL_PORT_PATH "/dev/ttyS1"

static void frame_handler(const unsigned char *frame, int len) {
    if (frame[0] != PAYLOAD_HEADER) {
        syslog(LOG_WARNING, "frame with unknown type received: %hhx\n",
               frame[0]);
        return;
    }

    extern RESPONSE_HANDLER g_response_handlers[];
    for (RESPONSE_HANDLER *handler = &g_response_handlers[0]; handler != NULL;
         handler++) {
        if (handler->type == frame[1]) {
            handler->handler(frame + 2,
                             len - 2); /* Start from payload content */
            return;
        }
    }

    syslog(LOG_WARNING, "frame with unknown response type received: %hhx\n",
           frame[1]);
}

/*
 * Enable UART2 in the DMU, prepare the boot-mode / reset GPIOs and reset the
 * MCU into APP mode.
 *
 * The MCU is always reset into APP mode here, regardless of the eventual
 * target mode, so that an app-mode version request can be answered before
 * screen_enter_bootloader() switches it to download mode. A delay after the
 * reset pulse gives the MCU time to boot its app firmware; without it the
 * version request is silently dropped.
 */
static int screen_initialize(int skip_reset) {
    mask_memory_byte(0x1800c1c1, 0xf0, 0); /* Enable UART2 in DMU */

    if (!skip_reset) {
        if (gpio_export(SCREEN_BOOT_MODE_GPIO) == FAILURE ||
            gpio_export(SCREEN_RESET_GPIO) == FAILURE) {
            syslog(LOG_ERR, "Could not export GPIOs\n");
            return FAILURE;
        }

        if (gpio_set_direction(SCREEN_BOOT_MODE_GPIO, GPIO_OUT) == FAILURE ||
            gpio_set_direction(SCREEN_RESET_GPIO, GPIO_OUT) == FAILURE) {
            syslog(LOG_ERR, "Could not set GPIO direction\n");
            return FAILURE;
        }

        if (gpio_set_value(SCREEN_BOOT_MODE_GPIO, BOOT_MODE_APP) == FAILURE ||
            gpio_set_value(SCREEN_RESET_GPIO, 0) == FAILURE ||
            gpio_set_value(SCREEN_RESET_GPIO, 1) == FAILURE) {
            syslog(LOG_ERR, "Could not reset screen\n");
            return FAILURE;
        }

        /* Give the MCU time to boot its app firmware before we talk to it. */
        usleep(1500000);
    }

    return SUCCESS;
}

/*
 * Switch the MCU into bootloader (download) mode by raising the boot-mode
 * GPIO and pulsing reset, then wait for the bootloader to come up. Only
 * call this after screen_initialize().
 */
static int screen_enter_bootloader() {
    if (gpio_set_value(SCREEN_BOOT_MODE_GPIO, BOOT_MODE_BOOTLOADER) == FAILURE ||
        gpio_set_value(SCREEN_RESET_GPIO, 0) == FAILURE ||
        gpio_set_value(SCREEN_RESET_GPIO, 1) == FAILURE) {
        syslog(LOG_ERR, "Could not reset screen into bootloader mode\n");
        return FAILURE;
    }
    usleep(200000); /* Give the bootloader time to come up */
    return SUCCESS;
}

/* Parameters here are too ugly */
void pollin_loop(int serial_fd, int signal_fd) {
    struct pollfd fds[2];
    fds[0].fd = serial_fd;
    fds[0].events = POLLIN;
    fds[1].fd = signal_fd;
    fds[1].events = POLLIN;

    while (1) {
        int readyfds = poll(fds, sizeof(fds) / sizeof(struct pollfd),
                            SERIAL_POLL_INTERVAL_MS);
        if (readyfds < 0) {
            syslog(LOG_ERR, "poll() failed: %s", strerror(errno));
            return;
        } else if (readyfds > 0) {
            if (fds[0].revents & POLLIN) {
                frame_notify_serial_recv();
            } else if (fds[1].revents & POLLIN) {
                signal_notify();
            }
        }
    }
}

void cleanup() {
    serial_close();
    config_free();
    syslog_stop();
}

int main(int argc, char *argv[]) {
    int signal_fd;
    int serial_fd;
    int boot_mode = BOOT_MODE_APP;

    atexit(cleanup);

    config_load_defaults();
    config_parse_cmdline(argc, argv);
    if (CFG->firmware_path[0] != '\0') {
        boot_mode = BOOT_MODE_BOOTLOADER;
    }

    syslog_setup(CFG->foreground);

    /*
     * In upgrade mode the collected info is never used, and running the data
     * scripts (notably weather.sh, which may block on a network API call) on
     * the upgrade path only adds risk. Skip it.
     */
    if (boot_mode == BOOT_MODE_APP) {
        update_all_info();
    }

    if (CFG->test_mode) {
        print_all_info();
        return 0;
    }

    if (screen_initialize(CFG->skip_reset) == FAILURE) {
        return -EIO;
    }

    if ((serial_fd = serial_setup("/dev/ttyS1")) < 0) {
        return -EIO;
    }

    if ((signal_fd = signal_setup()) < 0) {
        return -EIO;
    }

    /*
     * Query the MCU version in app mode before any mode-specific setup.
     * This runs for both normal app startup and firmware upgrade mode; in
     * upgrade mode it logs the version of the firmware that is about to be
     * replaced. The response is handled by handle_mcu_version() via the
     * frame_handler dispatcher.
     */
    frame_set_received_callback(frame_handler);
    request_mcu_version();

    if (boot_mode == BOOT_MODE_APP) {
        page_send_initial_data();
        refresh_screen_timeout();
        alarm(CFG->update_interval);
    } else if (boot_mode == BOOT_MODE_BOOTLOADER) {
        /*
         * Drain the app-mode version response (and any other pending frame)
         * from the serial port before resetting the MCU into bootloader
         * mode. Otherwise the leftover frame would be picked up by
         * fwupgrade_frame_handler and misreported as an error.
         */
        for (int i = 0; i < 20; i++) {
            struct pollfd pfd = { .fd = serial_fd, .events = POLLIN };
            int r = poll(&pfd, 1, 100);
            if (r > 0 && (pfd.revents & POLLIN)) {
                frame_notify_serial_recv();
            } else {
                break; /* No more data within 100 ms */
            }
        }

        /* Now that the current version has been logged, reset the MCU into
         * download mode and hand control to the firmware upgrade state
         * machine. */
        if (CFG->skip_reset == 0) {
            if (screen_enter_bootloader() == FAILURE) {
                return -EIO;
            }
        }
        fwupgrade_start();
    }

    pollin_loop(serial_fd, signal_fd);
}
