/*
 * black_box.c — see black_box.h
 */

#define _POSIX_C_SOURCE 200809L /* for gmtime_r */

#include "black_box.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct black_box {
    sqlite3 *db;
    int max_events;
};

static const char *event_type_name(black_box_event_type_t type) {
    switch (type) {
        case BB_EVENT_DETECTION:        return "detection";
        case BB_EVENT_GUARD_ALARM:      return "guard_alarm";
        case BB_EVENT_WATCHDOG_TIMEOUT: return "watchdog_timeout";
        case BB_EVENT_THERMAL_THROTTLE: return "thermal_throttle";
        case BB_EVENT_THERMAL_RECOVER:  return "thermal_recover";
        default:                        return "unknown";
    }
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[black_box] SQL error: %s (sql: %s)\n", err ? err : "?", sql);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

black_box_t *black_box_open(const char *db_path, int max_events) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "[black_box] could not open %s: %s\n", db_path, sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* WAL mode: safer for a process that's writing every poll cycle
     * while something else (e.g. a manual `sqlite3` query for a report
     * screenshot) might read the DB concurrently. */
    exec_sql(db, "PRAGMA journal_mode=WAL;");

    const char *schema =
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  event_type TEXT NOT NULL,"
        "  timestamp TEXT NOT NULL,"
        "  person_count INTEGER,"
        "  cpu_temp_c REAL,"
        "  detail TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value INTEGER NOT NULL"
        ");"
        "INSERT OR IGNORE INTO meta (key, value) VALUES ('total_events_ever', 0);";

    if (exec_sql(db, schema) != 0) {
        sqlite3_close(db);
        return NULL;
    }

    black_box_t *bb = calloc(1, sizeof(black_box_t));
    if (!bb) {
        sqlite3_close(db);
        return NULL;
    }
    bb->db = db;
    bb->max_events = (max_events > 0) ? max_events : 1000;

    return bb;
}

void black_box_close(black_box_t *bb) {
    if (!bb) return;
    if (bb->db) sqlite3_close(bb->db);
    free(bb);
}

int black_box_log_event(black_box_t *bb, black_box_event_type_t type,
                         int person_count, double cpu_temp_c, const char *detail) {
    if (!bb) return -1;

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    const char *insert_sql =
        "INSERT INTO events (event_type, timestamp, person_count, cpu_temp_c, detail) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(bb->db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[black_box] prepare failed: %s\n", sqlite3_errmsg(bb->db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, event_type_name(type), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, person_count);
    sqlite3_bind_double(stmt, 4, cpu_temp_c);
    sqlite3_bind_text(stmt, 5, detail ? detail : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[black_box] insert failed: %s\n", sqlite3_errmsg(bb->db));
        return -1;
    }

    /* Never-trimmed total counter — increment unconditionally, BEFORE
     * the circular buffer trim below, so it reflects every event that
     * was ever logged regardless of how much of the events table has
     * since been overwritten. */
    exec_sql(bb->db, "UPDATE meta SET value = value + 1 WHERE key = 'total_events_ever';");

    /* Circular buffer: trim the events table down to max_events by
     * deleting the oldest rows past that cap. Simpler and just as
     * correct as a literal fixed-size ring buffer for this use case —
     * "circular buffer" here means "bounded, oldest-evicted-first
     * storage," which this achieves without needing manual slot/index
     * bookkeeping. */
    char trim_sql[256];
    snprintf(trim_sql, sizeof(trim_sql),
        "DELETE FROM events WHERE id NOT IN ("
        "  SELECT id FROM events ORDER BY id DESC LIMIT %d"
        ");", bb->max_events);
    exec_sql(bb->db, trim_sql);

    return 0;
}

int black_box_get_total_event_count(black_box_t *bb, long *out) {
    if (!bb || !out) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(bb->db, "SELECT value FROM meta WHERE key = 'total_events_ever';",
                            -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -1;
}

int black_box_get_current_row_count(black_box_t *bb, long *out) {
    if (!bb || !out) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(bb->db, "SELECT COUNT(*) FROM events;", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -1;
}
