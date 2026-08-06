/*
 * blackbox_reader.c — see blackbox_reader.h
 */

#include "blackbox_reader.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int blackbox_reader_get_total(const char *db_path, long *out) {
    sqlite3 *db = NULL;
    /* SQLITE_OPEN_READONLY: this server never writes the DB, and
     * opening read-only avoids ever accidentally creating an empty file
     * at db_path if the notifier daemon hasn't started yet — that
     * failure should look like "not available yet" (return -1), not
     * silently produce a valid-looking empty DB. */
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'total_events_ever';",
                            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    int rc = sqlite3_step(stmt);
    int result = -1;
    if (rc == SQLITE_ROW) {
        *out = (long)sqlite3_column_int64(stmt, 0);
        result = 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

int blackbox_reader_get_recent_events_json(const char *db_path, int limit, char *out, size_t out_size) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }

    if (limit <= 0) limit = 20;
    if (limit > 200) limit = 200; /* keep responses bounded */

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id, event_type, timestamp, person_count, cpu_temp_c, detail "
        "FROM events ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, limit);

    size_t pos = 0;
    int n = snprintf(out + pos, out_size - pos, "[");
    if (n < 0 || (size_t)n >= out_size - pos) { sqlite3_finalize(stmt); sqlite3_close(db); return -1; }
    pos += (size_t)n;

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(stmt, 0);
        const unsigned char *event_type = sqlite3_column_text(stmt, 1);
        const unsigned char *timestamp = sqlite3_column_text(stmt, 2);
        int person_count = sqlite3_column_int(stmt, 3);
        double cpu_temp_c = sqlite3_column_double(stmt, 4);
        const unsigned char *detail = sqlite3_column_text(stmt, 5);

        n = snprintf(out + pos, out_size - pos,
            "%s{\"id\": %lld, \"event_type\": \"%s\", \"timestamp\": \"%s\", "
            "\"person_count\": %d, \"cpu_temp_c\": %.1f, \"detail\": \"%s\"}",
            first ? "" : ", ", id,
            event_type ? (const char *)event_type : "",
            timestamp ? (const char *)timestamp : "",
            person_count, cpu_temp_c,
            detail ? (const char *)detail : "");
        if (n < 0 || (size_t)n >= out_size - pos) break; /* truncate gracefully, don't overflow */
        pos += (size_t)n;
        first = 0;
    }

    n = snprintf(out + pos, out_size - pos, "]");
    if (n >= 0 && (size_t)n < out_size - pos) pos += (size_t)n;

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (int)pos;
}
