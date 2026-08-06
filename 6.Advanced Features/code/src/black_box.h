/*
 * black_box.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * SQLite-backed event log, written directly via SQLite's C API (never
 * shelling out to the `sqlite3` CLI). Two things, on purpose kept
 * separate:
 *
 *   - `events` table: a CIRCULAR BUFFER — capped at max_events rows.
 *     Every insert past that cap deletes the oldest row(s) first, so
 *     this table always holds only the most RECENT events, bounded
 *     storage regardless of how long the system runs.
 *   - `meta` table: a single monotonically-increasing counter,
 *     total_events_ever, that is NEVER trimmed by the circular buffer
 *     logic above. This is what answers "queryable total-detection-
 *     count" unambiguously — the events table alone can't answer "how
 *     many detections happened in total" once it's wrapped around and
 *     started overwriting old rows, so this counter exists specifically
 *     to survive that trimming.
 */

#ifndef BLACK_BOX_H
#define BLACK_BOX_H

#include <stddef.h>

typedef struct black_box black_box_t;

typedef enum {
    BB_EVENT_DETECTION,        /* count >= 1 seen in a frame */
    BB_EVENT_GUARD_ALARM,      /* guard mode was active and triggered */
    BB_EVENT_WATCHDOG_TIMEOUT, /* watchdog detected a stale feed */
    BB_EVENT_THERMAL_THROTTLE, /* CPU temp crossed the throttle threshold */
    BB_EVENT_THERMAL_RECOVER,  /* CPU temp dropped back below recovery threshold */
} black_box_event_type_t;

/* Opens (creating if needed) the DB at db_path, creates the schema if
 * it doesn't exist yet, and applies max_events as the circular buffer
 * cap for future inserts (does NOT retroactively trim an existing DB
 * that was previously opened with a larger cap). Returns NULL on
 * failure (details on stderr). */
black_box_t *black_box_open(const char *db_path, int max_events);

void black_box_close(black_box_t *bb);

/* Inserts one event row, then trims the events table down to
 * max_events if the insert pushed it over, and increments the
 * never-trimmed total_events_ever counter. Returns 0 on success, -1 on
 * failure. */
int black_box_log_event(black_box_t *bb, black_box_event_type_t type,
                         int person_count, double cpu_temp_c, const char *detail);

/* The "queryable total-detection-count" — reads meta.total_events_ever.
 * Returns 0 on success (writes the count to *out), -1 on failure. */
int black_box_get_total_event_count(black_box_t *bb, long *out);

/* How many rows are CURRENTLY in the circular buffer (<= max_events).
 * Returns 0 on success, -1 on failure. */
int black_box_get_current_row_count(black_box_t *bb, long *out);

#endif /* BLACK_BOX_H */
