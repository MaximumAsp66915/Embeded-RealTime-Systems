#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include <MQTTClient.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "api.h"
#include "cluster.h"

std::string g_db_path = "master.db";
std::string g_mqtt_broker = "tcp://192.168.56.101:1883";
bool g_running = true;
int g_snmp_port = 1161;
int g_api_port = 8000;

const std::string BASE_OID = ".1.3.6.1.4.1.9999";
std::vector<int> g_sensor_ids = {101, 102, 103, 104, 201, 202, 203, 204, 301, 302, 303, 304, 401};

const char REC_SEP = '\x1F';

struct PendingRequest {
    std::mutex mtx;
    std::condition_variable cv;
    std::string response_payload = "";
    bool resolved = false;
    int responses_received = 0;
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
    (void)signum;
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

struct SensorRecord {
    bool found = false;
    std::string type;
    std::string name;
    std::string location;
    std::string unit;
    std::string value;
    std::string source;
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

double elapsed_ms(const std::chrono::high_resolution_clock::time_point& start) {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

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

std::string fetch_from_slaves_mqtt(const std::string& s_id, const std::string& corr_id) {
    PendingRequest req;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests[corr_id] = &req;
    }

    std::string request_payload = "{\"id\":\"" + s_id + "\",\"correlation_id\":\"" + corr_id + "\"}";
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)request_payload.c_str();
    pubmsg.payloadlen = (int)request_payload.length();
    pubmsg.qos = 1;
    MQTTClient_publishMessage(g_mqtt_client, "cluster/slave/request", &pubmsg, nullptr);

    std::unique_lock<std::mutex> u_lock(req.mtx);
    req.cv.wait_for(u_lock, std::chrono::milliseconds(1500), [&req] { return req.resolved; });

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests.erase(corr_id);
    }

    return req.response_payload.empty() ? "{\"status\":\"NOT_FOUND\"}" : req.response_payload;
}

// Section 5 (api.cpp) historical-logs equivalent of fetch_from_slaves_mqtt()
// above: same request/wait/timeout pattern, different topic pair, because a
// "give me everything on this date" query doesn't fit the single-latest-
// value protocol the SNMP cascade already uses.
std::string fetch_logs_from_slaves_mqtt(const std::string& sensor_id, const std::string& sensor_type,
                                         const std::string& date, const std::string& corr_id) {
    PendingRequest req;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests[corr_id] = &req;
    }

    std::string request_payload =
        "{\"id\":\"" + sensor_id + "\",\"sensor_type\":\"" + sensor_type +
        "\",\"date\":\"" + date + "\",\"correlation_id\":\"" + corr_id + "\"}";
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)request_payload.c_str();
    pubmsg.payloadlen = (int)request_payload.length();
    pubmsg.qos = 1;
    MQTTClient_publishMessage(g_mqtt_client, "cluster/slave/logs_request", &pubmsg, nullptr);

    std::unique_lock<std::mutex> u_lock(req.mtx);
    req.cv.wait_for(u_lock, std::chrono::milliseconds(1500), [&req] { return req.resolved; });

    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending_requests.erase(corr_id);
    }

    return req.response_payload.empty() ? "{\"status\":\"NOT_FOUND\"}" : req.response_payload;
}

SensorRecord resolve_sensor_record(const std::string& s_id, const std::string& corr_id, double& response_time_ms) {
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string cached = g_cache.get(s_id);
    if (!cached.empty()) {
        SensorRecord rec = decode_record(cached);
        rec.source = "CACHE";
        response_time_ms = elapsed_ms(start_time);
        return rec;
    }

    SensorRecord rec = query_local_db(s_id);
    if (rec.found) {
        rec.source = "DB";
        g_cache.set(s_id, encode_record(rec.type, rec.name, rec.location, rec.unit, rec.value), 300);
        response_time_ms = elapsed_ms(start_time);
        return rec;
    }

    std::string slave_response = fetch_from_slaves_mqtt(s_id, corr_id);
    std::string status = extract_json_field(slave_response, "status");
    if (status != "NOT_FOUND" && status != "DB_ERROR") {
        rec.type = extract_json_field(slave_response, "sensor_type");
        rec.name = extract_json_field(slave_response, "sensor_name");
        rec.location = extract_json_field(slave_response, "location");
        rec.unit = extract_json_field(slave_response, "unit");
        rec.value = extract_json_field(slave_response, "value");
        if (!rec.value.empty()) {
            rec.found = true;
            rec.source = "MQTT";
            g_cache.set(s_id, encode_record(rec.type, rec.name, rec.location, rec.unit, rec.value), 300);
        }
    }

    response_time_ms = elapsed_ms(start_time);
    return rec;
}

std::vector<int> parse_sensor_ids(const std::string& s) {
    std::vector<int> ids;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        while (!tok.empty() && (tok.front() == ' ')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\r' || tok.back() == '\n')) tok.pop_back();
        if (!tok.empty()) ids.push_back(std::atoi(tok.c_str()));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool is_known_sensor(int id) {
    return std::find(g_sensor_ids.begin(), g_sensor_ids.end(), id) != g_sensor_ids.end();
}

bool find_next_leaf(int cur_id, int cur_field, int& next_id, int& next_field) {
    for (int id : g_sensor_ids) {
        for (int field = 1; field <= 3; field++) {
            if (id > cur_id || (id == cur_id && field > cur_field)) {
                next_id = id;
                next_field = field;
                return true;
            }
        }
    }
    return false;
}

struct OidPos {
    int id = -1;
    int field = 0;
    bool valid = true;
};

OidPos parse_oid_pos(const std::string& oid_full_in) {
    std::string oid_full = oid_full_in;
    while (!oid_full.empty() && (oid_full.back() == '\n' || oid_full.back() == '\r' || oid_full.back() == ' '))
        oid_full.pop_back();

    OidPos pos;

    if (oid_full == BASE_OID) {
        pos.id = -1;
        pos.field = 0;
        return pos;
    }

    std::string prefix = BASE_OID + ".";
    if (oid_full.rfind(prefix, 0) != 0) {
        pos.valid = false;
        return pos;
    }

    std::string sub = oid_full.substr(prefix.length());
    size_t dot = sub.find('.');
    if (dot == std::string::npos) {
        pos.id = std::atoi(sub.c_str());
        pos.field = 0;
    } else {
        pos.id = std::atoi(sub.substr(0, dot).c_str());
        pos.field = std::atoi(sub.substr(dot + 1).c_str());
    }
    return pos;
}

std::string build_response(int id, int field) {
    std::string oid = BASE_OID + "." + std::to_string(id) + "." + std::to_string(field);

    double res_time = 0.0;
    std::string corr_id = "snmp_corr_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000);
    SensorRecord rec = resolve_sensor_record(std::to_string(id), corr_id, res_time);

    if (field == 1) {
        std::string name = rec.found ? rec.name : ("UNKNOWN_SENSOR_" + std::to_string(id));
        return oid + "\nstring\n" + name;
    } else if (field == 2) {
        std::string desc;
        if (rec.found) {
            desc = rec.name + " - " + rec.type + " sensor @ " + rec.location + " (unit: " + rec.unit + ")";
        } else {
            desc = "No metadata available for sensor " + std::to_string(id);
        }
        return oid + "\nstring\n" + desc;
    } else {
        std::string val = rec.found ? rec.value : "NOT_FOUND";
        return oid + "\nstring\n" + val;
    }
}

void snmp_listener_loop() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return;

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(g_snmp_port);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        close(sockfd);
        return;
    }
    std::cout << "[SNMPD-PASS] Bridge listener active on UDP port " << g_snmp_port << std::endl;

    char buffer[2048];
    struct sockaddr_in clientaddr{};
    socklen_t addr_len = sizeof(clientaddr);

    struct timeval tv{0, 100000};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (g_running) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&clientaddr, &addr_len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string raw_req(buffer, n);

            size_t sep = raw_req.find('|');
            if (sep == std::string::npos) continue;

            std::string mode = raw_req.substr(0, sep);
            std::string oid_full = raw_req.substr(sep + 1);

            std::string out_payload;

            if (mode == "-g") {
                OidPos p = parse_oid_pos(oid_full);
                if (p.valid && p.field >= 1 && p.field <= 3 && is_known_sensor(p.id)) {
                    out_payload = build_response(p.id, p.field);
                }
            } else if (mode == "-n") {
                OidPos p = parse_oid_pos(oid_full);
                int nid = -1, nfield = 0;
                if (p.valid && find_next_leaf(p.id, p.field, nid, nfield)) {
                    out_payload = build_response(nid, nfield);
                }
            }

            if (!out_payload.empty()) {
                sendto(sockfd, out_payload.c_str(), out_payload.length(), 0, (struct sockaddr*)&clientaddr, addr_len);
            }
        }
    }
    close(sockfd);
}

void mqtt_listen_loop() {
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;

    if (MQTTClient_connect(g_mqtt_client, &conn_opts) != MQTTCLIENT_SUCCESS) {
        return;
    }

    MQTTClient_subscribe(g_mqtt_client, "cluster/slave/response", 1);
    MQTTClient_subscribe(g_mqtt_client, "cluster/slave/logs_response", 1);

    char* topicName = nullptr; int topicLen = 0; MQTTClient_message* message = nullptr;
    const int TOTAL_SLAVES = 2;

    while (g_running) {
        int rc = MQTTClient_receive(g_mqtt_client, &topicName, &topicLen, &message, 100);
        if (rc == MQTTCLIENT_SUCCESS && message != nullptr) {
            std::string topic(topicName ? topicName : "");
            std::string payload((char*)message->payload, message->payloadlen);
            std::string corr_id = extract_json_field(payload, "correlation_id");

            std::lock_guard<std::mutex> lock(g_pending_mutex);
            auto it = g_pending_requests.find(corr_id);
            if (it != g_pending_requests.end()) {
                std::lock_guard<std::mutex> req_lock(it->second->mtx);
                it->second->responses_received++;

                if (topic == "cluster/slave/logs_response") {
                    // Section 5 historical-logs cascade: status is
                    // "FOUND"/"NOT_FOUND" (no "DB_ERROR" case here).
                    std::string status = extract_json_field(payload, "status");
                    if (status == "FOUND") {
                        it->second->response_payload = payload;
                        it->second->resolved = true;
                        it->second->cv.notify_one();
                    } else if (it->second->responses_received >= TOTAL_SLAVES && !it->second->resolved) {
                        it->second->response_payload = "{\"status\":\"NOT_FOUND\"}";
                        it->second->resolved = true;
                        it->second->cv.notify_one();
                    }
                } else {
                    // Existing SNMP/latest-value cascade.
                    std::string status = extract_json_field(payload, "status");
                    if (status != "NOT_FOUND" && status != "DB_ERROR") {
                        it->second->response_payload = payload;
                        it->second->resolved = true;
                        it->second->cv.notify_one();
                    }
                    else if (it->second->responses_received >= TOTAL_SLAVES && !it->second->resolved) {
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
    const char* sids = std::getenv("SENSOR_IDS");
    const char* api_port_env = std::getenv("API_PORT");
    if (db) g_db_path = db;
    if (mq) g_mqtt_broker = mq;
    if (sids && std::string(sids).length() > 0) g_sensor_ids = parse_sensor_ids(sids);
    if (api_port_env && std::string(api_port_env).length() > 0) g_api_port = std::atoi(api_port_env);

    std::cout << "[STARTUP] Initializing Master Engine..." << std::endl;
    std::cout << "[STARTUP] Exposed SNMP sensor IDs: ";
    for (size_t i = 0; i < g_sensor_ids.size(); i++) std::cout << g_sensor_ids[i] << (i + 1 < g_sensor_ids.size() ? "," : "");
    std::cout << std::endl;
    std::cout << "[STARTUP] Sensor Log API will listen on port " << g_api_port << std::endl;

    if (!g_cache.init()) return 1;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    MQTTClient_create(&g_mqtt_client, g_mqtt_broker.c_str(), "MasterCoreNode", MQTTCLIENT_PERSISTENCE_NONE, NULL);

    std::thread mqtt_thread(mqtt_listen_loop);
    // Section 5: embedded Sensor Log HTTP API, reading from the same
    // g_db_path SQLite database the SNMP/cache/MQTT cascade already uses.
    std::thread api_thread(run_api_server, g_db_path, g_api_port);

    snmp_listener_loop();

    if (mqtt_thread.joinable()) mqtt_thread.join();
    if (api_thread.joinable()) api_thread.join();
    MQTTClient_destroy(&g_mqtt_client);
    return 0;
}
