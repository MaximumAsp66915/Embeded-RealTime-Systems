#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include "mongoose.h"

// Configuration storage
std::string g_port = "8080";
std::string g_db_path = "slave1.db";
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

// Signal handler for clean termination
void signal_handler(int signum) {
    std::cout << "\n[INFO] Termination signal (" << signum << ") received. Shutting down cleanly..." << std::endl;
    g_running = false;
}

// Database helper function to query latest reading
std::string get_latest_sensor_reading(const std::string& sensor_type, const std::string& sensor_id) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    std::string result = "NOT_FOUND";

    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
        if (db) sqlite3_close(db);
        return "DB_ERROR";
    }

    std::string sql = "SELECT r.value FROM sensors s "
                      "JOIN sensor_readings r ON r.sensor_id = s.sensor_id "
                      "WHERE s.sensor_type = ? AND s.sensor_id = ? "
                      "ORDER BY r.recorded_at DESC LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return "DB_ERROR";
    }

    sqlite3_bind_text(stmt, 1, sensor_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, sensor_id.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) {
            result = reinterpret_cast<const char*>(val);
            std::cout << "[DB_SUCCESS] Found local data for Type=" << sensor_type << ", ID=" << sensor_id << " -> Value=" << result << std::endl;
        }
    } else if (rc == SQLITE_DONE) {
        std::cout << "[DB_MISS] No records found for Type=" << sensor_type << ", ID=" << sensor_id << std::endl;
    } else {
        std::cerr << "[ERROR] Execution error: " << sqlite3_errmsg(db) << std::endl;
        result = "DB_ERROR";
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

// Mongoose event handler
static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        auto start_time = std::chrono::high_resolution_clock::now();
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        char type_buf[128] = {0};
        char id_buf[128] = {0};
        
        mg_http_get_var(&hm->query, "type", type_buf, sizeof(type_buf));
        mg_http_get_var(&hm->query, "id", id_buf, sizeof(id_buf));
        
        std::string sensor_type(type_buf);
        std::string sensor_id(id_buf);
        
        std::cout << "[HTTP_REQ] Received request for Type=[" << sensor_type << "], ID=[" << sensor_id << "]" << std::endl;
        
        if (sensor_type.empty() || sensor_id.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"BAD_REQUEST\"}\n");
            return;
        }
        
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
        
        if (db_result == "NOT_FOUND") {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"status\":\"NOT_FOUND\"}\n");
        } else if (db_result == "DB_ERROR") {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"INTERNAL_SERVER_ERROR\"}\n");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                          "{\"value\":\"%s\",\"source\":\"%s\",\"response_time_ms\":%.3f}\n", 
                          db_result.c_str(), source.c_str(), elapsed_ms);
        }
    }
}

int main() {
    const char* env_port = std::getenv("PORT");
    const char* env_db = std::getenv("DB_PATH");
    
    if (env_port) g_port = env_port;
    if (env_db) g_db_path = env_db;
    
    std::cout << "[STARTUP] Initializing Slave Node..." << std::endl;
    
    if (!g_cache.init()) {
        std::cerr << "[CRITICAL] Failed to attach standard cache layer connection namespace." << std::endl;
        return 1;
    }
    std::cout << "[STARTUP] Memcached structural connectivity established successfully." << std::endl;
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    
    std::string listen_address = "http://0.0.0.0:" + g_port;
    if (mg_http_listen(&mgr, listen_address.c_str(), fn, NULL) == NULL) {
        std::cerr << "[FATAL] Failed to bind HTTP listener on port " << g_port << std::endl;
        mg_mgr_free(&mgr);
        return 1;
    }
    
    std::cout << "[RUNNING] HTTP Listening active on port " << g_port << ". Press Ctrl+C to intercept and stop." << std::endl;
    
    while (g_running) {
        mg_mgr_poll(&mgr, 500);
    }
    
    mg_mgr_free(&mgr);
    std::cout << "[SHUTDOWN] Execution terminated gracefully." << std::endl;
    return 0;
}