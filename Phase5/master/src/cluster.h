#pragma once
#include <string>

// ==============================================================================
// Shared cluster-messaging helpers.
//
// main.cpp owns the MQTT connection, the pending-request table, and the
// existing "latest value" cascade (cluster/slave/request|response). api.cpp
// (Section 5) needs to reach the slaves too -- for a *historical, date-scoped*
// query instead of "latest value" -- so it reuses the same MQTT client and
// pending-request machinery through this one function, without needing
// direct access to main.cpp's internals (mutex, map, MQTTClient handle all
// stay private to main.cpp).
// ==============================================================================

// Extracts a simple quoted-string field's value from a flat JSON payload,
// e.g. extract_json_field({"status":"FOUND"}, "status") == "FOUND".
// Defined once in main.cpp; declared here so api.cpp can reuse it instead
// of duplicating the same parsing logic.
std::string extract_json_field(const std::string& json, const std::string& field);

// Publishes a historical-logs request to all slaves on
// "cluster/slave/logs_request" and blocks (with a timeout) for a matching
// reply on "cluster/slave/logs_response", correlated by corr_id -- the same
// request/response/timeout pattern main.cpp already uses for single-value
// SNMP lookups, just against a different pair of topics.
//
// Response payload shape (see slave/src/main.cpp):
//   found:     {"status":"FOUND","sensor_id":"201","sensor_name":"...",
//               "values":"HH:MM:SS|value;HH:MM:SS|value","correlation_id":"..."}
//   not found: {"status":"NOT_FOUND","correlation_id":"..."}
//
// "values" is ';'-separated "time|value" pairs (matching the project's
// existing manual-encoding style, e.g. REC_SEP-encoded cache records)
// rather than a nested JSON array, so the caller can parse it with the
// same extract_json_field() + a plain split -- no JSON array parser needed.
std::string fetch_logs_from_slaves_mqtt(const std::string& sensor_id, const std::string& sensor_type,
                                         const std::string& date, const std::string& corr_id);
