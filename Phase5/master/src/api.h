#pragma once
#include <string>

// ==============================================================================
// Section 5 - Sensor Log Reading API, embedded inside the Master engine.
//
// run_api_server() starts a Mongoose HTTP listener on the given port and
// serves it until the process-wide g_running flag (declared in main.cpp)
// is set to false by the signal handler. It is meant to be launched on its
// own std::thread from main(), alongside the existing SNMP bridge listener
// and MQTT listener threads.
//
// It reads from the SAME SQLite database (`db_path`) that the SNMP/cache/
// MQTT cascade already uses -- no separate database, no seeding, no schema
// changes. The `sensors` and `sensor_readings` tables are expected to
// already exist and be populated, exactly as they are for Phases 1-4.
//
// Matching key is sensor_id + sensor_type + date. When a sensor isn't
// found in this node's own DB, the handler falls back to the slaves over
// MQTT (see cluster.h / fetch_logs_from_slaves_mqtt), so a request to the
// master can answer for any sensor in the cluster, not just the ones
// physically stored in master.db.
// ==============================================================================
void run_api_server(const std::string& db_path, int port);
