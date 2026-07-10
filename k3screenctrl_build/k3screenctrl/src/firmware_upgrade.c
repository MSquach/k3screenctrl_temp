#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#include "common.h"
#include "frame.h"
#include "mcu_proto.h"
#include "config.h"
#include "hwdef.h"
#include "file_util.h"
#include "gpio.h"
#include "requests.h"

#define UPGRADE_STAGE_IDLE 0
#define UPGRADE_STAGE_VERSION_CHECK 1
#define UPGRADE_STAGE_ERASE 2
#define UPGRADE_STAGE_FLASH 3
#define UPGRADE_STAGE_RESET 4
#define UPGRADE_STAGE_CONFIRM 5

/* usleep() this time before sending command */
#define UPGRADE_STAGE_VERSION_CHECK_USLEEP 500000
#define UPGRADE_STAGE_ERASE_USLEEP 100000
#define UPGRADE_STAGE_FLASH_USLEEP 10000 /* Sleep before every single frame */
#define UPGRADE_STAGE_RESET_USLEEP 1000000
#define UPGRADE_STAGE_CONFIRM_USLEEP UPGRADE_STAGE_VERSION_CHECK_USLEEP

/* Time out after this time after sending command */
#define UPGRADE_STAGE_VERSION_CHECK_TIMEOUT 3
#define UPGRADE_STAGE_ERASE_TIMEOUT 10
#define UPGRADE_STAGE_FLASH_TIMEOUT 3 /* Timeout for every single frame */
#define UPGRADE_STAGE_RESET_TIMEOUT 1
#define UPGRADE_STAGE_CONFIRM_TIMEOUT UPGRADE_STAGE_VERSION_CHECK_TIMEOUT

#define FLASH_IHEX_BATCH_SIZE 10 /* Send 10 lines of iHex file in one frame */

int g_in_upgrade_mode = 0;

static int g_upgrade_stage = UPGRADE_STAGE_IDLE;
static int g_flash_finished = 0;

/* Retry counter for the current stage. Reset to 0 on every successful step. */
static int g_retry_count = 0;
#define UPGRADE_MAX_RETRY 3

/*
 * Set when a timeout occurs in ERASE/FLASH stage, cleared on success.
 * While set, SIGTERM/SIGINT are allowed even during erasing/flashing,
 * because the MCU is no longer in a critical write operation (it has
 * stopped responding). This prevents the program from being stuck forever
 * when the MCU becomes unresponsive.
 */
static int g_timed_out = 0;

/*
 * Expected MCU major version parsed from the firmware filename
 * (e.g. "app.1.2.116.hex" → 1). -1 means the filename did not contain
 * a recognizable <number>.<number>.<number> pattern, so version
 * matching is skipped (the user was prompted to confirm at startup).
 * During the VERSION_CHECK stage, if this does not match the MCU's
 * reported major version, the user is prompted to confirm.
 */
static int g_expected_major = -1;

/*
 * Parse version numbers from a firmware filename.
 * Searches for the pattern <number>.<number>.<number> anywhere in the
 * filename, so "app.1.2.116.hex", "app.2.2.120_ela.hex", "1.2.116", etc.
 * all match. Returns SUCCESS and fills major/minor/patch on match,
 * FAILURE otherwise.
 */
static int parse_firmware_filename_version(const char *path,
                                           int *major, int *minor, int *patch) {
    const char *basename = strrchr(path, '/');
    basename = (basename != NULL) ? basename + 1 : path;

    for (const char *p = basename; *p != '\0'; p++) {
        char *endptr;
        long m = strtol(p, &endptr, 10);
        const char *p1 = endptr;
        if (p1 == p || *p1 != '.')
            continue;

        long n = strtol(p1 + 1, &endptr, 10);
        const char *p2 = endptr;
        if (p2 == p1 + 1 || *p2 != '.')
            continue;

        long pt = strtol(p2 + 1, &endptr, 10);
        const char *p3 = endptr;
        if (p3 == p2 + 1)
            continue;

        *major = (int)m;
        *minor = (int)n;
        *patch = (int)pt;
        return SUCCESS;
    }
    return FAILURE;
}

static void fwupgrade_step_success();

static unsigned char char2hex(const char *str) {
#define LOWER2HEX(digit) (((digit) >= 'a') ? (10 + ((digit) - 'a')) : ((digit) - '0'))
    const char digit0 = tolower(str[0]);
    const char digit1 = tolower(str[1]);

    if (digit0 == '\0') {
        return 0;
    }

    if (digit1 == '\0') {
        return LOWER2HEX(digit0);
    }

    return 16 * LOWER2HEX(digit0) + LOWER2HEX(digit1);
}

/*
 * In k3_7new, frame_send(data, len) sends `data` verbatim as the payload and
 * computes the CRC over it. The first byte of `data` therefore plays the role
 * of the frame type. To send a bootloader command we prepend the type byte to
 * the payload, which produces the exact same wire format as the original dev
 * branch's frame_send(type, data, len).
 */

static int bl_send_version_req() {
    unsigned char type = FRAME_BL_MCU_VERSION_REQ;
    printf("INFO: Checking bootloader version...\n");
    return frame_send(&type, 1);
}

static int bl_send_erase_req() {
    unsigned char type = FRAME_BL_ERASE_REQ;
    printf("INFO: Erasing flash...\n");
    return frame_send(&type, 1);
}

static int bl_send_flash_req(const unsigned char *payload, int payload_len) {
    unsigned char *buf = (unsigned char *)malloc(payload_len + 1);
    if (buf == NULL) {
        fprintf(stderr, "ERROR: could not allocate flash TX buffer\n");
        return FAILURE;
    }
    buf[0] = FRAME_BL_FLASH_REQ;
    if (payload_len > 0) {
        memcpy(buf + 1, payload, payload_len);
    }
    int ret = frame_send(buf, payload_len + 1);
    free(buf);
    return ret;
}

static int bl_request_flash() {
    static FILE *fp = NULL;
    static int offset = 0;

    int lines_read = 0;
    int bin_bytes_written = 0;
    char line_buf[512];
    unsigned char bin_buf[512];

    if (fp == NULL) {
        fp = fopen(CFG->firmware_path, "r");
        if (fp == NULL) {
            fprintf(stderr, "ERROR: could not open %s after the flash has been "
                    "erased - you may want to check the file and restart the "
                    "upgrade process.\n", CFG->firmware_path);
            exit(EXIT_FAILURE);
        }
    }

    while (lines_read++ < FLASH_IHEX_BATCH_SIZE) {
        char *read_result = fgets(line_buf, sizeof(line_buf), fp);
        if (read_result != NULL) {
            if (line_buf[0] == ':') {
                /* Assume even length after ":" */
                int curr_pos = 1, line_len = strlen(read_result);

                if (line_buf[line_len - 1] == '\n') {
                    line_buf[line_len - 1] = '\0';
                    line_len--;
                }

                if (line_buf[line_len - 1] == '\r') {
                    line_buf[line_len - 1] = '\0';
                    line_len--;
                }

                while (curr_pos < line_len) {
                    if (bin_bytes_written >= (int)sizeof(bin_buf)) {
                        fprintf(stderr, "ERROR: iHex batch decodes to more than "
                                "%zu bytes and overflows the %zu-byte TX buffer. "
                                "The firmware file may be malformed; you may "
                                "want to check the file and restart the upgrade "
                                "process.\n", (size_t)bin_bytes_written,
                                sizeof(bin_buf));
                        exit(EXIT_FAILURE);
                    }
                    bin_buf[bin_bytes_written++] = char2hex(&line_buf[curr_pos]);
                    curr_pos += 2;
                }
            }
        } else {
            if (feof(fp)) {
                /* Finished sending */
                g_flash_finished = 1;
            } else {
                /* Read error */
                fprintf(stderr, "ERROR: could not read from firmware file: %d. "
                        "You may want to check the file and restart the upgrade "
                        "process.\n", ferror(fp));
                exit(EXIT_FAILURE);
            }
        }
    }

    printf("INFO: Sending %d-byte chunk from offset %d\n", bin_bytes_written, offset);
    offset += bin_bytes_written;
    bl_send_flash_req(bin_buf, bin_bytes_written);
    return 1; /* Suggest success */
}

static int fwupgrade_reset_normal() {
    printf("INFO: Resetting MCU to normal mode...\n");
    if (gpio_set_value(SCREEN_BOOT_MODE_GPIO, BOOT_MODE_APP) == FAILURE ||
        gpio_set_value(SCREEN_RESET_GPIO, 0) == FAILURE ||
        gpio_set_value(SCREEN_RESET_GPIO, 1) == FAILURE) {
        fprintf(stderr, "Could not reset screen to normal mode\n");
        return FAILURE;
    }
    return SUCCESS;
}

static int fwupgrade_set_stage(int stage, int timeout, int delay) {
    g_upgrade_stage = stage;
    usleep(delay);
    return alarm(timeout);
}

/* Proceed to next step */
static void fwupgrade_step_success() {
#define SET_STAGE(x) fwupgrade_set_stage(x, x ## _TIMEOUT, x ## _USLEEP)

    g_retry_count = 0;
    g_timed_out = 0;

    switch (g_upgrade_stage) {
        case UPGRADE_STAGE_IDLE:
            SET_STAGE(UPGRADE_STAGE_VERSION_CHECK);
            bl_send_version_req();
            break;
        case UPGRADE_STAGE_VERSION_CHECK:
            SET_STAGE(UPGRADE_STAGE_ERASE);
            bl_send_erase_req();
            break;
        case UPGRADE_STAGE_ERASE:
            SET_STAGE(UPGRADE_STAGE_FLASH);
            bl_request_flash();
            break;
        case UPGRADE_STAGE_FLASH:
            if (g_flash_finished != 1) {
                SET_STAGE(UPGRADE_STAGE_FLASH);
                bl_request_flash();
            } else {
                SET_STAGE(UPGRADE_STAGE_RESET);
                fwupgrade_reset_normal();
            }
            break;
        case UPGRADE_STAGE_RESET:
            /* We are in normal mode, request with normal app commands */
            SET_STAGE(UPGRADE_STAGE_CONFIRM);
            request_mcu_version();
            break;
        case UPGRADE_STAGE_CONFIRM:
            printf("MCU firmware has been successfully upgraded. You can restart k3screenctrl now.\n");
            exit(EXIT_SUCCESS);
            break;
    }
}

static void fwupgrade_handle_timeout() {
    g_retry_count++;
    g_timed_out = 1;

    switch (g_upgrade_stage) {
        case UPGRADE_STAGE_VERSION_CHECK:
            if (g_retry_count > UPGRADE_MAX_RETRY) {
                fprintf(stderr, "ERROR: bootloader did not respond to version request after %d "
                        "retries. Aborting.\n", UPGRADE_MAX_RETRY);
                exit(EXIT_FAILURE);
            }
            fprintf(stderr, "WARNING: bootloader did not respond to version request "
                    "(retry %d/%d). Retrying...\n", g_retry_count, UPGRADE_MAX_RETRY);
            bl_send_version_req();
            alarm(UPGRADE_STAGE_VERSION_CHECK_TIMEOUT);
            break;
        case UPGRADE_STAGE_ERASE:
            if (g_retry_count > UPGRADE_MAX_RETRY) {
                fprintf(stderr, "ERROR: flash erasing timed out after %d retries. "
                        "Aborting.\n", UPGRADE_MAX_RETRY);
                exit(EXIT_FAILURE);
            }
            fprintf(stderr, "WARNING: flash erasing timed out (retry %d/%d). "
                    "Retrying...\n", g_retry_count, UPGRADE_MAX_RETRY);
            bl_send_erase_req();
            alarm(UPGRADE_STAGE_ERASE_TIMEOUT);
            break;
        case UPGRADE_STAGE_FLASH:
            if (g_retry_count > UPGRADE_MAX_RETRY) {
                fprintf(stderr, "ERROR: flash writing timed out after %d retries. "
                        "Aborting.\n", UPGRADE_MAX_RETRY);
                exit(EXIT_FAILURE);
            }
            fprintf(stderr, "WARNING: flash writing timed out (retry %d/%d). "
                    "Retrying...\n", g_retry_count, UPGRADE_MAX_RETRY);
            /* Do not resend the chunk — the MCU may have received it but was
             * slow to ACK. Resending would duplicate the data. Just re-arm
             * the alarm and wait. */
            alarm(UPGRADE_STAGE_FLASH_TIMEOUT);
            break;
        case UPGRADE_STAGE_RESET:
            /* By design: wait for _TIMEOUT seconds and jump to next stage. */
            fwupgrade_step_success();
            break;
        case UPGRADE_STAGE_CONFIRM:
            if (g_retry_count > UPGRADE_MAX_RETRY) {
                fprintf(stderr, "ERROR: new firmware not responding after %d "
                        "retries. The upgrade may have failed. Aborting.\n",
                        UPGRADE_MAX_RETRY);
                exit(EXIT_FAILURE);
            }
            fprintf(stderr, "WARNING: new firmware still not up (retry %d/%d). "
                    "Retrying...\n", g_retry_count, UPGRADE_MAX_RETRY);
            request_mcu_version();
            break;
    }
}

static void fwupgrade_frame_handler(const unsigned char *frame, int len) {
    unsigned char ver_major = 0, ver_minor = 0;

    switch (frame[0]) {
    case FRAME_BL_MCU_VERSION_REQ:
        ver_minor = frame[1];
        ver_major = frame[2];

        if (g_upgrade_stage != UPGRADE_STAGE_VERSION_CHECK &&
            g_upgrade_stage != UPGRADE_STAGE_CONFIRM) {
            fprintf(stderr, "bootloader reported version but we did not request it?\n");
            return;
        }

        printf("INFO: Bootloader reported version: %d.%d\n", ver_major, ver_minor);
        if (g_expected_major >= 0 && ver_major != g_expected_major) {
            /* Different major version means different architecture
             * (e.g. 1.x = MIPS, 2.x = ARM). They are incompatible. */
            fprintf(stderr, "ERROR: bootloader major version (%d) does not match "
                    "firmware file (%d). Different major versions use "
                    "different architectures and are incompatible.\n",
                    ver_major, g_expected_major);
            fwupgrade_reset_normal();
            exit(EXIT_FAILURE);
        } else if (g_expected_major < 0) {
            /* Filename had no recognizable version — the user already
             * confirmed at startup, just note it. */
            fprintf(stderr, "WARNING: Could not verify bootloader version against "
                    "firmware filename. Bootloader reports %d.%d. Proceeding.\n",
                    ver_major, ver_minor);
        }
        fwupgrade_step_success();
        break;

    case FRAME_BL_ERASE_REQ:
        if (g_upgrade_stage != UPGRADE_STAGE_ERASE) {
            fprintf(stderr, "WARNING: MCU reported erase successful but we did not request it?\n");
            return;
        }
        fwupgrade_step_success();
        break;

    case FRAME_BL_FLASH_REQ:
        if (g_upgrade_stage != UPGRADE_STAGE_FLASH) {
            fprintf(stderr, "WARNING: MCU reported flash successful but we did not request it?\n");
            return;
        }
        fwupgrade_step_success();
        break;

    case PAYLOAD_HEADER:
        /* A normal APP frame can arrive anytime after resetting to normal mode. */
        if (g_upgrade_stage == UPGRADE_STAGE_RESET) {
            /* MCU just booted into app mode. Any APP frame means it's alive.
             * Advance to CONFIRM, which sends request_mcu_version() and arms
             * the timeout alarm. */
            fwupgrade_step_success();
            return;
        }
        if (g_upgrade_stage == UPGRADE_STAGE_CONFIRM) {
            /* In CONFIRM, only accept the MCU version response. Other APP
             * frames (e.g. key presses) are ignored so we don't declare
             * success prematurely. */
            if (len >= 2 && frame[1] == RESPONSE_MCU_VERSION) {
                if (len >= 6) {
                    unsigned short patch_ver = frame[2] | (frame[3] << 8);
                    unsigned char major_ver = frame[4];
                    unsigned char minor_ver = frame[5];
                    printf("INFO: MCU firmware upgraded to version %d.%d.%d\n",
                           major_ver, minor_ver, patch_ver);
                } else {
                    printf("INFO: MCU firmware upgraded (version response too short to parse)\n");
                }
                fwupgrade_step_success();
            }
            return;
        }
        /* fall thru */
    default:
        fprintf(stderr, "WARNING: MCU reported %d in stage %d but we do not support that\n",
                frame[0], g_upgrade_stage);
        break;
    }
}


void fwupgrade_notify_signal(int sig) {
    switch (sig) {
    case SIGALRM:
        fwupgrade_handle_timeout();
        break;
    case SIGTERM:
    case SIGINT:
        if ((g_upgrade_stage == UPGRADE_STAGE_ERASE ||
             g_upgrade_stage == UPGRADE_STAGE_FLASH) && !g_timed_out) {
            fprintf(stderr, "WARNING: Currently erasing or flashing. SIGTERM/INT rejected.\n");
            fprintf(stderr, "         If the MCU is unresponsive, wait for timeout to exit.\n");
            return;
        }
        exit(EXIT_FAILURE);
        break;
    default:
        printf("INFO: signal %d received and ignored\n", sig);
        break;
    }
}

void fwupgrade_start() {
    FILE *tmp_fp = NULL;
    tmp_fp = fopen(CFG->firmware_path, "r");
    if (tmp_fp == NULL) {
        fprintf(stderr, "ERROR: file %s could not be read (%s).\n",
                CFG->firmware_path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    fclose(tmp_fp);

    /* Try to extract version from the firmware filename.
     * Leniently searches for <number>.<number>.<number> anywhere in the
     * filename, e.g. "app.1.2.116.hex" → 1.2.116, "app.2.2.120_ela.hex" → 2.2.120 */
    int fw_major = -1, fw_minor = -1, fw_patch = -1;
    if (parse_firmware_filename_version(CFG->firmware_path,
                                        &fw_major, &fw_minor, &fw_patch) == SUCCESS) {
        g_expected_major = fw_major;
        printf("INFO: Firmware file version: %d.%d.%d\n", fw_major, fw_minor, fw_patch);
    } else {
        g_expected_major = -1;
        fprintf(stderr, "WARNING: Could not determine firmware version from filename '%s'.\n",
                CFG->firmware_path);
        fprintf(stderr, "         Expected filename to contain <major>.<minor>.<patch> (e.g. app.1.2.116.hex)\n");
        fprintf(stderr, "         Version verification will be skipped.\n");
        fprintf(stderr, "Continue with upgrade? (y/N): ");
        fflush(stderr);

        char confirm[8];
        if (fgets(confirm, sizeof(confirm), stdin) == NULL ||
            (confirm[0] != 'y' && confirm[0] != 'Y')) {
            fprintf(stderr, "Upgrade cancelled.\n");
            exit(EXIT_FAILURE);
        }
    }

    g_in_upgrade_mode = 1;
    frame_set_received_callback(fwupgrade_frame_handler);
    fwupgrade_step_success(); /* Begin from IDLE */
}
