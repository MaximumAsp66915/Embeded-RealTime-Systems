/*
 * api_router.c — see api_router.h
 */

#define _POSIX_C_SOURCE 200809L /* for gmtime_r */

#include "api_router.h"
#include "mjpeg_stream.h"
#include "sysinfo.h"
#include "persons_reader.h"
#include "history_log.h"
#include "command_dispatch.h"
#include "service_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Same adapter pattern as https_server.c's ssl_write_adapter — mjpeg_stream
 * only needs a plain function pointer, not any particular struct, so a
 * small local copy here is simpler than sharing one across translation
 * units for five lines of code. */
static long ssl_write_adapter(void *conn, const void *buf, size_t len) {
    SSL *ssl = (SSL *)conn;
    int n = SSL_write(ssl, buf, (int)len);
    if (n <= 0) return -1;
    return n;
}

static void send_json(SSL *ssl, int status_code, const char *status_text,
                       const char *body, int body_len) {
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, body_len);

    SSL_write(ssl, header, header_len);
    SSL_write(ssl, body, body_len);
}

/*
 * Escapes `src` (arbitrary text — e.g. raw journalctl output, which can
 * contain quotes, backslashes, and newlines) into `dst` as a valid JSON
 * string body (NOT including the surrounding quotes — callers wrap it
 * themselves so a truncation marker can be appended outside the escaped
 * text if needed). Silently stops before overflowing dst_size, keeping
 * the output nul-terminated. Returns the number of bytes written.
 */
static size_t json_escape_into(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  if (out + 2 < dst_size) { dst[out++] = '\\'; dst[out++] = '"';  } break;
            case '\\': if (out + 2 < dst_size) { dst[out++] = '\\'; dst[out++] = '\\'; } break;
            case '\n': if (out + 2 < dst_size) { dst[out++] = '\\'; dst[out++] = 'n';  } break;
            case '\r': if (out + 2 < dst_size) { dst[out++] = '\\'; dst[out++] = 'r';  } break;
            case '\t': if (out + 2 < dst_size) { dst[out++] = '\\'; dst[out++] = 't';  } break;
            default:
                if (c < 0x20) {
                    /* other control chars: drop rather than emit invalid JSON */
                } else {
                    dst[out++] = (char)c;
                }
        }
    }
    dst[out] = '\0';
    return out;
}

static void handle_stream(SSL *ssl, const server_config_t *cfg) {
    /* Loops internally until the client disconnects, same as /stream.mjpg */
    mjpeg_stream_serve(cfg, ssl, ssl_write_adapter);
}

#define FRAME_MAX_BYTES (2 * 1024 * 1024) /* generous cap for a single frame */

static void handle_frame(SSL *ssl, const server_config_t *cfg) {
    /* Single still JPEG — unlike /api/v1/stream, this is a normal,
     * one-shot response: easy to curl, screenshot, or open directly in
     * any image viewer/browser tab, no multipart parsing required. Reads
     * the same frame.jpg the live stream and the dashboard use. */
    FILE *f = fopen(cfg->frame_path, "rb");
    if (!f) {
        const char *body = "{\"error\": \"no frame available yet — is person_detector.py running?\"}";
        send_json(ssl, 503, "Service Unavailable", body, (int)strlen(body));
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > FRAME_MAX_BYTES) {
        fclose(f);
        const char *body = "{\"error\": \"frame file empty or unexpectedly large\"}";
        send_json(ssl, 500, "Internal Server Error", body, (int)strlen(body));
        return;
    }

    unsigned char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        const char *body = "{\"error\": \"out of memory\"}";
        send_json(ssl, 500, "Internal Server Error", body, (int)strlen(body));
        return;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read_bytes != (size_t)size) {
        free(buf);
        const char *body = "{\"error\": \"short read on frame file\"}";
        send_json(ssl, 500, "Internal Server Error", body, (int)strlen(body));
        return;
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        size);

    SSL_write(ssl, header, header_len);
    SSL_write(ssl, buf, (int)size);
    free(buf);
}

static void handle_persons(SSL *ssl, const server_config_t *cfg) {
    persons_info_t p;
    persons_reader_read(cfg->persons_path, &p);

    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"count\": %d, \"timestamp\": \"%s\"}", p.count, p.timestamp);

    send_json(ssl, 200, "OK", body, body_len);
}

static void handle_telemetry(SSL *ssl, const server_config_t *cfg) {
    (void)cfg;

    double temp_c = sysinfo_read_cpu_temp_c();

    mem_info_t mem;
    int mem_ok = (sysinfo_read_mem_info(&mem) == 0);
    double mem_free_mb = mem_ok ? mem.available_kb / 1024.0 : -1.0;
    double mem_total_mb = mem_ok ? mem.total_kb / 1024.0 : -1.0;

    double cpu_percent = sysinfo_read_cpu_percent();
    if (cpu_percent < 0.0) cpu_percent = 0.0;

    double uptime_s = sysinfo_read_uptime_seconds();

    double load1 = -1.0, load5 = -1.0, load15 = -1.0;
    sysinfo_read_loadavg(&load1, &load5, &load15); /* leaves -1.0 on failure */

    disk_info_t disk;
    int disk_ok = (sysinfo_read_disk_info("/", &disk) == 0);
    double disk_free_mb = disk_ok ? disk.free_mb : -1.0;
    double disk_total_mb = disk_ok ? disk.total_mb : -1.0;

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char body[600];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"cpu_temp_c\": %.1f, "
        "\"cpu_percent\": %.1f, "
        "\"mem_free_mb\": %.1f, "
        "\"mem_total_mb\": %.1f, "
        "\"uptime_seconds\": %.0f, "
        "\"load_avg_1m\": %.2f, "
        "\"load_avg_5m\": %.2f, "
        "\"load_avg_15m\": %.2f, "
        "\"disk_free_mb\": %.1f, "
        "\"disk_total_mb\": %.1f, "
        "\"timestamp\": \"%s\""
        "}",
        temp_c, cpu_percent, mem_free_mb, mem_total_mb, uptime_s,
        load1, load5, load15, disk_free_mb, disk_total_mb, ts);

    send_json(ssl, 200, "OK", body, body_len);
}

static void handle_history(SSL *ssl, const server_config_t *cfg) {
    (void)cfg;
    char body[1024];
    int body_len = history_log_get_json(body, sizeof(body));
    if (body_len < 0) {
        const char *err = "{\"error\": \"history buffer render failed\"}";
        send_json(ssl, 500, "Internal Server Error", err, (int)strlen(err));
        return;
    }
    send_json(ssl, 200, "OK", body, body_len);
}

static void handle_history_summary(SSL *ssl, const server_config_t *cfg) {
    (void)cfg;
    char body[256];
    int body_len = history_log_get_summary_json(body, sizeof(body));
    if (body_len < 0) {
        const char *err = "{\"error\": \"history summary render failed\"}";
        send_json(ssl, 500, "Internal Server Error", err, (int)strlen(err));
        return;
    }
    send_json(ssl, 200, "OK", body, body_len);
}

/* Extracts the string value of "cmd" from a small JSON body like
 * {"cmd": "reboot"}. Deliberately not a general JSON parser — same
 * tolerant-scanner approach as persons_reader.c, appropriate for a
 * fixed, tiny, always-the-same-shape request body. */
static int extract_cmd_field(const char *json_body, char *out, size_t out_size) {
    const char *pos = strstr(json_body, "\"cmd\"");
    if (!pos) return -1;

    pos = strchr(pos + 5, ':');
    if (!pos) return -1;
    pos++;

    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return -1;
    pos++;

    size_t i = 0;
    while (pos[i] && pos[i] != '"' && i < out_size - 1) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

static void handle_command(SSL *ssl, const server_config_t *cfg, const http_request_t *req) {
    char cmd_name[64];
    if (extract_cmd_field(req->body, cmd_name, sizeof(cmd_name)) != 0) {
        const char *err = "{\"error\": \"body must be JSON like {\\\"cmd\\\": \\\"noop\\\"}\"}";
        send_json(ssl, 400, "Bad Request", err, (int)strlen(err));
        return;
    }

    char message[256];
    cmd_result_status_t status = command_dispatch_run(cfg, cmd_name, message, sizeof(message));

    char body[512];
    int body_len;
    int http_status;
    const char *http_status_text;

    switch (status) {
        case CMD_RESULT_OK:
            http_status = 200; http_status_text = "OK";
            body_len = snprintf(body, sizeof(body),
                "{\"cmd\": \"%s\", \"status\": \"ok\", \"message\": \"%s\"}", cmd_name, message);
            break;
        case CMD_RESULT_REFUSED_DANGEROUS:
            http_status = 403; http_status_text = "Forbidden";
            body_len = snprintf(body, sizeof(body),
                "{\"cmd\": \"%s\", \"status\": \"refused\", \"message\": \"%s\"}", cmd_name, message);
            break;
        case CMD_RESULT_UNKNOWN_COMMAND:
        default:
            http_status = 404; http_status_text = "Not Found";
            body_len = snprintf(body, sizeof(body),
                "{\"cmd\": \"%s\", \"status\": \"unknown\", \"message\": \"%s\"}", cmd_name, message);
            break;
    }

    send_json(ssl, http_status, http_status_text, body, body_len);
}

static void handle_unknown_api_route(SSL *ssl) {
    const char *body = "{\"error\": \"unknown API route\"}";
    send_json(ssl, 404, "Not Found", body, (int)strlen(body));
}

/* --- /api/v1/services and /api/v1/services/{name}/{restart,logs} --- */

static void handle_services_list(SSL *ssl) {
    /* {"services": [{"name": "...", "status": "active"}, ...]} */
    char body[512];
    int pos = snprintf(body, sizeof(body), "{\"services\": [");

    for (size_t i = 0; i < SERVICE_ALLOWLIST_COUNT; i++) {
        char status[32];
        if (service_ctl_get_status(SERVICE_ALLOWLIST[i], status, sizeof(status)) != 0) {
            snprintf(status, sizeof(status), "unknown");
        }
        pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
            "%s{\"name\": \"%s\", \"status\": \"%s\"}",
            (i == 0) ? "" : ", ", SERVICE_ALLOWLIST[i], status);
    }
    pos += snprintf(body + pos, sizeof(body) - (size_t)pos, "]}");

    send_json(ssl, 200, "OK", body, pos);
}

static void handle_service_restart(SSL *ssl, const char *name) {
    char message[256];
    int rc = service_ctl_restart(name, message, sizeof(message));

    char escaped_message[400];
    json_escape_into(message, escaped_message, sizeof(escaped_message));

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"service\": \"%s\", \"status\": \"%s\", \"message\": \"%s\"}",
        name, (rc == 0) ? "ok" : "error", escaped_message);

    send_json(ssl, (rc == 0) ? 200 : 500, (rc == 0) ? "OK" : "Internal Server Error", body, body_len);
}

static void handle_service_logs(SSL *ssl, const char *name, int lines) {
    char raw_logs[8192];
    int raw_len = service_ctl_get_logs(name, lines, raw_logs, sizeof(raw_logs));

    if (raw_len < 0) {
        const char *err = "{\"error\": \"could not fetch logs (unknown service or journalctl failed)\"}";
        send_json(ssl, 404, "Not Found", err, (int)strlen(err));
        return;
    }

    char escaped_logs[8192 * 2]; /* worst case every char needs a 2-char escape */
    json_escape_into(raw_logs, escaped_logs, sizeof(escaped_logs));

    /* body buffer sized generously to hold escaped_logs plus the small
     * amount of surrounding JSON structure. Deliberately NOT `static` —
     * this server is thread-per-connection, so a static buffer here
     * would be shared/corrupted across concurrent /logs requests. ~16KB
     * is a trivial stack allocation for a pthread's default stack size. */
    char body[sizeof(escaped_logs) + 128];
    int body_len = snprintf(body, sizeof(body),
        "{\"service\": \"%s\", \"lines_requested\": %d, \"logs\": \"%s\"}",
        name, lines, escaped_logs);

    send_json(ssl, 200, "OK", body, body_len);
}

/*
 * Parses "/api/v1/services/{name}/{action}[?query]" into name/action,
 * validating name against the allowlist as it goes. Returns 1 and fills
 * name_out/action_out on a structurally valid match, 0 otherwise (caller
 * falls through to the unknown-route 404).
 */
static int parse_service_path(const char *path, char *name_out, size_t name_out_size,
                               char *action_out, size_t action_out_size) {
    const char *prefix = "/api/v1/services/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) return 0;

    const char *rest = path + prefix_len;
    const char *slash = strchr(rest, '/');
    if (!slash) return 0;

    size_t name_len = (size_t)(slash - rest);
    if (name_len == 0 || name_len >= name_out_size) return 0;
    memcpy(name_out, rest, name_len);
    name_out[name_len] = '\0';

    const char *action_start = slash + 1;
    const char *query = strchr(action_start, '?');
    size_t action_len = query ? (size_t)(query - action_start) : strlen(action_start);
    if (action_len == 0 || action_len >= action_out_size) return 0;
    memcpy(action_out, action_start, action_len);
    action_out[action_len] = '\0';

    return 1;
}

/* Extracts an integer "lines" query parameter from a path like
 * ".../logs?lines=100". Returns the parsed value, or default_value if
 * absent/malformed. */
static int parse_lines_query_param(const char *path, int default_value) {
    const char *q = strchr(path, '?');
    if (!q) return default_value;

    const char *key = strstr(q, "lines=");
    if (!key) return default_value;

    int value = atoi(key + strlen("lines="));
    return (value > 0) ? value : default_value;
}

void api_router_init(const server_config_t *cfg) {
    history_log_start(cfg);
}

int api_router_dispatch(SSL *ssl, const server_config_t *cfg, const http_request_t *req) {
    if (strncmp(req->path, "/api/v1/", 8) != 0) {
        return 0; /* not an API route, let the caller fall through */
    }

    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/stream") == 0) {
        handle_stream(ssl, cfg);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/frame.jpg") == 0) {
        handle_frame(ssl, cfg);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/persons") == 0) {
        handle_persons(ssl, cfg);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/telemetry") == 0) {
        handle_telemetry(ssl, cfg);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/history") == 0) {
        handle_history(ssl, cfg);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/history/summary") == 0) {
        handle_history_summary(ssl, cfg);
    } else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/v1/command") == 0) {
        handle_command(ssl, cfg, req);
    } else if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/v1/services") == 0) {
        handle_services_list(ssl);
    } else if (strncmp(req->path, "/api/v1/services/", 17) == 0) {
        char name[64], action[32];
        if (!parse_service_path(req->path, name, sizeof(name), action, sizeof(action))) {
            handle_unknown_api_route(ssl);
        } else if (strcmp(req->method, "POST") == 0 && strcmp(action, "restart") == 0) {
            handle_service_restart(ssl, name);
        } else if (strcmp(req->method, "GET") == 0 && strcmp(action, "logs") == 0) {
            int lines = parse_lines_query_param(req->path, 50);
            handle_service_logs(ssl, name, lines);
        } else {
            handle_unknown_api_route(ssl);
        }
    } else {
        handle_unknown_api_route(ssl);
    }

    return 1;
}
