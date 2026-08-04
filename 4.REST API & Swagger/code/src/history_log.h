/*
 * history_log.h — Smart Surveillance System, Step 4 (REST API)
 *
 * GET /api/v1/history needs "the last 5 detection records" — but
 * person_detector.py (Step 2) only ever writes the CURRENT count/fps/
 * timestamp to persons.json, overwriting it every frame. There's no
 * history there to read.
 *
 * Rather than modifying the already-working Python detector, this keeps
 * "core logic in C" by running a small background thread here that polls
 * persons.json (same file /api/v1/persons reads) at
 * cfg->history_poll_interval_ms, and whenever the timestamp field changes
 * (i.e. the detector actually produced a new frame, not just us polling
 * faster than it updates), pushes a new entry into a 5-slot ring buffer.
 */

#ifndef HISTORY_LOG_H
#define HISTORY_LOG_H

#include "config.h"
#include <stddef.h>

/* Starts the background sampling thread. Call once at server startup,
 * before serving any requests. Safe to call even if person_detector.py
 * isn't running yet — it just won't have anything to record until
 * persons.json appears. */
void history_log_start(const server_config_t *cfg);

/* Renders the current history (oldest first) as a JSON array into `out`,
 * e.g.:
 *   [{"count":2,"timestamp":"..."},{"count":1,"timestamp":"..."}]
 * Returns the number of bytes written (excluding the null terminator),
 * or -1 if it didn't fit in out_size. */
int history_log_get_json(char *out, size_t out_size);

/* Renders a summary of the current ring buffer:
 *   {"records_stored": int, "capacity": 5, "min_count": int,
 *    "max_count": int, "avg_count": float}
 * min/max/avg are 0 if records_stored is 0. Returns bytes written, or -1
 * if it didn't fit in out_size. */
int history_log_get_summary_json(char *out, size_t out_size);

#endif /* HISTORY_LOG_H */
