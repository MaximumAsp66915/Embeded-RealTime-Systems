/*
 * redirect_server.c — see redirect_server.h
 */

#include "redirect_server.h"
#include "http_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[redirect] socket");
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
        perror("[redirect] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("[redirect] listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void handle_client(int client_fd, int https_port) {
    char buf[2048];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    http_request_t req;
    http_parse_request(buf, &req);

    /* Fall back to a sane default if the client didn't send a Host header
     * (rare, but shouldn't crash the redirect). */
    const char *host = (req.host[0] != '\0') ? req.host : "localhost";

    /* Strip any port the client's Host header may already carry
     * (e.g. "192.168.0.170:80") since we're appending the HTTPS port. */
    char host_only[HTTP_HOST_MAXLEN];
    snprintf(host_only, sizeof(host_only), "%s", host);
    char *colon = strchr(host_only, ':');
    if (colon) *colon = '\0';

    char location[512];
    if (https_port == 443) {
        snprintf(location, sizeof(location), "https://%s/", host_only);
    } else {
        snprintf(location, sizeof(location), "https://%s:%d/", host_only, https_port);
    }

    char response[768];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        location);

    ssize_t written = write(client_fd, response, (size_t)len);
    (void)written; /* best-effort; if this fails the client just sees a closed connection */
    close(client_fd);
}

void *redirect_server_run(void *arg) {
    server_config_t *cfg = (server_config_t *)arg;

    int listen_fd = make_listen_socket(cfg->http_port);
    if (listen_fd < 0) {
        fprintf(stderr, "[redirect] failed to start listener on port %d\n", cfg->http_port);
        return NULL;
    }

    fprintf(stdout, "[redirect] listening on :%d -> redirecting to https\n", cfg->http_port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("[redirect] accept");
            continue;
        }
        handle_client(client_fd, cfg->https_port);
    }

    /* unreachable */
    close(listen_fd);
    return NULL;
}
