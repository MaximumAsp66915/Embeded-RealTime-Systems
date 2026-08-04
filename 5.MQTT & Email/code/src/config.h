/*
 * config.h — Smart Surveillance System, Step 5 (MQTT + Email)
 *
 * Loads notifier.conf: a flat key=value file, same style as Step 3/4's
 * server.conf. Deliberately does NOT hardcode credentials anywhere in
 * source — SMTP username/password and the MQTT broker address all come
 * from this file, which you edit locally and should NOT commit with real
 * credentials in it (see notifier.conf's own top comment).
 */

#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAXLEN 256

typedef struct {
    /* --- Identity --- */
    char student_id[CFG_MAXLEN];

    /* --- Shared with Step 2/3/4 --- */
    char frame_path[CFG_MAXLEN * 2];
    char persons_path[CFG_MAXLEN * 2];

    /* --- Polling --- */
    int poll_interval_ms;      /* how often to re-check persons.json + telemetry */
    int debounce_seconds;      /* min seconds between emails, regardless of how
                                 * many detections happen in between */

    /* --- MQTT --- */
    char mqtt_host[CFG_MAXLEN];
    int mqtt_port;
    int mqtt_keepalive_s;
    char mqtt_topic_prefix[CFG_MAXLEN]; /* e.g. "home" -> home/persons/<id> */
    char mqtt_client_id[CFG_MAXLEN * 2];    /* derived from student_id if left blank */
    /* Leave both blank for an anonymous-access broker. If your broker
     * requires auth (e.g. secure_setup.sh's Mosquitto config, which sets
     * allow_anonymous false), these come from that script's own
     * secrets.env output — MQTT_USER / MQTT_PASS. */
    char mqtt_username[CFG_MAXLEN];
    char mqtt_password[CFG_MAXLEN];

    /* --- Email (SMTP via libcurl) --- */
    char smtp_url[CFG_MAXLEN];      /* e.g. smtp://smtp.gmail.com:587 or smtps://...:465 */
    char smtp_username[CFG_MAXLEN];
    char smtp_password[CFG_MAXLEN];
    char smtp_from[CFG_MAXLEN];
    char smtp_to[CFG_MAXLEN];
    int smtp_use_starttls; /* 1 = STARTTLS (typical for port 587), 0 = implicit TLS/plain */
} notifier_config_t;

/* Fills `out` with defaults, then overrides from the key=value file at
 * `path`. Returns 0 on success (even if the file has some unknown keys —
 * those are just ignored), -1 if the file couldn't be opened at all. */
int config_load(const char *path, notifier_config_t *out);

#endif /* CONFIG_H */
