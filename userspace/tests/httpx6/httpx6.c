/*
 * httpx6.c — REALINTERNET_PLAN X6 in-guest gate.
 *
 * Exercises the libahttp keep-alive client end to end against a test
 * server outside the qemu SLIRP boundary:
 *
 *   1. GET  http://<ip>:<port>/first    — fresh connection
 *   2. GET  http://<ip>:<port>/second   — must reuse the cached socket
 *   3. POST http://<ip>:<port>/echo     — body upload + echo, still one socket
 *   4. GET  http://<ip>:<port>/redirect — 301 → https://<ip>:<tls>/ which
 *      serves the [X6_HTTPS_MARKER] payload; redirects_used must be 1.
 *
 * Usage: run httpx6 <ip> <http_port> <tls_port>
 *
 * The host-side python server proves from its own socket counters that
 * steps 1–4 travelled over exactly ONE HTTP connection (the qemu test
 * asserts connections=1 requests=4), independently of the guest log.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ahttp/http.h"

#define HTTP_BODY_MARKER "x6-post-payload"
#define HTTPS_MARKER "[X6_HTTPS_MARKER]"

static int g_fails;

/* NUL-free substring scan: response bodies are counted, not terminated. */
static int body_contains(const ahttp_response *r, const char *needle) {
    size_t nl = strlen(needle);
    if (r->body_len < nl) return 0;
    for (size_t i = 0; i + nl <= r->body_len; i++) {
        if (memcmp(r->body + i, needle, nl) == 0) return 1;
    }
    return 0;
}

static void check(int cond, const char *name) {
    if (cond) {
        printf("[httpx6] PASS %s\n", name);
    } else {
        printf("[httpx6] FAIL %s\n", name);
        g_fails++;
    }
}

static int expect_get(ahttp_client *c, const char *url,
                      const char *want_body, int want_reused,
                      const char *name) {
    ahttp_response *r = ahttp_client_get(c, url);
    int ok = r && r->error == AHTTP_OK && r->status_code == 200 &&
             r->body_len == strlen(want_body) &&
             memcmp(r->body, want_body, strlen(want_body)) == 0 &&
             r->reused_connection == want_reused;
    if (!ok) {
        printf("[httpx6] dbg %s: r=%p err=%d st=%d len=%u reused=%d\n",
               name, (void *)r, r ? r->error : -99,
               r ? r->status_code : -1,
               r ? (unsigned)r->body_len : 0,
               r ? r->reused_connection : -1);
    }
    if (r) ahttp_response_free(r);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        printf("usage: httpx6 <ip> <http_port> <tls_port>\n");
        printf("[httpx6] FAIL usage\n");
        return 1;
    }
    const char *ip = argv[1];
    const char *hport = argv[2];
    const char *tport = argv[3];

    printf("[httpx6] X6 HTTP completeness gate (ip=%s http=%s tls=%s)\n",
           ip, hport, tport);

    ahttp_client *c = ahttp_client_new();
    if (!c) {
        printf("[httpx6] FAIL client_new\n");
        return 1;
    }

    char url[256];

    /* 1+2: basic keep-alive reuse on one origin. */
    snprintf(url, sizeof(url), "http://%s:%s/first", ip, hport);
    check(expect_get(c, url, "first-page", 0, "get-first"), "get-first");

    snprintf(url, sizeof(url), "http://%s:%s/second", ip, hport);
    check(expect_get(c, url, "second-page", 1, "get-second-reuses"),
          "get-second-reuses");

    /* 3: POST with a Content-Length body, echoed back on the same socket. */
    snprintf(url, sizeof(url), "http://%s:%s/echo", ip, hport);
    {
        ahttp_response *r = ahttp_client_request(
            c, "POST", url, "text/plain",
            HTTP_BODY_MARKER, strlen(HTTP_BODY_MARKER));
        int ok = r && r->error == AHTTP_OK && r->status_code == 200 &&
                 r->body_len == strlen(HTTP_BODY_MARKER) &&
                 memcmp(r->body, HTTP_BODY_MARKER,
                        strlen(HTTP_BODY_MARKER)) == 0 &&
                 r->reused_connection == 1;
        if (r) ahttp_response_free(r);
        check(ok, "post-echo-reuses");
    }

    /* 4: http→https redirect in one request: /redirect answers
     * 301 Location: https://<ip>:<tls>/ ; the TLS server (self-signed,
     * CertificateVerify-only trust mode) serves the marker page. */
    snprintf(url, sizeof(url), "http://%s:%s/redirect", ip, hport);
    {
        ahttp_response *r = ahttp_client_get(c, url);
        int ok = r && r->error == AHTTP_OK && r->status_code == 200 &&
                 r->redirects_used == 1 &&
                 r->body &&
                 body_contains(r, HTTPS_MARKER);
        if (r && !ok)
            printf("[httpx6] dbg redirect: err=%d st=%d hops=%d len=%u\n",
                   r->error, r->status_code, r->redirects_used,
                   (unsigned)r->body_len);
        if (r) ahttp_response_free(r);
        check(ok, "redirect-http-to-https");
    }

    ahttp_client_free(c);

    if (g_fails == 0) {
        printf("[httpx6] ALL PASS\n");
        return 0;
    }
    printf("[httpx6] %d FAILURES\n", g_fails);
    return 1;
}
