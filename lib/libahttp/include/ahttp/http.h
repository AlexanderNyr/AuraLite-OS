#ifndef AHTTP_HTTP_H
#define AHTTP_HTTP_H

/* ahttp/http.h — HTTP/1.1 client over plain TCP or TLS (INTERNET_PLAN.md N6).
 *
 * One function: `ahttp_get(url)` — handles both http:// and https://.
 * Returns a dynamically allocated response that the caller must free
 * with `ahttp_response_free()`.
 *
 * Features:
 *   - HTTP/1.1: Host header, chunked transfer decoding, Content-Length.
 *   - Redirects: 301/302/307/308, bounded to 5 hops.
 *   - HTTPS via libatls (TLS 1.3, Ed25519/RSA certificate validation).
 *   - Growing response buffer with an explicit 1 MiB cap.
 *
 * The library uses BSD sockets internally (socket/connect/send/recv)
 * and never touches the kernel's legacy net_* API.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum response body size (1 MiB). */
#define AHTTP_MAX_BODY (1024 * 1024)

/* Maximum number of redirects to follow. */
#define AHTTP_MAX_REDIRECTS 5

/* HTTP response. */
typedef struct {
    int   status_code;      /* e.g. 200, 301, 404 */
    char *headers;          /* raw header text (NUL-terminated) */
    size_t headers_len;
    uint8_t *body;          /* decoded body bytes */
    size_t body_len;
    char *final_url;        /* URL after redirects (NUL-terminated) */
    int   error;            /* 0 on success, negative on error */
} ahttp_response;

/* Error codes. */
#define AHTTP_OK                0
#define AHTTP_ERR_URL          -1   /* malformed URL */
#define AHTTP_ERR_DNS          -2   /* DNS resolution failed */
#define AHTTP_ERR_CONNECT      -3   /* TCP connect failed */
#define AHTTP_ERR_TLS          -4   /* TLS handshake failed */
#define AHTTP_ERR_REQUEST      -5   /* send failed */
#define AHTTP_ERR_RESPONSE     -6   /* malformed response */
#define AHTTP_ERR_REDIRECT     -7   /* redirect loop or too many redirects */
#define AHTTP_ERR_TOO_LARGE    -8   /* response body exceeds AHTTP_MAX_BODY */
#define AHTTP_ERR_NOMEM        -9   /* allocation failure */

/* Fetch a URL.  Returns a heap-allocated response (caller frees).
 * Supports http:// and https://.
 * hostname: the URL to fetch (e.g. "http://example.com/path").
 * Returns NULL on allocation failure. */
ahttp_response *ahttp_get(const char *url);

/* Free a response. */
void ahttp_response_free(ahttp_response *r);

/* ---- URL parsing (exposed for testing) ---- */
typedef struct {
    char scheme[8];     /* "http" or "https" */
    char host[256];     /* hostname */
    int  port;          /* 0 = default (80 for http, 443 for https) */
    char path[2048];    /* path + query (starts with '/') */
} ahttp_url;

/* Parse a URL. Returns 0 on success, negative on error. */
int ahttp_url_parse(const char *url, ahttp_url *out);

#ifdef __cplusplus
}
#endif

#endif /* AHTTP_HTTP_H */
