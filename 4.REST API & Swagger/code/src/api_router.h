/*
 * api_router.h — Smart Surveillance System, Step 4 (REST API)
 *
 * All /api/v1 routes live here, kept separate from https_server.c's
 * dashboard routes (/, /stream.mjpg, /stats.json — Step 3) so each file
 * stays focused on one concern.
 *
 *   GET  /api/v1/stream     -> MJPEG live video (same feed as /stream.mjpg)
 *   GET  /api/v1/frame.jpg  -> single still JPEG (one-shot, easy to curl/screenshot)
 *   GET  /api/v1/persons    -> {"count": int, "timestamp": "..."}
 *   GET  /api/v1/telemetry  -> {"cpu_temp_c", "cpu_percent", "mem_free_mb",
 *                                "mem_total_mb", "timestamp"}
 *   POST /api/v1/command    -> body {"cmd": "..."} , see command_dispatch.h
 *   GET  /api/v1/history    -> last up to 5 detection records, see history_log.h
 */

#ifndef API_ROUTER_H
#define API_ROUTER_H

#include <openssl/ssl.h>
#include "config.h"
#include "http_utils.h"

/* Starts any background work the API needs (currently: the history
 * sampler). Call once at server startup, before serving requests. */
void api_router_init(const server_config_t *cfg);

/*
 * If req->path starts with "/api/v1/", handles it fully (writes an HTTP
 * response on `ssl`, including a JSON 404 for an unrecognized route under
 * that prefix) and returns 1. If req->path does NOT start with
 * "/api/v1/", writes nothing and returns 0 so the caller can fall through
 * to its own routing.
 */
int api_router_dispatch(SSL *ssl, const server_config_t *cfg, const http_request_t *req);

#endif /* API_ROUTER_H */
