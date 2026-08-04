/*
 * config.c — see config.h
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s) {
    /* trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
    /* leading */
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
}

static void set_defaults(notifier_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->student_id, sizeof(cfg->student_id), "000000000");

    snprintf(cfg->frame_path, sizeof(cfg->frame_path), "/dev/shm/surveillance/frame.jpg");
    snprintf(cfg->persons_path, sizeof(cfg->persons_path), "/dev/shm/surveillance/persons.json");

    cfg->poll_interval_ms = 2000;
    cfg->debounce_seconds = 30;

    snprintf(cfg->mqtt_host, sizeof(cfg->mqtt_host), "localhost");
    cfg->mqtt_port = 1883;
    cfg->mqtt_keepalive_s = 60;
    snprintf(cfg->mqtt_topic_prefix, sizeof(cfg->mqtt_topic_prefix), "home");
    cfg->mqtt_client_id[0] = '\0'; /* derived from student_id at runtime if blank */

    snprintf(cfg->smtp_url, sizeof(cfg->smtp_url), "smtp://localhost:587");
    cfg->smtp_use_starttls = 1;
}

int config_load(const char *path, notifier_config_t *out) {
    set_defaults(out);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);
        if (key[0] == '\0') continue;

        if      (strcmp(key, "student_id") == 0)        snprintf(out->student_id, sizeof(out->student_id), "%s", value);
        else if (strcmp(key, "frame_path") == 0)          snprintf(out->frame_path, sizeof(out->frame_path), "%s", value);
        else if (strcmp(key, "persons_path") == 0)        snprintf(out->persons_path, sizeof(out->persons_path), "%s", value);
        else if (strcmp(key, "poll_interval_ms") == 0)    out->poll_interval_ms = atoi(value);
        else if (strcmp(key, "debounce_seconds") == 0)    out->debounce_seconds = atoi(value);
        else if (strcmp(key, "mqtt_host") == 0)           snprintf(out->mqtt_host, sizeof(out->mqtt_host), "%s", value);
        else if (strcmp(key, "mqtt_port") == 0)           out->mqtt_port = atoi(value);
        else if (strcmp(key, "mqtt_keepalive_s") == 0)    out->mqtt_keepalive_s = atoi(value);
        else if (strcmp(key, "mqtt_topic_prefix") == 0)   snprintf(out->mqtt_topic_prefix, sizeof(out->mqtt_topic_prefix), "%s", value);
        else if (strcmp(key, "mqtt_client_id") == 0)      snprintf(out->mqtt_client_id, sizeof(out->mqtt_client_id), "%s", value);
        else if (strcmp(key, "mqtt_username") == 0)       snprintf(out->mqtt_username, sizeof(out->mqtt_username), "%s", value);
        else if (strcmp(key, "mqtt_password") == 0)       snprintf(out->mqtt_password, sizeof(out->mqtt_password), "%s", value);
        else if (strcmp(key, "smtp_url") == 0)            snprintf(out->smtp_url, sizeof(out->smtp_url), "%s", value);
        else if (strcmp(key, "smtp_username") == 0)       snprintf(out->smtp_username, sizeof(out->smtp_username), "%s", value);
        else if (strcmp(key, "smtp_password") == 0)       snprintf(out->smtp_password, sizeof(out->smtp_password), "%s", value);
        else if (strcmp(key, "smtp_from") == 0)           snprintf(out->smtp_from, sizeof(out->smtp_from), "%s", value);
        else if (strcmp(key, "smtp_to") == 0)              snprintf(out->smtp_to, sizeof(out->smtp_to), "%s", value);
        else if (strcmp(key, "smtp_use_starttls") == 0)   out->smtp_use_starttls = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        /* unknown keys are silently ignored, same policy as Step 3/4's config.c */
    }

    fclose(f);

    if (out->mqtt_client_id[0] == '\0') {
        snprintf(out->mqtt_client_id, sizeof(out->mqtt_client_id), "surveillance-notifier-%s", out->student_id);
    }

    return 0;
}
