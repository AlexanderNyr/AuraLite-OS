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
#include "atls/certval.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum response body size (1 MiB). */
#define AHTTP_MAX_BODY (1024 * 1024)

/* Maximum request body size for POST/PUT (64 KiB).  The interface is
 * deliberately bounded: uploads are buffered in memory before sending,
 * so the cap keeps RAM usage predictable on a hobby OS (X6). */
#define AHTTP_MAX_REQ_BODY (64 * 1024)

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
    int   redirects_used;   /* number of 3xx hops followed (X6) */
    int   reused_connection;/* 1 if the final fetch reused a cached socket (X6) */
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
#define AHTTP_ERR_METHOD       -10  /* unsupported/disallowed method or oversized request body (X6) */

/* Fetch a URL.  Returns a heap-allocated response (caller frees).
 * Supports http:// and https://.
 * hostname: the URL to fetch (e.g. "http://example.com/path").
 * Returns NULL on allocation failure. */
ahttp_response *ahttp_get(const char *url);

/* Free a response. */
void ahttp_response_free(ahttp_response *r);

/* ---- Keep-alive client (REALINTERNET_PLAN X6) ----
 * A client caches one live connection (keyed by scheme+host+port) and
 * reuses it for subsequent requests to the same origin, sending
 * "Connection: keep-alive" and transparently reopening a stale socket
 * once for idempotent methods (GET/HEAD).  Every reuse/reopen is
 * logged with a [ahttp] keep-alive line so the integration gates can
 * assert socket reuse from the serial log.
 *
 * The client is not thread-safe (AuraLite apps are single-threaded
 * fetch loops); call sites must serialise their own access. */
typedef struct ahttp_client ahttp_client;

/* Create/destroy a client.  Freeing closes any cached connection. */
ahttp_client *ahttp_client_new(void);
void          ahttp_client_free(ahttp_client *c);

/* Per-client trust store; overrides the global ahttp_set_trust_roots()
 * setting for fetches through this client.  Ownership stays with the
 * caller; arrays must outlive the client.  Passing num_roots == 0 keeps
 * CertificateVerify-only checking. */
void ahttp_client_set_trust_roots(ahttp_client *c,
                                  const atls_trust_root *roots, int num_roots,
                                  const atls_time_now *now);

/* Perform a request.  method is one of GET/HEAD/POST/PUT/DELETE
 * (case-insensitive); anything else fails with AHTTP_ERR_METHOD.
 * body/body_len are only sent for POST/PUT, must be <= AHTTP_MAX_REQ_BODY,
 * and get Content-Length (+ Content-Type when content_type != NULL).
 * Follows redirects like ahttp_get(): 301/302 convert to GET (body
 * dropped), 307/308 keep the method and re-send the body. */
ahttp_response *ahttp_client_request(ahttp_client *c, const char *method,
                                     const char *url,
                                     const char *content_type,
                                     const void *body, size_t body_len);

/* Convenience: ahttp_client_request(c, "GET", url, NULL, NULL, 0). */
ahttp_response *ahttp_client_get(ahttp_client *c, const char *url);

/* ---- Redirect target resolution (X6, exposed for testing) ----
 * Resolve a Location header value against the URL that produced it.
 * Handles absolute http(s) URLs, protocol-relative //host/path,
 * absolute-path /path, and relative references against the current
 * path's directory.  Writes a full absolute URL into `out`.
 * Returns 0 on success, AHTTP_ERR_REDIRECT if unresolvable/too long. */
int ahttp_resolve_redirect(char *out, size_t cap,
                           const char *current_url, const char *location);

/* ---- Trust-store loader (X6) ----
 * Parse a PEM bundle of CERTIFICATE blocks into caller-owned storage,
 * so every client app does not duplicate the 40-line PEM walk.
 * roots[] entries alias slices of derbuf.  Returns 0 when at least one
 * root was decoded, -1 otherwise. */
int ahttp_load_trust_roots(const char *path, atls_trust_root *roots,
                           uint8_t *derbuf, int max_roots,
                           size_t derbuf_size, int *out_count);

/* ---- Trust store (REALINTERNET_PLAN X2) ----
 * By default HTTPS verifies only the CertificateVerify signature.  Call
 * ahttp_set_trust_roots() to supply a pinned trust store and current time
 * so the whole server chain is validated before the fetch succeeds.  The
 * caller keeps ownership of the arrays; they must outlive the fetch.
 * Pass num_roots == 0 to disable chain validation (the default). */
void ahttp_set_trust_roots(const atls_trust_root *roots, int num_roots,
                           const atls_time_now *now);

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
