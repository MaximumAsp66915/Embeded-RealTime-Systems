/*
 * email_notifier.h — Smart Surveillance System, Step 5 (MQTT + Email)
 *
 * Sends the detection alert email via libcurl's SMTP support — not by
 * shelling out to `mail`/`sendmail`/`msmtp`, and not a hand-rolled SMTP
 * socket implementation either. libcurl's easy interface + curl_mime
 * gives us STARTTLS/TLS, auth, and a multipart body (text + JPEG
 * attachment) in well-tested library code, which is the appropriate
 * level of "write this in C" for a coursework SMTP client — reimplementing
 * MIME/TLS/SASL by hand would just be reinventing libcurl, badly.
 */

#ifndef EMAIL_NOTIFIER_H
#define EMAIL_NOTIFIER_H

#include "config.h"

/*
 * Sends the alert email: subject/body summarize the reason + count +
 * timestamp + CPU temp, with frame_path attached as a JPEG (if it can
 * be read — see email_notifier.c for why a missing/unreadable frame
 * doesn't block the send). Returns 0 on success, -1 on failure (details
 * on stderr — libcurl's verbose error strings, not silently swallowed).
 *
 * `reason` is a short human string that becomes part of the subject
 * line — this is the ONE email-sending path shared by all of Step 6's
 * triggers (plain detection, Guard Mode alarm, watchdog timeout,
 * thermal throttle), each passing a different reason, e.g.:
 *   "Person detected"
 *   "GUARD MODE ALARM"
 *   "Watchdog: camera feed stale"
 *   "Thermal: CPU over threshold, throttling"
 * Deliberately ONE function, not four near-duplicate ones — every
 * caller still goes through the exact same debounce check in main.c
 * (should_send_email()), which is what actually enforces "max 1 email
 * per 30 seconds" regardless of which trigger fired.
 */
int email_notifier_send(const notifier_config_t *cfg, const char *reason, int count,
                         const char *timestamp, double cpu_temp_c);

#endif /* EMAIL_NOTIFIER_H */
