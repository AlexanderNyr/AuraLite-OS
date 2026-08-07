/*
 * wv_http.h — HTTP client pieces for the AuraLite web view
 * (WEBVIEW_PLAN W6).  Pure C (no libc beyond memcpy), so the same file is
 * unit-tested on the host and compiled into the guest.
 *
 * This is NOT a sockets layer: the caller performs the I/O (net_connect /
 * net_send / net_recv in the guest) and feeds bytes to these functions.
 *
 * Components:
 *   - request building: GET <path> HTTP/1.1 + Host: (the plan: most
 *     servers stopped speaking 1.0 politely);
 *   - response parsing: status line, headers, Content-Length, chunked
 *     flag, body extraction;
 *   - chunked transfer decoding (the plan: a chunked response decodes to
 *     the same bytes as an unchunked one);
 *   - a GROWING response buffer with an explicit cap and a diagnosed
 *     refusal past it (replaces the 16 KB static buffer).
 */

#ifndef AURALITE_WV_HTTP_H
#define AURALITE_WV_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include "wv_url.h"

/* ---- request ---- */

/* Build "GET <path> HTTP/1.1\r\nHost: <host>[:port]\r\nConnection:
 * close\r\n\r\n" into out.  Returns the length, or -1 when it does not
 * fit. */
int wv_http_build_request(const wv_url_t *u, char *out, size_t cap);

/* ---- response buffer (growing) ---- */

#define WV_HTTP_INITIAL_CAP 8192
#define WV_HTTP_MAX_CAP     (512 * 1024)   /* explicit ceiling */

typedef struct {
    char  *data;
    size_t cap;
    size_t len;
    int    refused;      /* set when the cap was hit (diagnosed refusal) */
} wv_resp_t;

/* Init with a caller-owned buffer of initial_cap (0 = default). */
void wv_resp_init(wv_resp_t *r, char *buf, size_t initial_cap);
/* Append bytes; doubles the buffer (realloc) up to WV_HTTP_MAX_CAP, then
 * sets refused and keeps the prefix.  Returns 1 on success. */
int wv_resp_append(wv_resp_t *r, const char *data, size_t n);

/* ---- response parsing ---- */

typedef struct {
    int      status;          /* 200, 404, ... ; -1 when not parsed */
    int      chunked;
    int      has_content_len;
    size_t   content_len;
    size_t   header_len;      /* bytes of headers (incl. CRLFCRLF) */
    int      ok;              /* header block fully present */
} wv_http_meta_t;

/* Parse the header block at the start of buf[0..len).  Returns 1 when the
 * header terminator CRLFCRLF was found and the status line parsed. */
int wv_http_parse_headers(const char *buf, size_t len, wv_http_meta_t *m);

/* Decode a chunked body: buf[0..len) contains the whole chunked payload
 * (after the headers).  Writes decoded bytes to out (cap = out_cap).
 * Returns the decoded length, or -1 when the stream is malformed or does
 * not fit.  When `complete` is non-NULL it is set to 1 only if the
 * terminating 0-chunk was seen. */
int wv_http_decode_chunked(const char *buf, size_t len,
                           char *out, size_t out_cap, int *complete);

/* Extract the body of a response given parsed meta: bytes after the
 * headers, Content-Length-truncated or complete-chunked-decoded.
 * Returns the body length or -1 (needs more data / malformed).  When
 * `done` is non-NULL it is set to 1 when the body is complete. */
int wv_http_body(const char *buf, size_t len, const wv_http_meta_t *m,
                 char *out, size_t out_cap, int *done);

#endif /* AURALITE_WV_HTTP_H */
