#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include <MQTTClient.h>

std::string g_db_path = "slave1.db";
std::string g_mqtt_broker = "tcp://127.0.0.1:1883";
bool g_running = true;

// Memcached Wrapper
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
        size_t value_length;
        uint32_t flags;
        memcached_return rc;
        char* value = memcached_get(memc, key.c_str(), key.length(), &value_length, &flags, &rc);
        if (value && rc == MEMCACHED_SUCCESS) {
            std::string ret(value, value_length);
            free(value);
            return ret;
        }
        return "";
    }

    void set(const std::string& key, const std::string& value, time_t expiration = 300) {
        if (!memc) return;
        memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), expiration, 0);
    }

    ~CacheManager() {
        if (memc) memcached_free(memc);
    }
};

CacheManager g_cache;
MQTTClient g_mqtt_client;

void signal_handler(int signum) {
    std::cout << "\n[INFO] Termination signal (" << signum << ") received. Shutting down cleanly..." << std::endl;
    g_running = false;
}

// Safely extract fields from incoming JSON payloads
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

// Check local database
std::string get_latest_sensor_reading(const std::string& sensor_type, const std::string& sensor_id) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    std::string result = "NOT_FOUND";

    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return "DB_ERROR";
    }

    std::string sql = "SELECT r.value FROM sensors s "
                      "JOIN sensor_readings r ON r.sensor_id = s.sensor_id "
                      "WHERE s.sensor_type = ? AND s.sensor_id = ? "
                      "ORDER BY r.recorded_at DESC LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return "DB_ERROR";
    }

    sqlite3_bind_text(stmt, 1, sensor_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, sensor_id.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) result = reinterpret_cast<const char*>(val);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

// Handle requests and publish findings back to the Master
void handle_incoming_request(const std::string& payload) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::string sensor_type = extract_json_field(payload, "type");
    std::string sensor_id = extract_json_field(payload, "id");
    std::string corr_id = extract_json_field(payload, "correlation_id");

    if (sensor_type.empty() || sensor_id.empty()) return;

    std::string cache_key = sensor_type + ":" + sensor_id;
    std::string db_result = g_cache.get(cache_key);
    std::string source = "CACHE";

    if (db_result.empty()) {
        db_result = get_latest_sensor_reading(sensor_type, sensor_id);
        source = "DB";
        if (db_result != "NOT_FOUND" && db_result != "DB_ERROR") {
            g_cache.set(cache_key, db_result, 300);
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::string response;
    char buffer[512];
    if (db_result == "NOT_FOUND") {
        snprintf(buffer, sizeof(buffer), "{\"status\":\"NOT_FOUND\",\"correlation_id\":\"%s\"}", corr_id.c_str());
        response = buffer;
    } else if (db_result == "DB_ERROR") {
        snprintf(buffer, sizeof(buffer), "{\"status\":\"DB_ERROR\",\"correlation_id\":\"%s\"}", corr_id.c_str());
        response = buffer;
    } else {
        snprintf(buffer, sizeof(buffer), "{\"value\":\"%s\",\"source\":\"%s\",\"response_time_ms\":%.3f,\"correlation_id\":\"%s\"}", 
                 db_result.c_str(), source.c_str(), elapsed_ms, corr_id.c_str());
        response = buffer;
    }

    // Return the response back to Master
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)response.c_str();
    pubmsg.payloadlen = (int)response.length();
    pubmsg.qos = 1;
    pubmsg.retained = 0;
    MQTTClient_publishMessage(g_mqtt_client, "cluster/slave/response", &pubmsg, nullptr);
}

int main() {
    const char* env_db = std::getenv("DB_PATH");
    const char* env_mq = std::getenv("MQTT_BROKER");
    
    if (env_db) g_db_path = env_db;
    if (env_mq) g_mqtt_broker = env_mq;
    
    std::cout << "[STARTUP] Initializing Pure MQTT Slave..." << std::endl;
    
    if (!g_cache.init()) {
        std::cerr << "[CRITICAL] Failed to attach standard cache layer connection namespace." << std::endl;
        return 1;
    }
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Dynamic identifier to distinguish nodes connected on same broker broker
    std::string client_id = "SlaveNode_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000);

    MQTTClient_create(&g_mqtt_client, g_mqtt_broker.c_str(), client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.MQTTVersion = MQTTVERSION_3_1_1;

    if (MQTTClient_connect(g_mqtt_client, &conn_opts) != MQTTCLIENT_SUCCESS) {
        std::cerr << "[CRITICAL] Connecting to MQTT broker failed." << std::endl;
        MQTTClient_destroy(&g_mqtt_client);
        return 1;
    }

    MQTTClient_subscribe(g_mqtt_client, "cluster/slave/request", 1);
    std::cout << "[RUNNING] Subscribed to topic 'cluster/slave/request'. Monitoring events..." << std::endl;

    char* topicName = nullptr;
    int topicLen = 0;
    MQTTClient_message* message = nullptr;

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
    std::cout << "[SHUTDOWN] Execution terminated gracefully." << std::endl;
    return 0;
}