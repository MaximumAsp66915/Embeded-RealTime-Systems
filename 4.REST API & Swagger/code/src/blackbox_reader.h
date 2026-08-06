/*
 * blackbox_reader.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * READ-ONLY here — the Step 5/6 notifier daemon owns the DB (see that
 * project's black_box.c for the schema/writer). This server just opens
 * the same SQLite file (WAL mode makes a concurrent read while another
 * process is writing safe, no locking dance needed) for
 * GET /api/v1/blackbox/count and GET /api/v1/blackbox/events, satisfying
 * "queryable total-detection-count" via the REST API rather than only a
 * manual sqlite3 CLI query.
 */

#ifndef BLACKBOX_READER_H
#define BLACKBOX_READER_H

#include <stddef.h>

/* The never-trimmed total (meta.total_events_ever). Returns 0 on
 * success (writes to *out), -1 on failure (DB missing/unreadable — the
 * notifier daemon may not have created it yet). */
int blackbox_reader_get_total(const char *db_path, long *out);

/* Renders the most recent `limit` events (any type) as a JSON array
 * into out, newest first. Returns bytes written, or -1 on failure. */
int blackbox_reader_get_recent_events_json(const char *db_path, int limit, char *out, size_t out_size);

#endif /* BLACKBOX_READER_H */
