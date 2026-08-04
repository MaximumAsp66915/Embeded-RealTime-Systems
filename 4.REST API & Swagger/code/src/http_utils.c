/*
 * http_utils.c — see http_utils.h
 */

#include "http_utils.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int http_parse_request(const char *raw, size_t raw_len, http_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->content_length = -1;

    if (!raw || raw_len == 0) return -1;

    /* Request line: "METHOD PATH HTTP/x.x" */
    if (sscanf(raw, "%7s %255s", out->method, out->path) != 2) {
        return -1;
    }

    /* Look for a "Host:" header anywhere in the buffer. */
    const char *host_hdr = strstr(raw, "Host:");
    if (host_hdr) {
        host_hdr += 5; /* skip "Host:" */
        while (*host_hdr == ' ') host_hdr++;

        size_t i = 0;
        while (host_hdr[i] && host_hdr[i] != '\r' && host_hdr[i] != '\n'
               && i < HTTP_HOST_MAXLEN - 1) {
            out->host[i] = host_hdr[i];
            i++;
        }
        out->host[i] = '\0';
    }

    /* Content-Length, if present (case-insensitive per HTTP spec, but
     * every real client sends the canonical capitalization). */
    const char *cl_hdr = strstr(raw, "Content-Length:");
    if (cl_hdr) {
        cl_hdr += strlen("Content-Length:");
        while (*cl_hdr == ' ') cl_hdr++;
        out->content_length = atol(cl_hdr);
    }

    /* Body starts right after the blank line ("\r\n\r\n") separating
     * headers from body. Search within raw_len, not strlen(raw), since a
     * binary-ish body might contain an early '\0' in theory. */
    const char *sep = NULL;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i+1] == '\n' && raw[i+2] == '\r' && raw[i+3] == '\n') {
            sep = raw + i + 4;
            break;
        }
    }

    if (sep) {
        size_t available = raw_len - (size_t)(sep - raw);
        size_t to_copy = available;
        if (out->content_length >= 0 && (size_t)out->content_length < to_copy) {
            to_copy = (size_t)out->content_length;
        }
        if (to_copy > HTTP_BODY_MAXLEN - 1) {
            to_copy = HTTP_BODY_MAXLEN - 1;
        }
        memcpy(out->body, sep, to_copy);
        out->body[to_copy] = '\0';
        out->body_len = to_copy;
    }

    return 0;
}
