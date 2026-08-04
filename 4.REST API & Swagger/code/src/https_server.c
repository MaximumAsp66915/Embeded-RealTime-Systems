/*
 * https_server.c — see https_server.h
 *
 * One thread per connection (detached pthreads). That's plenty for a
 * coursework-scale project (a handful of browser tabs + curl during
 * experiments) and keeps the code far simpler than an event loop.
 */

#include "https_server.h"
#include "http_utils.h"
#include "index_page.h"
#include "mjpeg_stream.h"
#include "sysinfo.h"
#include "persons_reader.h"
#include "api_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

typedef struct {
    SSL *ssl;
    int client_fd;
    const server_config_t *cfg;
} client_ctx_t;

/* Adapts SSL_write to the conn_write_fn signature mjpeg_stream.c expects,
 * so the same streaming code works over both plain and TLS sockets. */
static long ssl_write_adapter(void *conn, const void *buf, size_t len) {
    SSL *ssl = (SSL *)conn;
    int n = SSL_write(ssl, buf, (int)len);
    if (n <= 0) return -1;
    return n;
}

static void send_simple_response(SSL *ssl, int status_code, const char *status_text,
                                  const char *content_type, const char *body) {
    size_t body_len = strlen(body);
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);

    SSL_write(ssl, header, header_len);
    if (body_len > 0) {
        SSL_write(ssl, body, (int)body_len);
    }
}

static void handle_index(SSL *ssl, const server_config_t *cfg) {
    /* index_page_render can produce a few KB (CSS+JS inline) */
    char *html = malloc(16384);
    if (!html) {
        send_simple_response(ssl, 500, "Internal Server Error", "text/plain", "out of memory");
        return;
    }
    int n = index_page_render(cfg, html, 16384);
    if (n < 0) {
        send_simple_response(ssl, 500, "Internal Server Error", "text/plain", "page too large for buffer");
    } else {
        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n",
            n);
        SSL_write(ssl, header, header_len);
        SSL_write(ssl, html, n);
    }
    free(html);
}

static void handle_stats(SSL *ssl, const server_config_t *cfg) {
    double temp_c = sysinfo_read_cpu_temp_c();

    mem_info_t mem;
    int mem_ok = (sysinfo_read_mem_info(&mem) == 0);
    double mem_free_mb = mem_ok ? mem.available_kb / 1024.0 : 0.0;
    double mem_total_mb = mem_ok ? mem.total_kb / 1024.0 : 0.0;

    double cpu_percent = sysinfo_read_cpu_percent();
    if (cpu_percent < 0.0) cpu_percent = 0.0;

    persons_info_t persons;
    persons_reader_read(cfg->persons_path, &persons);

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"persons\": %d, "
        "\"temp_c\": %.1f, "
        "\"mem_free_mb\": %.1f, "
        "\"mem_total_mb\": %.1f, "
        "\"cpu_percent\": %.1f, "
        "\"detector_fps\": %.1f, "
        "\"timestamp\": \"%s\""
        "}",
        persons.count, temp_c, mem_free_mb, mem_total_mb,
        cpu_percent, persons.fps, persons.timestamp);

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    SSL_write(ssl, header, header_len);
    SSL_write(ssl, body, body_len);
}

static void handle_not_found(SSL *ssl) {
    send_simple_response(ssl, 404, "Not Found", "text/plain", "404 not found\n");
}

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    SSL *ssl = ctx->ssl;

    if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        goto cleanup;
    }

    char buf[4096];
    int n = SSL_read(ssl, buf, sizeof(buf) - 1);
    if (n <= 0) {
        goto cleanup;
    }
    buf[n] = '\0';

    http_request_t req;
    if (http_parse_request(buf, (size_t)n, &req) != 0) {
        goto cleanup;
    }

    if (api_router_dispatch(ssl, ctx->cfg, &req)) {
        /* handled by Step 4's /api/v1 routes */
    } else if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/index.html") == 0) {
        handle_index(ssl, ctx->cfg);
    } else if (strcmp(req.path, "/stream.mjpg") == 0) {
        /* Loops internally until the client disconnects */
        mjpeg_stream_serve(ctx->cfg, ssl, ssl_write_adapter);
    } else if (strcmp(req.path, "/stats.json") == 0) {
        handle_stats(ssl, ctx->cfg);
    } else {
        handle_not_found(ssl);
    }

cleanup:
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(ctx->client_fd);
    free(ctx);
    return NULL;
}

static SSL_CTX *create_ssl_context(const server_config_t *cfg) {
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, cfg->cert_file, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[https] failed to load cert: %s\n", cfg->cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, cfg->key_file, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[https] failed to load key: %s\n", cfg->key_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "[https] private key does not match certificate\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[https] socket");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[https] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 32) < 0) {
        perror("[https] listen");
        close(fd);
        return -1;
    }

    return fd;
}

void *https_server_run(void *arg) {
    server_config_t *cfg = (server_config_t *)arg;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX *ctx = create_ssl_context(cfg);
    if (!ctx) {
        fprintf(stderr, "[https] TLS context setup failed, aborting.\n");
        return NULL;
    }

    int listen_fd = make_listen_socket(cfg->https_port);
    if (listen_fd < 0) {
        fprintf(stderr, "[https] failed to start listener on port %d\n", cfg->https_port);
        SSL_CTX_free(ctx);
        return NULL;
    }

    fprintf(stdout, "[https] listening on :%d (TLS)\n", cfg->https_port);

    api_router_init(cfg);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("[https] accept");
            continue;
        }

        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client_fd);

        client_ctx_t *client_ctx = malloc(sizeof(client_ctx_t));
        client_ctx->ssl = ssl;
        client_ctx->client_fd = client_fd;
        client_ctx->cfg = cfg;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, client_ctx) != 0) {
            perror("[https] pthread_create");
            SSL_free(ssl);
            close(client_fd);
            free(client_ctx);
            continue;
        }
        pthread_detach(tid);
    }

    /* unreachable */
    close(listen_fd);
    SSL_CTX_free(ctx);
    return NULL;
}
