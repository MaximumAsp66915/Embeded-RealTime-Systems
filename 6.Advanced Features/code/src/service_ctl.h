/*
 * service_ctl.h — Smart Surveillance System, Step 4 bonus (REST API)
 *
 * Lets the API restart or fetch logs for a FIXED, ALLOWLISTED set of
 * systemd units — surveillance-web/imgproc/mqtt/gateway. This is
 * deliberately NOT a general "run any systemctl/journalctl command"
 * interface: every function here takes a service *name*, validates it
 * against SERVICE_ALLOWLIST, and only then invokes systemctl/journalctl
 * via execvp() with a fixed argv array — never a shell string built from
 * request input, so there is no command-injection surface regardless of
 * what a caller sends as the name (an unrecognized name is just rejected
 * before any process is spawned).
 */

#ifndef SERVICE_CTL_H
#define SERVICE_CTL_H

#include <stddef.h>

/* The only unit names any function in this module will act on. Add a
 * service here (and nowhere else) to make it manageable via the API. */
extern const char *const SERVICE_ALLOWLIST[];
extern const size_t SERVICE_ALLOWLIST_COUNT;

int service_ctl_is_allowed(const char *name);

/*
 * Restarts a unit. Returns 0 if systemctl reported success, -1 otherwise
 * (message_out explains why either way).
 *
 * Special case: restarting "surveillance-web" restarts the very process
 * handling this request. To let the HTTP response actually reach the
 * client first, this spawns a short-delayed, detached restart instead of
 * restarting synchronously and reports "restart scheduled" rather than a
 * confirmed result — see the .c file for details.
 */
int service_ctl_restart(const char *name, char *message_out, size_t message_out_size);

/* Fills status_out with systemctl's one-word ActiveState (e.g. "active",
 * "inactive", "failed"). Returns 0 on success, -1 on failure. */
int service_ctl_get_status(const char *name, char *status_out, size_t status_out_size);

/*
 * Fills out with the last `lines` journalctl entries for the unit,
 * newline-separated, truncated to fit out_size. Returns the number of
 * bytes written, or -1 on failure.
 */
int service_ctl_get_logs(const char *name, int lines, char *out, size_t out_size);

#endif /* SERVICE_CTL_H */
