/*
 * http_utils.h — Smart Surveillance System, Step 3/4 (Web Server + REST API)
 *
 * Deliberately minimal HTTP/1.1 handling: this project only needs to parse
 * a request line ("GET /path HTTP/1.1"), pull out the Host header for the
 * redirect, and — as of Step 4 — capture a small JSON body for POST
 * requests. It is NOT a general-purpose HTTP parser.
 *
 * Body handling note: the body is copied out of whatever was already read
 * off the socket in one recv() call (see https_server.c's client_thread,
 * which reads up to 4095 bytes before parsing). That's enough for this
 * project's JSON command bodies (a few dozen bytes), but this parser does
 * NOT handle a body split across multiple TCP segments / chunked transfer
 * encoding — a real production HTTP server would need to keep reading
 * until Content-Length bytes of body have arrived.
 */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

#define HTTP_METHOD_MAXLEN 8
#define HTTP_PATH_MAXLEN 256
#define HTTP_HOST_MAXLEN 256
#define HTTP_BODY_MAXLEN 2048

typedef struct {
    char method[HTTP_METHOD_MAXLEN];
    char path[HTTP_PATH_MAXLEN];
    char host[HTTP_HOST_MAXLEN]; /* empty string if no Host header found */
    char body[HTTP_BODY_MAXLEN]; /* empty string if no body / GET request */
    size_t body_len;
    long content_length;         /* from the Content-Length header, or -1 if absent */
} http_request_t;

/*
 * Parses a raw request buffer (as read straight off the socket) into
 * `out`. Returns 0 on success, -1 if the request line itself couldn't be
 * parsed (garbage input, empty request, etc).
 *
 * `raw_len` is the number of bytes actually read (raw may contain binary
 * garbage past that point in the caller's buffer) — pass the recv()
 * return value, not strlen(raw), since a JSON body doesn't guarantee a
 * null terminator lines up where you'd expect.
 */
int http_parse_request(const char *raw, size_t raw_len, http_request_t *out);

#endif /* HTTP_UTILS_H */
