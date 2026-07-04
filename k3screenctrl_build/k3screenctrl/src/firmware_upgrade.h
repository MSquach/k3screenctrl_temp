#ifndef __FIRMWARE_UPGRADE_H
#define __FIRMWARE_UPGRADE_H

/*
 * Non-zero while k3screenctrl is running in firmware upgrade mode.
 * Inspected by signal_notify() so that SIGALRM/SIGTERM are routed to the
 * upgrade state machine instead of the normal page-update logic.
 */
extern int g_in_upgrade_mode;

/*
 * Enter firmware upgrade mode. Takes over the frame callback and the signal
 * flow. Returns only via exit() (success or failure).
 */
void fwupgrade_start();

/*
 * Dispatch a signal to the upgrade state machine.
 */
void fwupgrade_notify_signal(int sig);

#endif
