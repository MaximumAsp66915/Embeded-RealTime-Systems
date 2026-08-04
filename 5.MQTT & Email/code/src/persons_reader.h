/*
 * persons_reader.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Reads the tiny JSON file written by Image Processing's SharedOutput class
 * (see Image Processing/code/person_detector.py):
 *     {"count": int, "timestamp": "...", "fps": float}
 *
 * We don't pull in a full JSON library for a 3-field, always-the-same-shape
 * file — a small tolerant scanner is enough and keeps the build dependency
 * -free (no extra -l flags, no vendoring a JSON parser).
 */

#ifndef PERSONS_READER_H
#define PERSONS_READER_H

#include <stddef.h>

typedef struct {
    int count;
    double fps;
    char timestamp[64];
} persons_info_t;

/*
 * Reads and parses `path`. Returns 0 on success, -1 on failure (file
 * missing or unreadable — e.g. person_detector.py hasn't started yet).
 * On failure, `out` is zeroed and timestamp is set to "unavailable".
 */
int persons_reader_read(const char *path, persons_info_t *out);

#endif /* PERSONS_READER_H */
