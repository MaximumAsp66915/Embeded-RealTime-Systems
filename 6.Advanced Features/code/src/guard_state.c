/*
 * guard_state.c — see guard_state.h
 */

#include "guard_state.h"

#include <stdio.h>
#include <string.h>

int guard_state_is_enabled(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0; /* missing file = guard mode off, not an error */

    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Same tolerant-scanner approach as persons_reader.c — this is a
     * fixed, tiny, always-the-same-shape file, not a case for a full
     * JSON parser. Scoped to right after the "enabled" key specifically
     * (not "does the word true appear anywhere") so it can't misfire on
     * some other field's content. */
    const char *key = strstr(buf, "\"enabled\"");
    if (!key) return 0;

    const char *colon = strchr(key, ':');
    if (!colon) return 0;

    return (strncmp(colon + 1, " true", 5) == 0 || strncmp(colon + 1, "true", 4) == 0);
}
