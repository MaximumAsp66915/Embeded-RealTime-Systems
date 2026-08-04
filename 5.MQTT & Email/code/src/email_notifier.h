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
 * Sends the alert email: subject/body summarize count + timestamp + CPU
 * temp, with frame_path attached as a JPEG. Returns 0 on success, -1 on
 * failure (details on stderr — libcurl's verbose error strings, not
 * silently swallowed).
 */
int email_notifier_send(const notifier_config_t *cfg, int count, const char *timestamp,
                         double cpu_temp_c);

#endif /* EMAIL_NOTIFIER_H */
