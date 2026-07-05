#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <sqlite3.h>
#include "mongoose.h"

// Configuration storage
std::string g_port = "8080";
std::string g_db_path = "slave1.db";
bool g_running = true;

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

    // Parameterized SQL query ensuring structural safety
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
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        char type_buf[128] = {0};
        char id_buf[128] = {0};
        
        mg_http_get_var(&hm->query, "type", type_buf, sizeof(type_buf));
        mg_http_get_var(&hm->query, "id", id_buf, sizeof(id_buf));
        
        std::string sensor_type(type_buf);
        std::string sensor_id(id_buf);
        
        std::cout << "[HTTP_REQ] Received request for Type=[" << sensor_type << "], ID=[" << sensor_id << "]" << std::endl;
        
        if (sensor_type.empty() || sensor_id.empty()) {
            mg_http_reply(c, 400, "Content-Type: text/plain\r\n", "BAD_REQUEST: Missing parameters 'type' or 'id'\n");
            return;
        }
        
        std::string db_result = get_latest_sensor_reading(sensor_type, sensor_id);
        
        if (db_result == "NOT_FOUND") {
            mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "NOT_FOUND\n");
        } else if (db_result == "DB_ERROR") {
            mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "INTERNAL_SERVER_ERROR\n");
        } else {
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s\n", db_result.c_str());
        }
    }
}

int main() {
    const char* env_port = std::getenv("PORT");
    const char* env_db = std::getenv("DB_PATH");
    
    if (env_port) g_port = env_port;
    if (env_db) g_db_path = env_db;
    
    std::cout << "[STARTUP] Initializing Slave Node..." << std::endl;
    std::cout << "[STARTUP] Target Database: " << g_db_path << std::endl;
    std::cout << "[STARTUP] Binding Port: " << g_port << std::endl;
    
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