/*
 * https_server.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * The main dashboard server: TLS-only (see redirect_server.c for the plain
 * HTTP -> HTTPS redirect on the other port), serving:
 *   GET /            -> dashboard HTML
 *   GET /stream.mjpg -> MJPEG live video (see mjpeg_stream.c)
 *   GET /stats.json  -> {persons, temp_c, mem_free_mb, cpu_percent, ...}
 */

#ifndef HTTPS_SERVER_H
#define HTTPS_SERVER_H

#include "config.h"

/*
 * Runs forever (intended to be called from main() directly, or from its
 * own thread if you want main() to do something else afterward). Only
 * returns if the listening socket or the TLS context could not be set up.
 */
void *https_server_run(void *arg /* server_config_t* */);

#endif /* HTTPS_SERVER_H */
