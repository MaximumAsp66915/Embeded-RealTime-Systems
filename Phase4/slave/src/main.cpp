#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include <MQTTClient.h>

std::string g_db_path = "slave1.db";
std::string g_mqtt_broker = "tcp://127.0.0.1:1883";
bool g_running = true;

// Field separator used to flatten a sensor record into a single string for
// storage in Memcached (must match the master's encoding).
const char REC_SEP = '\x1F';

struct CacheManager {
    memcached_st* memc = nullptr;
    bool init(const std::string& host = "127.0.0.1", int port = 11211) {
        memc = memcached_create(NULL);
        if (!memc) return false;
        memcached_server_st* servers = memcached_server_list_append(NULL, host.c_str(), port, NULL);
        memcached_return rc = memcached_server_push(memc, servers);
        memcached_server_list_free(servers);
        return rc == MEMCACHED_SUCCESS;
    }
    std::string get(const std::string& key) {
        if (!memc) return "";
        size_t value_length; uint32_t flags; memcached_return rc;
        char* value = memcached_get(memc, key.c_str(), key.length(), &value_length, &flags, &rc);
        if (value && rc == MEMCACHED_SUCCESS) { std::string ret(value, value_length); free(value); return ret; }
        return "";
    }
    void set(const std::string& key, const std::string& value, time_t expiration = 300) {
        if (!memc) return;
        memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), expiration, 0);
    }
    ~CacheManager() { if (memc) memcached_free(memc); }
};

CacheManager g_cache;
MQTTClient g_mqtt_client;

void signal_handler(int signum) {
    std::cout << "\n[INFO] Termination signal (" << signum << ") received. Shutting down..." << std::endl;
    g_running = false;
}

std::string extract_json_field(const std::string& json, const std::string& field) {
    size_t pos = json.find("\"" + field + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    size_t start = json.find("\"", pos);
    if (start == std::string::npos) return "";
    size_t end = json.find("\"", start + 1);
    if (end == std::string::npos) return "";
    return json.substr(start + 1, end - start - 1);
}

// ---------------------------------------------------------------------------
// Sensor record: everything the master's SNMP layer needs (name, location,
// unit, type, value) resolved from ONE lookup by sensor_id alone.
// ---------------------------------------------------------------------------
struct SensorRecord {
    bool found = false;
    std::string type;
    std::string name;
    std::string location;
    std::string unit;
    std::string value;
};

std::string encode_record(const std::string& type, const std::string& name, const std::string& location, const std::string& unit, const std::string& value) {
    return type + REC_SEP + name + REC_SEP + location + REC_SEP + unit + REC_SEP + value;
}

SensorRecord decode_record(const std::string& encoded) {
    SensorRecord rec;
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= encoded.size(); i++) {
        if (i == encoded.size() || encoded[i] == REC_SEP) {
            parts.push_back(encoded.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() == 5) {
        rec.found = true;
        rec.type = parts[0];
        rec.name = parts[1];
        rec.location = parts[2];
        rec.unit = parts[3];
        rec.value = parts[4];
    }
    return rec;
}

// Looks a sensor up by sensor_id alone (sensor IDs are globally unique
// across the cluster, so no sensor_type needs to be known in advance).
SensorRecord query_local_db(const std::string& id) {
    SensorRecord rec;
    sqlite3* db = nullptr; sqlite3_stmt* stmt = nullptr;
    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) { if (db) sqlite3_close(db); return rec; }

    std::string sql =
        "SELECT s.sensor_type, s.sensor_name, s.location, s.unit, r.value "
        "FROM sensors s JOIN sensor_readings r ON r.sensor_id = s.sensor_id "
        "WHERE s.sensor_id = ? ORDER BY r.recorded_at DESC LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return rec; }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            const unsigned char* v = sqlite3_column_text(stmt, i);
            return v ? reinterpret_cast<const char*>(v) : "";
        };
        rec.type = col(0);
        rec.name = col(1);
        rec.location = col(2);
        rec.unit = col(3);
        rec.value = col(4);
        rec.found = true;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rec;
}

void handle_incoming_request(const std::string& payload) {
    auto start_time = std::chrono::high_resolution_clock::now();
    std::string sensor_id = extract_json_field(payload, "id");
    std::string corr_id = extract_json_field(payload, "correlation_id");

    if (sensor_id.empty()) return;

    SensorRecord rec;
    std::string source = "CACHE";

    std::string cached = g_cache.get(sensor_id);
    if (!cached.empty()) {
        rec = decode_record(cached);
    }

    if (!rec.found) {
        rec = query_local_db(sensor_id);
        source = "DB";
        if (rec.found) {
            g_cache.set(sensor_id, encode_record(rec.type, rec.name, rec.location, rec.unit, rec.value), 300);
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::string response;
    if (!rec.found) {
        response = "{\"status\":\"NOT_FOUND\",\"correlation_id\":\"" + corr_id + "\"}";
    } else {
        response =
            "{\"value\":\"" + rec.value + "\","
            "\"sensor_type\":\"" + rec.type + "\","
            "\"sensor_name\":\"" + rec.name + "\","
            "\"location\":\"" + rec.location + "\","
            "\"unit\":\"" + rec.unit + "\","
            "\"source\":\"" + source + "\","
            "\"response_time_ms\":" + std::to_string(elapsed_ms) + ","
            "\"correlation_id\":\"" + corr_id + "\"}";
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)response.c_str();
    pubmsg.payloadlen = (int)response.length();
    pubmsg.qos = 1;
    MQTTClient_publishMessage(g_mqtt_client, "cluster/slave/response", &pubmsg, nullptr);
}

int main() {
    const char* env_db = std::getenv("DB_PATH");
    const char* env_mq = std::getenv("MQTT_BROKER");
    if (env_db) g_db_path = env_db;
    if (env_mq) g_mqtt_broker = env_mq;

    std::cout << "[STARTUP] Initializing Pure MQTT Slave..." << std::endl;
    if (!g_cache.init()) return 1;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string client_id = "SlaveNode_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000);
    MQTTClient_create(&g_mqtt_client, g_mqtt_broker.c_str(), client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;

    if (MQTTClient_connect(g_mqtt_client, &conn_opts) != MQTTCLIENT_SUCCESS) {
        MQTTClient_destroy(&g_mqtt_client);
        return 1;
    }

    MQTTClient_subscribe(g_mqtt_client, "cluster/slave/request", 1);
    char* topicName = nullptr; int topicLen = 0; MQTTClient_message* message = nullptr;

    while (g_running) {
        int rc = MQTTClient_receive(g_mqtt_client, &topicName, &topicLen, &message, 250);
        if (rc == MQTTCLIENT_SUCCESS && message != nullptr) {
            std::string payload((char*)message->payload, message->payloadlen);
            handle_incoming_request(payload);
            MQTTClient_freeMessage(&message);
            MQTTClient_free(topicName);
        }
    }
    MQTTClient_disconnect(g_mqtt_client, 1000);
    MQTTClient_destroy(&g_mqtt_client);
    return 0;
}