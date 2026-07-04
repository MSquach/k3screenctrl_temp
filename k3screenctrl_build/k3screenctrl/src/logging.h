#ifndef _LOGGING_H
#define _LOGGING_H

#include <syslog.h>

#define LOG_IDENT "K3Screen"

/* Set to 1 the first time k3_syslog() (i.e. syslog()) is called. */
extern int g_syslog_used;

/*
 * Wrapper around syslog() that sets g_syslog_used. Existing syslog() calls
 * are automatically redirected here via the macro below, so no source files
 * need to change their call sites.
 */
void k3_syslog(int priority, const char *format, ...);
#define syslog k3_syslog

void syslog_setup();
void syslog_stop();
#endif
