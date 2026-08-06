/*
 * guard_mode.c — see guard_mode.h
 */

#define _POSIX_C_SOURCE 200809L /* for gmtime_r */

#include "guard_mode.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int guard_mode_set(const char *path, int enabled) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;
    fprintf(f, "{\"enabled\": %s, \"changed_at\": \"%s\"}", enabled ? "true" : "false", ts);
    fclose(f);

    /* Atomic rename so the reader (Step 5/6's poll loop) never sees a
     * half-written file — same pattern used throughout this project for
     * frame.jpg / control.json. */
    return rename(tmp_path, path);
}

int guard_mode_get(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *key = strstr(buf, "\"enabled\"");
    if (!key) return 0;
    const char *colon = strchr(key, ':');
    if (!colon) return 0;

    return (strncmp(colon + 1, " true", 5) == 0 || strncmp(colon + 1, "true", 4) == 0);
}
