/*
 * config.c — see config.h
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* Strip leading/trailing whitespace in place, return pointer into the
 * same buffer (does not allocate). */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void set_defaults(server_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->student_name, CFG_MAXLEN, "Unknown");
    snprintf(cfg->student_id, CFG_MAXLEN, "000000");
    cfg->http_port = 80;
    cfg->https_port = 443;
    snprintf(cfg->cert_file, CFG_MAXLEN, "/etc/ssl/surveillance/server.crt");
    snprintf(cfg->key_file, CFG_MAXLEN, "/etc/ssl/surveillance/server.key");
    snprintf(cfg->shared_dir, CFG_MAXLEN, "/dev/shm/surveillance");
    snprintf(cfg->frame_filename, CFG_MAXLEN, "frame.jpg");
    snprintf(cfg->persons_filename, CFG_MAXLEN, "persons.json");
    cfg->mjpeg_frame_interval_ms = 200;
    cfg->stats_poll_interval_ms = 2000;
}

static void build_derived_paths(server_config_t *cfg) {
    snprintf(cfg->frame_path, sizeof(cfg->frame_path), "%s/%s",
             cfg->shared_dir, cfg->frame_filename);
    snprintf(cfg->persons_path, sizeof(cfg->persons_path), "%s/%s",
             cfg->shared_dir, cfg->persons_filename);
}

int config_load(const char *path, server_config_t *out) {
    set_defaults(out);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] could not open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue; /* malformed line, skip rather than crash */

        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);

        if      (strcmp(key, "student_name") == 0) snprintf(out->student_name, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "student_id") == 0)   snprintf(out->student_id, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "http_port") == 0)    out->http_port = atoi(value);
        else if (strcmp(key, "https_port") == 0)   out->https_port = atoi(value);
        else if (strcmp(key, "cert_file") == 0)    snprintf(out->cert_file, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "key_file") == 0)     snprintf(out->key_file, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "shared_dir") == 0)   snprintf(out->shared_dir, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "frame_filename") == 0)   snprintf(out->frame_filename, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "persons_filename") == 0) snprintf(out->persons_filename, CFG_MAXLEN, "%s", value);
        else if (strcmp(key, "mjpeg_frame_interval_ms") == 0) out->mjpeg_frame_interval_ms = atoi(value);
        else if (strcmp(key, "stats_poll_interval_ms") == 0)  out->stats_poll_interval_ms = atoi(value);
        /* Unknown keys are ignored on purpose — forward compatible with a
         * conf file that has extra lines for other parts of the project. */
    }

    fclose(f);
    build_derived_paths(out);
    return 0;
}
