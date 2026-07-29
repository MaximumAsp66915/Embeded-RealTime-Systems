/*
 * mjpeg_stream.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Serves the "video" as an MJPEG multipart stream: repeatedly re-reads the
 * latest annotated JPEG that Image Processing writes to shared memory, and
 * pushes it down the connection. Browsers render this natively via a plain
 * <img> tag — no video codec involved anywhere in this path.
 *
 * This module writes through a generic transport (a function pointer), so
 * it works unmodified whether the underlying connection is a plain socket
 * or a TLS one — see conn_write_fn.
 */

#ifndef MJPEG_STREAM_H
#define MJPEG_STREAM_H

#include <stddef.h>
#include "config.h"

/*
 * Write callback: implementations return the number of bytes actually
 * written, or -1 on error (used to detect a disconnected client, at which
 * point the streaming loop stops).
 */
typedef long (*conn_write_fn)(void *conn, const void *buf, size_t len);

/*
 * Streams frames from cfg->frame_path over `conn` using `write_fn`, sleeping
 * cfg->mjpeg_frame_interval_ms between frames. Returns when the client
 * disconnects (write_fn returns <= 0) — this function does not return
 * otherwise, so call it as the last thing done for a stream connection.
 */
void mjpeg_stream_serve(const server_config_t *cfg, void *conn, conn_write_fn write_fn);

#endif /* MJPEG_STREAM_H */
