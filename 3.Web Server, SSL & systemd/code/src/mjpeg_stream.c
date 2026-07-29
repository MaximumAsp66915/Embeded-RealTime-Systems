/*
 * mjpeg_stream.c — see mjpeg_stream.h
 */

#define _POSIX_C_SOURCE 200809L

#include "mjpeg_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BOUNDARY "surveillanceframe"
#define MAX_JPEG_BYTES (2 * 1024 * 1024) /* generous cap for a single frame */

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Reads the whole current frame file into a malloc'd buffer.
 * Returns byte count, or -1 if the file doesn't exist yet / is empty. */
static long read_frame_file(const char *path, unsigned char **out_buf) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > MAX_JPEG_BYTES) {
        fclose(f);
        return -1;
    }

    unsigned char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read_bytes != (size_t)size) {
        free(buf);
        return -1;
    }

    *out_buf = buf;
    return size;
}

static int write_all(void *conn, conn_write_fn write_fn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t sent = 0;
    while (sent < len) {
        long n = write_fn(conn, p + sent, len - sent);
        if (n <= 0) return -1; /* client gone */
        sent += (size_t)n;
    }
    return 0;
}

void mjpeg_stream_serve(const server_config_t *cfg, void *conn, conn_write_fn write_fn) {
    /* HTTP headers for a multipart/x-mixed-replace stream. Sent once; every
     * frame after this is just another boundary-delimited part. */
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        BOUNDARY);

    if (write_all(conn, write_fn, header, (size_t)header_len) != 0) return;

    for (;;) {
        unsigned char *jpeg_buf = NULL;
        long jpeg_len = read_frame_file(cfg->frame_path, &jpeg_buf);

        if (jpeg_len > 0) {
            char part_header[256];
            int part_header_len = snprintf(part_header, sizeof(part_header),
                "--%s\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %ld\r\n"
                "\r\n",
                BOUNDARY, jpeg_len);

            if (write_all(conn, write_fn, part_header, (size_t)part_header_len) != 0 ||
                write_all(conn, write_fn, jpeg_buf, (size_t)jpeg_len) != 0 ||
                write_all(conn, write_fn, "\r\n", 2) != 0) {
                free(jpeg_buf);
                return; /* client disconnected */
            }
            free(jpeg_buf);
        }
        /* If the frame file isn't there yet (detector not started), we just
         * skip this tick and try again next interval rather than erroring
         * out the whole stream. */

        sleep_ms(cfg->mjpeg_frame_interval_ms);
    }
}
