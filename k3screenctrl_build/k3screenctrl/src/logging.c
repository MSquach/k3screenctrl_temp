#include <stdarg.h>
#include <stdio.h>

#include "logging.h"

int g_syslog_used = 0;

void k3_syslog(int priority, const char *format, ...) {
    g_syslog_used = 1;
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

void syslog_setup(int print_stderr) {
    int log_options = LOG_CONS | LOG_PID;
    if (print_stderr)
        log_options |= LOG_PERROR;

    openlog(LOG_IDENT, log_options, LOG_USER);
}

void syslog_stop() {
    if (g_syslog_used) {
        fprintf(stderr, "INFO: Logs were sent to syslog. Run 'logread' to view them.\n");
    }
    closelog();
}
