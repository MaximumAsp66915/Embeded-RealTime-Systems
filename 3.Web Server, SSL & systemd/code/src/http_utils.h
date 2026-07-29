/*
 * http_utils.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Deliberately minimal HTTP/1.1 handling: this project only needs to parse
 * a request line ("GET /path HTTP/1.1") and pull out the Host header for
 * the redirect. It is NOT a general-purpose HTTP parser — headers beyond
 * Host are ignored, and only GET is meaningfully handled.
 */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#define HTTP_METHOD_MAXLEN 8
#define HTTP_PATH_MAXLEN 256
#define HTTP_HOST_MAXLEN 256

typedef struct {
    char method[HTTP_METHOD_MAXLEN];
    char path[HTTP_PATH_MAXLEN];
    char host[HTTP_HOST_MAXLEN]; /* empty string if no Host header found */
} http_request_t;

/*
 * Parses a raw request buffer (as read straight off the socket) into
 * `out`. Returns 0 on success, -1 if the request line itself couldn't be
 * parsed (garbage input, empty request, etc).
 */
int http_parse_request(const char *raw, http_request_t *out);

#endif /* HTTP_UTILS_H */
