// ==============================================================================
// Section 6 - Alert Daemon.
//
// One binary (`alert_daemon`), deployed identically on master, slave1, and
// slave2 -- each instance only ever looks at its OWN DB_PATH (its own
// `sensors` / `sensor_readings` tables), the exact same way master_node and
// slave_node each only know their own sensors. There is no MQTT, no cache,
// and no cross-node awareness here: this is a purely local watchdog over
// whatever SQLite file it's pointed at. That's a deliberate scope decision,
// not an oversight -- cross-node/aggregated alerting was explicitly out of
// scope for Section 6.
//
// Every POLL_INTERVAL_SEC seconds, the daemon:
//   1. Loads every row in this node's `sensors` table.
//   2. For each sensor, looks up its most recent `sensor_readings` row (and
//      how many seconds old it is, computed directly in SQL via julianday()).
//   3. Evaluates the four sample alert conditions against it.
//   4. Logs a new `alerts` row the moment a condition transitions from
//      "not violated" to "violated", and flips that same row's `status` to
//      'resolved' the moment it transitions back -- see the dedupe note by
//      handle_condition() below for the full rationale.
//
// Schema (created with CREATE TABLE IF NOT EXISTS on startup, in the same
// local DB the SNMP/cache/MQTT cascade already writes to -- no separate
// alerts database):
//   alerts(
//       id INTEGER PRIMARY KEY AUTOINCREMENT,
//       sensor_id TEXT,
//       sensor_name TEXT,
//       alert_type TEXT,
//       sensor_value TEXT,
//       created_at TEXT,
//       status TEXT
//   )
// ==============================================================================

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <sqlite3.h>

// ------------------------------------------------------------------
// Configuration -- all overridable via env vars, same getenv-with-
// defaults pattern main.cpp already uses for DB_PATH / MQTT_BROKER.
// ------------------------------------------------------------------
std::string g_db_path = "master.db";
int    g_poll_interval_sec  = 10;
double g_temp_high_max      = 35.0;
double g_humidity_min       = 20.0;
double g_humidity_max       = 70.0;
int    g_sensor_timeout_sec = 300;
double g_value_min          = -50.0;
double g_value_max          = 1000.0;

bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

// ------------------------------------------------------------------
// Dedupe state: for each (sensor_id, alert_type) pair currently in
// violation, remember the `alerts.id` of the still-open row, so a
// sustained violation doesn't insert a new row every poll cycle. See
// handle_condition() for the full transition logic and the tradeoff
// this implies.
// ------------------------------------------------------------------
std::unordered_map<std::string, sqlite3_int64> g_active_alerts;

// ------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------

// Strict numeric parse: the whole string must be consumed, and it must be
// non-empty. Used for the invalid-value condition (a non-numeric recorded
// value is itself one of the ways a reading can be "invalid") and to guard
// the threshold checks, which only make sense against a real number.
bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* endptr = nullptr;
    out = std::strtod(s.c_str(), &endptr);
    return endptr != s.c_str() && *endptr == '\0';
}

struct SensorMeta {
    std::string sensor_id;
    std::string sensor_type;
    std::string sensor_name;
};

struct LatestReading {
    bool found = false;
    std::string value;
    long long age_seconds = -1;
};

// ------------------------------------------------------------------
// DB access -- straight sqlite3 C API, same style as query_local_db()
// in master/src/main.cpp and slave/src/main.cpp. One connection is
// opened per poll cycle (see run_poll_cycle()) rather than per query,
// since a poll cycle does several queries in a row and the daemon has
// no other writer contending with itself.
// ------------------------------------------------------------------

void ensure_alerts_table(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS alerts ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sensor_id TEXT,"
        "sensor_name TEXT,"
        "alert_type TEXT,"
        "sensor_value TEXT,"
        "created_at TEXT,"
        "status TEXT"
        ");";
    char* errmsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::cerr << "[STARTUP][ERROR] Failed to create alerts table: "
                  << (errmsg ? errmsg : "unknown error") << std::endl;
        if (errmsg) sqlite3_free(errmsg);
    }
}

std::vector<SensorMeta> load_sensors(sqlite3* db) {
    std::vector<SensorMeta> out;
    const char* sql = "SELECT sensor_id, sensor_type, sensor_name FROM sensors;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        auto col = [&](int i) -> std::string {
            const unsigned char* v = sqlite3_column_text(stmt, i);
            return v ? reinterpret_cast<const char*>(v) : "";
        };
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SensorMeta m;
            m.sensor_id   = col(0);
            m.sensor_type = col(1);
            m.sensor_name = col(2);
            out.push_back(m);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

// Most recent reading for a sensor, plus its age in seconds -- computed by
// SQLite itself (julianday('now') - julianday(recorded_at), in days,
// scaled to seconds) rather than parsed in C++, since recorded_at is
// already known to be in a format SQLite's date functions understand (see
// api.cpp's use of date()/time() on the same column).
LatestReading load_latest_reading(sqlite3* db, const std::string& sensor_id) {
    LatestReading r;
    const char* sql =
        "SELECT value, "
        "CAST((julianday('now') - julianday(recorded_at)) * 86400 AS INTEGER) "
        "FROM sensor_readings WHERE sensor_id = ? "
        "ORDER BY recorded_at DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sensor_id.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(stmt, 0);
            r.value = v ? reinterpret_cast<const char*>(v) : "";
            r.age_seconds = sqlite3_column_int64(stmt, 1);
            r.found = true;
        }
        sqlite3_finalize(stmt);
    }
    return r;
}

sqlite3_int64 insert_alert(sqlite3* db, const std::string& sensor_id, const std::string& sensor_name,
                            const std::string& alert_type, const std::string& value) {
    const char* sql =
        "INSERT INTO alerts (sensor_id, sensor_name, alert_type, sensor_value, created_at, status) "
        "VALUES (?, ?, ?, ?, datetime('now'), 'active');";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_int64 row_id = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sensor_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, sensor_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, alert_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, value.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            row_id = sqlite3_last_insert_rowid(db);
        }
        sqlite3_finalize(stmt);
    }
    return row_id;
}

void resolve_alert_row(sqlite3* db, sqlite3_int64 row_id) {
    const char* sql = "UPDATE alerts SET status = 'resolved' WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, row_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ------------------------------------------------------------------
// Dedupe strategy: active/resolved transition tracking, not one row
// per poll cycle.
//
// g_active_alerts remembers, per (sensor_id, alert_type), the row id of
// the still-open ('active') alert for that condition, if any:
//   - condition false -> true (not open yet):  INSERT a new 'active' row,
//     remember its id.
//   - condition stays true (already open):      do nothing -- this is the
//     dedupe: a sustained violation does NOT produce a new row every
//     POLL_INTERVAL_SEC.
//   - condition true -> false (was open):        UPDATE that row's status
//     to 'resolved', forget it.
//   - condition stays false (not open):          do nothing.
//
// Tradeoff (see ALERTS_REPORT.md section 3 for the full writeup): this keeps
// `alerts` a meaningful incident log -- one row per violation *episode*,
// and "SELECT * FROM alerts WHERE status='active'" is a direct answer to
// "what's wrong right now" -- at the cost of not recording how many poll
// cycles a violation lasted, and of in-memory-only state: a daemon
// restart forgets which conditions were already open, so a condition
// that's still violated at restart is logged as a brand-new alert rather
// than recognized as a continuation. No startup reconciliation pass
// against existing 'active' rows is implemented, to keep the daemon
// simple; this is a known, documented limitation.
// ------------------------------------------------------------------
void handle_condition(sqlite3* db, const std::string& sensor_id, const std::string& sensor_name,
                       const std::string& alert_type, bool condition_active, const std::string& value_str) {
    std::string key = sensor_id + "|" + alert_type;
    auto it = g_active_alerts.find(key);
    bool currently_open = (it != g_active_alerts.end());

    if (condition_active && !currently_open) {
        sqlite3_int64 row_id = insert_alert(db, sensor_id, sensor_name, alert_type, value_str);
        if (row_id > 0) {
            g_active_alerts[key] = row_id;
            std::cout << "[ALERT][OPEN] " << alert_type << " sensor_id=" << sensor_id
                      << " (" << sensor_name << ") value=" << value_str << std::endl;
        }
    } else if (!condition_active && currently_open) {
        resolve_alert_row(db, it->second);
        std::cout << "[ALERT][CLEAR] " << alert_type << " sensor_id=" << sensor_id
                  << " (" << sensor_name << ")" << std::endl;
        g_active_alerts.erase(it);
    }
    // else: no state transition -- either still violating (already logged,
    // dedupe in effect) or still fine (nothing to do).
}

// ------------------------------------------------------------------
// The four sample alert conditions, evaluated per sensor per poll.
// ------------------------------------------------------------------
void evaluate_sensor(sqlite3* db, const SensorMeta& sensor, const LatestReading& lr) {
    // Condition 3: sensor timeout -- no reading at all, or the latest one
    // is older than SENSOR_TIMEOUT_SEC. Applies to every sensor regardless
    // of type, and is the only condition that can fire with no reading
    // present at all.
    bool timed_out = !lr.found || lr.age_seconds > g_sensor_timeout_sec;
    handle_condition(db, sensor.sensor_id, sensor.sensor_name, "SENSOR_TIMEOUT",
                      timed_out, lr.found ? lr.value : "NO_DATA");

    if (!lr.found) return;  // nothing further to check without a reading

    double val = 0.0;
    bool numeric = parse_double(lr.value, val);

    // Condition 4: invalid recorded value -- either not a number at all, or
    // numeric but outside a generic sanity range that no real sensor in
    // this cluster should ever produce (VALUE_MIN/VALUE_MAX). This check
    // deliberately takes precedence over the type-specific threshold
    // checks below: if a reading is already flagged invalid, we don't also
    // claim it proves a "confirmed" high-temperature or bad-humidity
    // reading off what is, by definition, garbage data.
    bool invalid = !numeric || val < g_value_min || val > g_value_max;
    handle_condition(db, sensor.sensor_id, sensor.sensor_name, "INVALID_VALUE",
                      invalid, lr.value);

    // Condition 1: high temperature.
    if (sensor.sensor_type == "temperature") {
        bool high_temp = numeric && !invalid && val > g_temp_high_max;
        handle_condition(db, sensor.sensor_id, sensor.sensor_name, "HIGH_TEMPERATURE",
                          high_temp, lr.value);
    }

    // Condition 2: humidity out of the allowed [HUMIDITY_MIN, HUMIDITY_MAX] range.
    if (sensor.sensor_type == "humidity") {
        bool bad_humidity = numeric && !invalid && (val < g_humidity_min || val > g_humidity_max);
        handle_condition(db, sensor.sensor_id, sensor.sensor_name, "HUMIDITY_OUT_OF_RANGE",
                          bad_humidity, lr.value);
    }
}

void run_poll_cycle() {
    sqlite3* db = nullptr;
    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[POLL][ERROR] Could not open DB_PATH=" << g_db_path << std::endl;
        if (db) sqlite3_close(db);
        return;
    }

    std::vector<SensorMeta> sensors = load_sensors(db);
    for (const auto& sensor : sensors) {
        LatestReading lr = load_latest_reading(db, sensor.sensor_id);
        evaluate_sensor(db, sensor, lr);
    }

    sqlite3_close(db);
}

int main() {
    const char* db_env       = std::getenv("DB_PATH");
    const char* poll_env     = std::getenv("POLL_INTERVAL_SEC");
    const char* temp_env     = std::getenv("TEMP_HIGH_MAX");
    const char* hum_min_env  = std::getenv("HUMIDITY_MIN");
    const char* hum_max_env  = std::getenv("HUMIDITY_MAX");
    const char* timeout_env  = std::getenv("SENSOR_TIMEOUT_SEC");
    const char* val_min_env  = std::getenv("VALUE_MIN");
    const char* val_max_env  = std::getenv("VALUE_MAX");

    if (db_env) g_db_path = db_env;
    if (poll_env    && *poll_env)    g_poll_interval_sec  = std::atoi(poll_env);
    if (temp_env    && *temp_env)    g_temp_high_max      = std::atof(temp_env);
    if (hum_min_env && *hum_min_env) g_humidity_min       = std::atof(hum_min_env);
    if (hum_max_env && *hum_max_env) g_humidity_max       = std::atof(hum_max_env);
    if (timeout_env && *timeout_env) g_sensor_timeout_sec = std::atoi(timeout_env);
    if (val_min_env && *val_min_env) g_value_min          = std::atof(val_min_env);
    if (val_max_env && *val_max_env) g_value_max          = std::atof(val_max_env);

    std::cout << "[STARTUP] Initializing Alert Daemon..." << std::endl;
    std::cout << "[STARTUP] DB_PATH=" << g_db_path << std::endl;
    std::cout << "[STARTUP] POLL_INTERVAL_SEC=" << g_poll_interval_sec << std::endl;
    std::cout << "[STARTUP] TEMP_HIGH_MAX=" << g_temp_high_max << std::endl;
    std::cout << "[STARTUP] HUMIDITY_MIN=" << g_humidity_min << " HUMIDITY_MAX=" << g_humidity_max << std::endl;
    std::cout << "[STARTUP] SENSOR_TIMEOUT_SEC=" << g_sensor_timeout_sec << std::endl;
    std::cout << "[STARTUP] VALUE_MIN=" << g_value_min << " VALUE_MAX=" << g_value_max << std::endl;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    {
        sqlite3* db = nullptr;
        if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "[STARTUP][FATAL] Could not open DB_PATH=" << g_db_path << std::endl;
            if (db) sqlite3_close(db);
            return 1;
        }
        ensure_alerts_table(db);
        sqlite3_close(db);
    }

    std::cout << "[STARTUP] alerts table ready (CREATE TABLE IF NOT EXISTS). Entering poll loop." << std::endl;

    while (g_running) {
        run_poll_cycle();

        // Sleep in short chunks rather than one long sleep, so SIGINT/
        // SIGTERM are honored promptly instead of waiting out a full
        // POLL_INTERVAL_SEC -- same responsiveness goal as the 100ms
        // recv timeout in master's snmp_listener_loop().
        const int chunk_ms = 200;
        int slept_ms = 0;
        while (g_running && slept_ms < g_poll_interval_sec * 1000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk_ms));
            slept_ms += chunk_ms;
        }
    }

    std::cout << "[SHUTDOWN] Alert Daemon stopping." << std::endl;
    return 0;
}
