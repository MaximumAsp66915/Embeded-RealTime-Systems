/*
 * main.c — Smart Surveillance System, Step 3 (Web Server)
 *
 * Starts two listeners:
 *   - redirect_server: plain HTTP on http_port, 301s everything to HTTPS
 *   - https_server:    TLS on https_port, serves the actual dashboard
 *
 * Usage:
 *   ./surveillance_web [path/to/server.conf]
 * (defaults to "server.conf" in the current directory if omitted — the
 * systemd unit sets WorkingDirectory so this "just works" there)
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "config.h"
#include "redirect_server.h"
#include "https_server.h"

int main(int argc, char *argv[]) {
    const char *config_path = (argc > 1) ? argv[1] : "server.conf";

    server_config_t cfg;
    if (config_load(config_path, &cfg) != 0) {
        fprintf(stderr, "[main] failed to load config from %s\n", config_path);
        return EXIT_FAILURE;
    }

    fprintf(stdout, "[main] Smart Surveillance web server starting\n");
    fprintf(stdout, "[main]   student: %s (%s)\n", cfg.student_name, cfg.student_id);
    fprintf(stdout, "[main]   http_port=%d https_port=%d\n", cfg.http_port, cfg.https_port);
    fprintf(stdout, "[main]   shared_dir=%s\n", cfg.shared_dir);

    pthread_t redirect_tid;
    if (pthread_create(&redirect_tid, NULL, redirect_server_run, &cfg) != 0) {
        fprintf(stderr, "[main] failed to start redirect server thread\n");
        return EXIT_FAILURE;
    }

    /* Run the HTTPS server on the main thread — if it exits (e.g. TLS
     * setup failure), the whole process exits and systemd restarts it. */
    https_server_run(&cfg);

    fprintf(stderr, "[main] https_server_run returned unexpectedly, exiting\n");
    return EXIT_FAILURE;
}
