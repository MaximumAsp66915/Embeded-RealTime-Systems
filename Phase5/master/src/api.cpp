// ==============================================================================
// Section 5 - Sensor Log Reading API (Mongoose + SQLite), embedded in Master.
//
//   GET /api/logs?sensor_id=<id>&sensor_type=<type>&date=<YYYY-MM-DD>
//
// Response (200, application/json):
//   {
//     "sensor_name": "Floor1_Room101_Temp",
//     "sensor_id": "101",
//     "date": "2026-06-01",
//     "values": [
//       { "time": "10:00:00", "value": "24.2" },
//       { "time": "10:15:00", "value": "24.8" }
//     ]
//   }
//
// If no readings exist for that sensor/date combination, "values" is an
// empty array and a human-readable "message" field is included instead.
//
// Matching key is sensor_id + sensor_type (sensor_name is no longer an
// input -- it's resolved from the DB and only echoed in the response).
// sensor_type disambiguates in case a sensor_id were ever reused for a
// different kind of sensor; the DB lookup requires both to agree.
//
// Reads directly from the master's existing SQLite database:
//   sensors(sensor_id, sensor_type, sensor_name, location, unit)
//   sensor_readings(sensor_id, value, recorded_at)   -- "YYYY-MM-DD HH:MM:SS"
// This is the exact schema already queried by query_local_db() in
// main.cpp for the SNMP path -- the API adds no tables and requires no
// seeding; it only reads what Phases 1-4 already wrote.
//
// Cluster fallback: sensor_id/sensor_type pairs owned by a slave (e.g.
// 201-204 on slave1, 301-304 on slave2) aren't in the master's own DB.
// When the local lookup misses, the handler falls back to the same
// cache -> local DB -> MQTT cascade pattern main.cpp already uses for the
// SNMP path, via fetch_logs_from_slaves_mqtt() (cluster.h) -- publishing a
// "cluster/slave/logs_request" and waiting (with a timeout) for a slave to
// answer on "cluster/slave/logs_response". This lets one call to
// master:8000/api/logs answer for any sensor in the cluster, not just the
// ones physically stored in master.db.
// ==============================================================================

#include "api.h"
#include "cluster.h"

#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <cctype>
#include <chrono>
#include <iostream>
#include <regex>
#include <sqlite3.h>
#include "mongoose.h"

// Defined in main.cpp; the API thread stops polling once this goes false
// (set by the process's SIGINT/SIGTERM handler).
extern bool g_running;

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string get_query_var(struct mg_http_message* hm, const char* name) {
    char buf[256];
    int len = mg_http_get_var(&hm->query, name, buf, sizeof(buf));
    if (len < 0) return "";
    return std::string(buf, static_cast<size_t>(len));
}

bool is_valid_date(const std::string& date) {
    static const std::regex re(R"(^\d{4}-\d{2}-\d{2}$)");
    return std::regex_match(date, re);
}

bool is_valid_sensor_id(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) if (!isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool is_valid_sensor_type(const std::string& type) {
    // alnum, not alpha: the actual dataset includes a "co2" sensor_type,
    // which contains a digit.
    if (type.empty()) return false;
    for (char c : type) if (!isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}

struct Reading {
    std::string time;
    std::string value;
};

// Parses the ';'-separated "time|value" pairs a slave's logs_response sends
// back (see cluster.h) into the same Reading vector the local-DB path uses,
// so both paths can share build_success_json() below.
std::vector<Reading> parse_values_string(const std::string& encoded) {
    std::vector<Reading> readings;
    if (encoded.empty()) return readings;

    std::stringstream ss(encoded);
    std::string pair;
    while (std::getline(ss, pair, ';')) {
        size_t sep = pair.find('|');
        if (sep == std::string::npos) continue;
        Reading r;
        r.time = pair.substr(0, sep);
        r.value = pair.substr(sep + 1);
        readings.push_back(r);
    }
    return readings;
}

struct QueryResult {
    bool db_open_ok = false;
    bool sensor_exists = false;      // sensor_id is a known row in `sensors`
    std::string actual_sensor_name;  // sensor_name as stored in the DB
    std::vector<Reading> readings;
};

// ---------------------------------------------------------------------------
// Data access -- read-only, straight against the existing tables.
// ---------------------------------------------------------------------------
QueryResult query_sensor_logs(const std::string& db_path, const std::string& sensor_id,
                               const std::string& sensor_type, const std::string& date) {
    QueryResult result;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return result;
    }
    result.db_open_ok = true;

    // Step 1: does a sensor with this id AND this type exist, and what is
    // its canonical name? Requiring both to match (instead of sensor_id
    // alone) is the disambiguation sensor_type is for.
    {
        const char* sql = "SELECT sensor_name FROM sensors WHERE sensor_id = ? AND sensor_type = ? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, sensor_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, sensor_type.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* v = sqlite3_column_text(stmt, 0);
                result.actual_sensor_name = v ? reinterpret_cast<const char*>(v) : "";
                result.sensor_exists = true;
            }
            sqlite3_finalize(stmt);
        }
    }

    // Step 2: every reading for that sensor on that calendar date, oldest
    // first. date(recorded_at) strips the time component so we match on
    // the day regardless of the exact timestamp.
    if (result.sensor_exists) {
        const char* sql =
            "SELECT time(recorded_at), value "
            "FROM sensor_readings "
            "WHERE sensor_id = ? AND date(recorded_at) = ? "
            "ORDER BY recorded_at ASC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, sensor_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, date.c_str(), -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* t = sqlite3_column_text(stmt, 0);
                const unsigned char* v = sqlite3_column_text(stmt, 1);
                Reading r;
                r.time  = t ? reinterpret_cast<const char*>(t) : "";
                r.value = v ? reinterpret_cast<const char*>(v) : "";
                result.readings.push_back(r);
            }
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    return result;
}

// ---------------------------------------------------------------------------
// JSON response builders
// ---------------------------------------------------------------------------

std::string build_success_json(const std::string& sensor_name, const std::string& sensor_id,
                                const std::string& date, const std::vector<Reading>& readings) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"sensor_name\": \"" << json_escape(sensor_name) << "\",\n";
    out << "  \"sensor_id\": \"" << json_escape(sensor_id) << "\",\n";
    out << "  \"date\": \"" << json_escape(date) << "\",\n";
    out << "  \"values\": [";
    for (size_t i = 0; i < readings.size(); i++) {
        out << (i == 0 ? "\n" : ",\n");
        out << "    { \"time\": \"" << json_escape(readings[i].time)
            << "\", \"value\": \"" << json_escape(readings[i].value) << "\" }";
    }
    out << (readings.empty() ? "]\n" : "\n  ]\n");
    out << "}";
    return out.str();
}

std::string build_message_json(const std::string& sensor_name, const std::string& sensor_id,
                                const std::string& date, const std::string& message) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"sensor_name\": \"" << json_escape(sensor_name) << "\",\n";
    out << "  \"sensor_id\": \"" << json_escape(sensor_id) << "\",\n";
    out << "  \"date\": \"" << json_escape(date) << "\",\n";
    out << "  \"values\": [],\n";
    out << "  \"message\": \"" << json_escape(message) << "\"\n";
    out << "}";
    return out.str();
}

std::string build_error_json(const std::string& error) {
    std::ostringstream out;
    out << "{ \"error\": \"" << json_escape(error) << "\" }";
    return out.str();
}

// ---------------------------------------------------------------------------
// HTTP handling
// ---------------------------------------------------------------------------

// Carries the DB path into the Mongoose event handler via fn_data, so the
// handler needs no globals of its own.
struct ApiContext {
    std::string db_path;
};

void handle_logs_request(struct mg_connection* c, struct mg_http_message* hm, const std::string& db_path) {
    std::string sensor_id   = get_query_var(hm, "sensor_id");
    std::string sensor_type = get_query_var(hm, "sensor_type");
    std::string date        = get_query_var(hm, "date");

    if (sensor_id.empty() || sensor_type.empty() || date.empty()) {
        std::string body = build_error_json(
            "Missing required parameter(s). Expected sensor_id, sensor_type and date.");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    if (!is_valid_sensor_id(sensor_id)) {
        std::string body = build_error_json("Invalid sensor_id. It must be numeric.");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    if (!is_valid_sensor_type(sensor_type)) {
        std::string body = build_error_json("Invalid sensor_type. It must be alphanumeric (e.g. 'temperature', 'co2').");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    if (!is_valid_date(date)) {
        std::string body = build_error_json("Invalid date. Expected format is YYYY-MM-DD.");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    QueryResult result = query_sensor_logs(db_path, sensor_id, sensor_type, date);

    if (!result.db_open_ok) {
        std::string body = build_error_json("Unable to open the sensor database on this node.");
        mg_http_reply(c, 500, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    if (result.sensor_exists) {
        if (result.readings.empty()) {
            std::string body = build_message_json(
                result.actual_sensor_name, sensor_id, date,
                "No recorded values found for sensor '" + result.actual_sensor_name +
                "' (id=" + sensor_id + ") on " + date + ".");
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
            return;
        }
        std::string body = build_success_json(result.actual_sensor_name, sensor_id, date, result.readings);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    // Not in this node's own DB -- fall back to the slaves, the same way
    // main.cpp's resolve_sensor_record() falls back to MQTT for the SNMP
    // path, so one call to master:8000/api/logs can answer for any sensor
    // in the cluster, not just the ones stored in master.db.
    std::string corr_id = "logs_corr_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000);
    std::string slave_response = fetch_logs_from_slaves_mqtt(sensor_id, sensor_type, date, corr_id);
    std::string status = extract_json_field(slave_response, "status");

    if (status != "FOUND") {
        std::string body = build_message_json(
            "", sensor_id, date,
            "No sensor found with sensor_id=" + sensor_id + " and sensor_type=" + sensor_type +
            " anywhere in the cluster.");
        mg_http_reply(c, 404, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    std::string slave_sensor_name = extract_json_field(slave_response, "sensor_name");
    std::vector<Reading> slave_readings = parse_values_string(extract_json_field(slave_response, "values"));

    if (slave_readings.empty()) {
        std::string body = build_message_json(
            slave_sensor_name, sensor_id, date,
            "No recorded values found for sensor '" + slave_sensor_name +
            "' (id=" + sensor_id + ") on " + date + ".");
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
        return;
    }

    std::string body = build_success_json(slave_sensor_name, sensor_id, date, slave_readings);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
}

void handle_health_request(struct mg_connection* c) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s",
                  "{ \"status\": \"ok\", \"service\": \"master-sensor-log-api\" }");
}

void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;
        ApiContext* ctx = (ApiContext*)c->fn_data;

        if (mg_match(hm->uri, mg_str("/api/logs"), NULL)) {
            handle_logs_request(c, hm, ctx->db_path);
        } else if (mg_match(hm->uri, mg_str("/api/health"), NULL)) {
            handle_health_request(c);
        } else {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                          "%s", "{ \"error\": \"Unknown endpoint. Use /api/logs\" }");
        }
    }
}

}  // namespace

void run_api_server(const std::string& db_path, int port) {
    ApiContext ctx;
    ctx.db_path = db_path;

    std::string listen_url = "http://0.0.0.0:" + std::to_string(port);

    std::cout << "[API] Sensor Log API starting on " << listen_url << std::endl;
    std::cout << "[API] Reading from DB_PATH = " << db_path << std::endl;
    std::cout << "[API] Endpoint: GET /api/logs?sensor_id=<id>&sensor_type=<type>&date=YYYY-MM-DD" << std::endl;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    struct mg_connection* c = mg_http_listen(&mgr, listen_url.c_str(), ev_handler, &ctx);
    if (c == NULL) {
        std::cerr << "[API][FATAL] Could not bind to " << listen_url << std::endl;
        mg_mgr_free(&mgr);
        return;
    }

    while (g_running) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    std::cout << "[API] Sensor Log API stopped." << std::endl;
}
