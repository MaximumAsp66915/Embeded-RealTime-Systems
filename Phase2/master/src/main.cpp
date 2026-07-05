#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <sqlite3.h>
#include <libmemcached/memcached.h>
#include "mongoose.h"

// Topology variables
std::string g_port = "8001";
std::string g_db_path = "master.db";
std::string g_slave1_port = "8081";
std::string g_slave2_port = "8082";
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

void signal_handler(int signum) {
    std::cout << "\n[INFO] Termination signal (" << signum << ") received. Shutting down Master cleanly..." << std::endl;
    g_running = false;
}

// Local Database Probe
std::string check_local_db(const std::string& type, const std::string& id) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    std::string result = "NOT_FOUND";

    if (sqlite3_open(g_db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "[ERROR] Master DB connection failed: " << sqlite3_errmsg(db) << std::endl;
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

    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) result = reinterpret_cast<const char*>(val);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

// Bundled payload matrix for standard internal HTTP client routing
struct ClientRequestContext {
    std::string path_query = ""; 
    bool completed = false;
    int status_code = 0;
    std::string body = "";
};

// Client HTTP Callback to handle outbound responses
static void client_cb(struct mg_connection *c, int ev, void *ev_data) {
    ClientRequestContext* ctx = static_cast<ClientRequestContext*>(c->fn_data);
    if (!ctx) return;

    if (ev == MG_EV_CONNECT) {
        mg_printf(c,
                  "GET %s HTTP/1.0\r\n"
                  "Host: 127.0.0.1\r\n"
                  "Content-Length: 0\r\n"
                  "\r\n",
                  ctx->path_query.c_str());
    } 
    else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        ctx->status_code = mg_http_status(hm); 
        ctx->body = std::string(hm->body.buf, hm->body.len);
        
        if(!ctx->body.empty() && ctx->body.back() == '\n') {
            ctx->body.pop_back();
        }
        
        ctx->completed = true;
        c->is_closing = 1;
    } 
    else if (ev == MG_EV_CLOSE) {
        ctx->completed = true;
    }
}

// Dynamic Cascading Proxy Routing Function
std::string fetch_from_slave(const std::string& port, const std::string& type, const std::string& id) {
    struct mg_mgr client_mgr;
    mg_mgr_init(&client_mgr);
    
    ClientRequestContext ctx;
    std::string connect_url = "http://127.0.0.1:" + port;
    ctx.path_query = "/query?type=" + type + "&id=" + id;
    
    struct mg_connection* conn = mg_http_connect(&client_mgr, connect_url.c_str(), client_cb, &ctx);
    if (!conn) {
        std::cerr << "[CLUSTER_WARN] Cannot instantiate connection channel to Node on port: " << port << std::endl;
        mg_mgr_free(&client_mgr);
        return "NOT_FOUND";
    }
    
    conn->fn_data = &ctx;
    
    int timeouts = 0;
    while (!ctx.completed && timeouts < 16) {
        mg_mgr_poll(&client_mgr, 50);
        timeouts++;
    }
    
    mg_mgr_free(&client_mgr);
    
    if (ctx.status_code == 200) {
        return ctx.body;
    }
    return "NOT_FOUND";
}

// Gateway Request interceptor
static void gateway_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        auto start_time = std::chrono::high_resolution_clock::now();
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        char type_buf[128] = {0};
        char id_buf[128] = {0};
        mg_http_get_var(&hm->query, "type", type_buf, sizeof(type_buf));
        mg_http_get_var(&hm->query, "id", id_buf, sizeof(id_buf));
        
        std::string s_type(type_buf);
        std::string s_id(id_buf);
        
        std::cout << "[GATEWAY_IN] Evaluation check for Type=" << s_type << ", ID=" << s_id << std::endl;
        
        if (s_type.empty() || s_id.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"BAD_REQUEST\"}\n");
            return;
        }
        
        // Cache Lookup Layer
        std::string cache_key = s_type + ":" + s_id;
        std::string lookup = g_cache.get(cache_key);
        if (!lookup.empty()) {
            auto end_time = std::chrono::high_resolution_clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
            std::cout << "[CACHE_HIT] Resolved via Master Cache Layer for Key: " << cache_key << std::endl;
            
            std::string final_val = lookup;
            
            // If the cached item is a raw downstream JSON string, extract the actual raw value
            // Look for "value":"XXXX"
            size_t val_pos = lookup.find("\"value\":\"");
            if (val_pos != std::string::npos) {
                size_t start_idx = val_pos + 9; // length of "\"value\":\""
                size_t end_idx = lookup.find("\"", start_idx);
                if (end_idx != std::string::npos) {
                    final_val = lookup.substr(start_idx, end_idx - start_idx);
                }
            }

            // Always reply with CACHE as the source and the new master-calculated turnaround time
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                          "{\"value\":\"%s\",\"source\":\"CACHE\",\"response_time_ms\":%.3f}\n", 
                          final_val.c_str(), elapsed_ms);
            return;
        }
        
        // Stage 1 Cascade: Local Database Search
        std::cout << "[CASCADE_1] Probing Master Database storage..." << std::endl;
        lookup = check_local_db(s_type, s_id);
        if (lookup != "NOT_FOUND" && lookup != "DB_ERROR") {
            g_cache.set(cache_key, lookup, 300);
            auto end_time = std::chrono::high_resolution_clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
            
            std::cout << "[CASCADE_HIT] Resolved locally inside Master storage matrix." << std::endl;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                          "{\"value\":\"%s\",\"source\":\"DB\",\"response_time_ms\":%.3f}\n", 
                          lookup.c_str(), elapsed_ms);
            return;
        }
        
        // Stage 2 Cascade: Query Slave 1
        std::cout << "[CASCADE_2] Forwarding request downstream to Slave 1 Port: " << g_slave1_port << std::endl;
        lookup = fetch_from_slave(g_slave1_port, s_type, s_id);
        if (lookup != "NOT_FOUND" && lookup != "DB_ERROR") {
            g_cache.set(cache_key, lookup, 300);
            std::cout << "[CASCADE_HIT] Resolved downstream inside Slave 1 node pipeline." << std::endl;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", lookup.c_str());
            return;
        }
        
        // Stage 3 Cascade: Query Slave 2
        std::cout << "[CASCADE_3] Forwarding request downstream to Slave 2 Port: " << g_slave2_port << std::endl;
        lookup = fetch_from_slave(g_slave2_port, s_type, s_id);
        if (lookup != "NOT_FOUND" && lookup != "DB_ERROR") {
            g_cache.set(cache_key, lookup, 300);
            std::cout << "[CASCADE_HIT] Resolved downstream inside Slave 2 node pipeline." << std::endl;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", lookup.c_str());
            return;
        }
        
        // Final fallback
        std::cout << "[CASCADE_MISS] Data missing across all topological data clusters." << std::endl;
        mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"status\":\"NOT_FOUND\"}\n");
    }
}

int main() {
    const char* p = std::getenv("PORT");
    const char* db = std::getenv("DB_PATH");
    const char* s1 = std::getenv("SLAVE1_PORT");
    const char* s2 = std::getenv("SLAVE2_PORT");
    
    if(p) g_port = p;
    if(db) g_db_path = db;
    if(s1) g_slave1_port = s1;
    if(s2) g_slave2_port = s2;
    
    std::cout << "[STARTUP] Initializing Master Orchestrator Core..." << std::endl;
    
    if (!g_cache.init()) {
        std::cerr << "[CRITICAL] Could not map operational pipeline link to Memcached daemon backend." << std::endl;
        return 1;
    }
    std::cout << "[STARTUP] Memcached structural connectivity established successfully." << std::endl;
              
    std::signal(SIGINT, signal_handler);
    
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    
    std::string master_bind = "http://0.0.0.0:" + g_port;
    if (mg_http_listen(&mgr, master_bind.c_str(), gateway_handler, NULL) == NULL) {
        std::cerr << "[FATAL] Master failed to claim binding interface port " << g_port << std::endl;
        mg_mgr_free(&mgr);
        return 1;
    }
    
    std::cout << "[RUNNING] Master Core processing incoming connections. Interrupt via Ctrl+C." << std::endl;
    while (g_running) {
        mg_mgr_poll(&mgr, 250);
    }
    
    mg_mgr_free(&mgr);
    std::cout << "[SHUTDOWN] Master Engine powered down gracefully." << std::endl;
    return 0;
}