#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include <MQTTClient.h>

std::string g_db_path = "master.db";
std::string g_mqtt_broker = "tcp://127.0.0.1:1883";
bool g_running = true;

struct PendingRequest {
    std::mutex mtx;
    std::condition_variable cv;
    std::string response_payload = "";
    bool resolved = false;
    int responses_received = 0; // Track how many slaves checked in
};

std::mutex g_pending_mutex;
std::unordered_map<std::string, PendingRequest*> g_pending_requests;

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
    std::cout << "\n[INFO] Termination signal (" << signum << ") received. Shutting down cleanly..." << std::endl;
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

std::string check_local_db(const std::string& type, const std::string& id) {
    sqlite3* db = nullptr; sqlite3_stmt* stmt = nullptr; std::string result = "NOT_FOUND";
    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) { if (db) sqlite3_close(db); return "DB_ERROR"; }
    std::string sql = "SELECT r.value FROM sensors s JOIN sensor_readings r ON r.sensor_id = s.sensor_id WHERE s.sensor_type = ? AND s.sensor_id = ? ORDER BY r.recorded_at DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { sqlite3_close(db); return "DB_ERROR"; }
    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) { const unsigned char* val = sqlite3_column_text(stmt, 0); if (val) result = reinterpret_cast<const char*>(val); }
    sqlite3_finalize(stmt); sqlite3_close(db);
    return result;
}

std::string fetch_from_slaves_mqtt(const std::string& type, const std::string& id, const std::string& corr_id) {
    PendingRequest req;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests[corr_id] = &req;
    }

    std::string request_payload = "{\"type\":\"" + type + "\",\"id\":\"" + id + "\",\"correlation_id\":\"" + corr_id + "\"}";
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)request_payload.c_str();
    pubmsg.payloadlen = (int)request_payload.length();
    pubmsg.qos = 1;
    MQTTClient_publishMessage(g_mqtt_client, "cluster/slave/request", &pubmsg, nullptr);

    std::unique_lock<std::mutex> u_lock(req.mtx);
    // Timeout or get answer from cluster nodes
    req.cv.wait_for(u_lock, std::chrono::milliseconds(1500), [&req] { return req.resolved; });

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests.erase(corr_id);
    }

    return req.response_payload.empty() ? "{\"status\":\"NOT_FOUND\"}" : req.response_payload;
}

std::string resolve_sensor_request(const std::string& s_type, const std::string& s_id, const std::string& corr_id, double& response_time_ms) {
    auto start_time = std::chrono::high_resolution_clock::now();
    std::string cache_key = s_type + ":" + s_id;
    
    std::string lookup = g_cache.get(cache_key);
    if (!lookup.empty()) {
        auto end_time = std::chrono::high_resolution_clock::now();
        response_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "{\"value\":\"%s\",\"source\":\"CACHE\",\"response_time_ms\":%.3f}", lookup.c_str(), response_time_ms);
        return std::string(buffer);
    }

    lookup = check_local_db(s_type, s_id);
    if (lookup != "NOT_FOUND" && lookup != "DB_ERROR") {
        g_cache.set(cache_key, lookup, 300);
        auto end_time = std::chrono::high_resolution_clock::now();
        response_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "{\"value\":\"%s\",\"source\":\"DB\",\"response_time_ms\":%.3f}", lookup.c_str(), response_time_ms);
        return std::string(buffer);
    }

    std::string slave_response = fetch_from_slaves_mqtt(s_type, s_id, corr_id);
    if (slave_response.find("NOT_FOUND") == std::string::npos && slave_response.find("DB_ERROR") == std::string::npos) {
        std::string value_extracted = extract_json_field(slave_response, "value");
        if (!value_extracted.empty()) {
            g_cache.set(cache_key, value_extracted, 300);
        }
        return slave_response;
    }

    return "{\"status\":\"NOT_FOUND\"}";
}

// Separate asynchronous worker execution thread for each client query request
void async_worker_executor(std::string payload) {
    std::string type = extract_json_field(payload, "type");
    std::string id = extract_json_field(payload, "id");
    std::string corr_id = extract_json_field(payload, "correlation_id");

    if (!type.empty() && !id.empty()) {
        double res_time = 0.0;
        std::string response = resolve_sensor_request(type, id, corr_id, res_time);
        
        if (response.back() == '}') response.pop_back();
        response += ",\"correlation_id\":\"" + corr_id + "\"}";

        MQTTClient_message pubmsg = MQTTClient_message_initializer;
        pubmsg.payload = (void*)response.c_str();
        pubmsg.payloadlen = (int)response.length();
        pubmsg.qos = 1;
        MQTTClient_publishMessage(g_mqtt_client, "sensor/response", &pubmsg, nullptr);
    }
}

void mqtt_listen_loop() {
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.MQTTVersion = MQTTVERSION_3_1_1;

    if (MQTTClient_connect(g_mqtt_client, &conn_opts) != MQTTCLIENT_SUCCESS) {
        std::cerr << "[MQTT_ERROR] Connection failure to " << g_mqtt_broker << std::endl;
        return;
    }
    
    MQTTClient_subscribe(g_mqtt_client, "sensor/request", 1);
    MQTTClient_subscribe(g_mqtt_client, "cluster/slave/response", 1);
    std::cout << "[MQTT] Core Subscriptions Active." << std::endl;

    char* topicName = nullptr; int topicLen = 0; MQTTClient_message* message = nullptr;

    while (g_running) {
        int rc = MQTTClient_receive(g_mqtt_client, &topicName, &topicLen, &message, 100);
        if (rc == MQTTCLIENT_SUCCESS && message != nullptr) {
            std::string topic(topicName, topicLen == 0 ? strlen(topicName) : topicLen);
            std::string payload((char*)message->payload, message->payloadlen);
            
            if (topic == "sensor/request") {
                // Hand the query job context over to a detached thread! Keep listening loop open!
                std::thread(async_worker_executor, payload).detach();
            } 
            else if (topic == "cluster/slave/response") {
                std::string corr_id = extract_json_field(payload, "correlation_id");
                std::string status = extract_json_field(payload, "status");
                
                std::lock_guard<std::mutex> lock(g_pending_mutex);
                auto it = g_pending_requests.find(corr_id);
                if (it != g_pending_requests.end()) {
                    std::lock_guard<std::mutex> req_lock(it->second->mtx);
                    it->second->responses_received++;

                    if (status != "NOT_FOUND") {
                        it->second->response_payload = payload;
                        it->second->resolved = true;
                        it->second->cv.notify_one();
                    } 
                    // Stop waiting if both available slave nodes check back negative
                    else if (it->second->responses_received >= 2) {
                        it->second->response_payload = "{\"status\":\"NOT_FOUND\"}";
                        it->second->resolved = true;
                        it->second->cv.notify_one();
                    }
                }
            }
            MQTTClient_freeMessage(&message);
            MQTTClient_free(topicName);
        }
    }
    MQTTClient_disconnect(g_mqtt_client, 1000);
}

int main() {
    const char* db = std::getenv("DB_PATH");
    const char* mq = std::getenv("MQTT_BROKER");
    
    if (db) {
        g_db_path = db;
    }
    if (mq) {
        g_mqtt_broker = mq;
    }
    
    std::cout << "[STARTUP] Initializing Non-Blocking Multi-Threaded Master..." << std::endl;
    if (!g_cache.init()) return 1;
              
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    MQTTClient_create(&g_mqtt_client, g_mqtt_broker.c_str(), "MasterCoreNode", MQTTCLIENT_PERSISTENCE_NONE, NULL);
    mqtt_listen_loop();
    
    MQTTClient_destroy(&g_mqtt_client);
    return 0;
}