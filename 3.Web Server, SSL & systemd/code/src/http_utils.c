/*
 * http_utils.c — see http_utils.h
 */

#include "http_utils.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

int http_parse_request(const char *raw, http_request_t *out) {
    memset(out, 0, sizeof(*out));

    if (!raw || raw[0] == '\0') return -1;

    /* Request line: "METHOD PATH HTTP/x.x" */
    if (sscanf(raw, "%7s %255s", out->method, out->path) != 2) {
        return -1;
    }

    /* Look for a "Host:" header anywhere in the buffer (case-sensitive is
     * fine here — every real browser sends it capitalized this way). */
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

    return 0;
}
