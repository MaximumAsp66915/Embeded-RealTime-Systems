/*
 * persons_reader.c — see persons_reader.h
 */

#include "persons_reader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Finds "key": and returns a pointer just past the colon, or NULL if the
 * key isn't present. Deliberately tolerant of whitespace variations. */
static const char *find_value_start(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *pos = strstr(json, needle);
    if (!pos) return NULL;

    pos = strchr(pos + strlen(needle), ':');
    if (!pos) return NULL;

    pos++; /* skip the colon itself */
    while (*pos == ' ' || *pos == '\t') pos++;
    return pos;
}

int persons_reader_read(const char *path, persons_info_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->timestamp, sizeof(out->timestamp), "unavailable");

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p;

    p = find_value_start(buf, "count");
    if (p) out->count = atoi(p);

    p = find_value_start(buf, "fps");
    if (p) out->fps = atof(p);

    p = find_value_start(buf, "timestamp");
    if (p && *p == '"') {
        p++; /* skip opening quote */
        size_t i = 0;
        while (*p && *p != '"' && i < sizeof(out->timestamp) - 1) {
            out->timestamp[i++] = *p++;
        }
        out->timestamp[i] = '\0';
    }

    return 0;
}
