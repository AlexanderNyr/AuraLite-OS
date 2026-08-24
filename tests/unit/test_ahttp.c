/* test_ahttp.c — Host-side HTTP client tests (N6 + X6).
 *
 * Part 1 (N6): URL parsing.
 * Part 2 (X6): redirect target resolution (pure function).
 * Part 3 (X6): keep-alive client against an in-process threaded TCP
 *              server — connection reuse, stale-socket reopen, request
 *              bodies (POST/PUT echo), 307 re-POST, loop guard,
 *              Connection: close handling, HEAD/204 framing, chunked.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include "ahttp/http.h"

/* Host stubs for guest-only functions. */
uint32_t dns_resolve(const char *h) {
    (void)h; return 0;
}
int closesocket(int fd) { return close(fd); }

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while(0)

/* ---- URL parsing tests ---- */

static void test_url_parse(void) {
    ahttp_url u;

    CHECK(ahttp_url_parse("http://example.com", &u) == AHTTP_OK &&
          strcmp(u.scheme, "http") == 0 && strcmp(u.host, "example.com") == 0 &&
          u.port == 80 && strcmp(u.path, "/") == 0,
          "URL: http://example.com");

    CHECK(ahttp_url_parse("https://example.com:8443/path?q=1", &u) == AHTTP_OK &&
          strcmp(u.scheme, "https") == 0 && u.port == 8443 &&
          strcmp(u.path, "/path?q=1") == 0,
          "URL: https://example.com:8443/path?q=1");

    CHECK(ahttp_url_parse("http://10.0.2.2:18080/index.html", &u) == AHTTP_OK &&
          strcmp(u.host, "10.0.2.2") == 0 && u.port == 18080,
          "URL: http://10.0.2.2:18080/index.html");

    CHECK(ahttp_url_parse("https://[fec0::2]:8446/", &u) == AHTTP_OK &&
          strcmp(u.scheme, "https") == 0 && strcmp(u.host, "fec0::2") == 0 &&
          u.port == 8446 && strcmp(u.path, "/") == 0,
          "URL: https://[fec0::2]:8446/");

    CHECK(ahttp_url_parse("http://[::1]/x", &u) == AHTTP_OK &&
          strcmp(u.host, "::1") == 0 && u.port == 80 &&
          strcmp(u.path, "/x") == 0,
          "URL: http://[::1]/x");

    CHECK(ahttp_url_parse("ftp://example.com", &u) == AHTTP_ERR_URL,
          "URL: ftp:// rejected");

    CHECK(ahttp_url_parse("http://", &u) == AHTTP_ERR_URL,
          "URL: http:// (empty host) rejected");

    CHECK(ahttp_url_parse("", &u) == AHTTP_ERR_URL,
          "URL: empty string rejected");

    CHECK(ahttp_url_parse(NULL, &u) == AHTTP_ERR_URL,
          "URL: NULL rejected");
}

/* ---- Redirect resolution tests (X6) ---- */

static void test_resolve_redirect(void) {
    char out[2304];
    const char *base = "http://a.com/dir/page?old=1";
    int rc;

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "https://b.com/x");
    CHECK(rc == 0 && strcmp(out, "https://b.com/x") == 0,
          "resolve: absolute https URL");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "//cdn.com/p");
    CHECK(rc == 0 && strcmp(out, "http://cdn.com/p") == 0,
          "resolve: protocol-relative keeps scheme");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "/new?q=1");
    CHECK(rc == 0 && strcmp(out, "http://a.com/new?q=1") == 0,
          "resolve: absolute path");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "z");
    CHECK(rc == 0 && strcmp(out, "http://a.com/dir/z") == 0,
          "resolve: relative sibling");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "../up");
    CHECK(rc == 0 && strcmp(out, "http://a.com/up") == 0,
          "resolve: dot-dot climbs one level");

    rc = ahttp_resolve_redirect(out, sizeof(out), "http://a.com/d/e/f", "../../x");
    CHECK(rc == 0 && strcmp(out, "http://a.com/x") == 0,
          "resolve: dot-dot beyond root clamps at /");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "./a");
    CHECK(rc == 0 && strcmp(out, "http://a.com/dir/a") == 0,
          "resolve: dot-self collapses");

    rc = ahttp_resolve_redirect(out, sizeof(out), "http://a.com:8080/p", "/q");
    CHECK(rc == 0 && strcmp(out, "http://a.com:8080/q") == 0,
          "resolve: non-default port preserved");

    rc = ahttp_resolve_redirect(out, sizeof(out), "https://a.com/p", "/q");
    CHECK(rc == 0 && strcmp(out, "https://a.com/q") == 0,
          "resolve: https default port elided");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "/a/../b?keep=1");
    CHECK(rc == 0 && strcmp(out, "http://a.com/b?keep=1") == 0,
          "resolve: dot removal keeps query");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "javascript:alert(1)");
    CHECK(rc != 0, "resolve: javascript: refused");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "data:text/html,boom");
    CHECK(rc != 0, "resolve: data: refused");

    rc = ahttp_resolve_redirect(out, sizeof(out), base, "/x?q=1\r\n");
    CHECK(rc == 0 && strcmp(out, "http://a.com/x?q=1") == 0,
          "resolve: trailing CRLF trimmed");

    char tiny[24];
    rc = ahttp_resolve_redirect(tiny, sizeof(tiny), base,
                                "/this/path/is/far/too/long/for/the/buffer");
    CHECK(rc != 0, "resolve: output overflow refused");
}

/* ---- In-process test server (X6) ----
 *
 * One thread accepts connections on 127.0.0.1 and serves a fixed number
 * of scripted requests, then closes the listener.  The parent counts
 * connections/requests to prove (non-)reuse from the client side. */

static int srv_listen_fd = -1;
static int srv_port;
static int srv_conns, srv_reqs;
static int srv_expected;      /* serve this many requests, then return */
static int srv_overdose;      /* more requests than expected arrived */

/* Read one HTTP request (headers + optional Content-Length body).
 * Returns 1 on success, 0 on EOF/error.  Fills method/path/body. */
static int srv_read_request(int c, char *method, char *path,
                            char *body, size_t *body_len) {
    char hdr[2048];
    size_t hl = 0;
    *body_len = 0;
    while (hl + 1 < sizeof(hdr)) {
        char ch;
        ssize_t n = recv(c, &ch, 1, 0);
        if (n <= 0) return 0;
        hdr[hl++] = ch;
        if (hl >= 4 && memcmp(hdr + hl - 4, "\r\n\r\n", 4) == 0) break;
    }
    hdr[hl] = 0;
    if (sscanf(hdr, "%7s %2047s", method, path) != 2) return 0;
    /* Dispatch is by path only; a request target may carry a query. */
    char *qm = strchr(path, '?');
    if (qm) *qm = 0;

    /* Content-Length? */
    size_t cl = 0;
    const char *p = hdr;
    while ((p = strstr(p, "Content-Length: ")) != NULL) {
        cl = (size_t)atoi(p + 16);
        break;
    }
    while (*body_len < cl && *body_len < 128 * 1024) {
        ssize_t n = recv(c, body + *body_len, cl - *body_len, 0);
        if (n <= 0) return 0;
        *body_len += (size_t)n;
    }
    body[*body_len] = 0;
    return 1;
}

static void srv_respond(int c, const char *status, const char *extra_headers,
                        const char *body) {
    char h[1024];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %s\r\nContent-Length: %zu\r\n%s\r\n",
                     status, body ? strlen(body) : 0,
                     extra_headers ? extra_headers : "");
    send(c, h, (size_t)n, 0);
    if (body) send(c, body, strlen(body), 0);
}

static void *srv_main(void *arg) {
    (void)arg;
    while (srv_reqs < srv_expected) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int c = accept(srv_listen_fd, (struct sockaddr *)&sa, &sl);
        if (c < 0) break;
        srv_conns++;
        /* Per-connection request loop (keep-alive). */
        for (;;) {
            char method[8], path[2048], body[128 * 1024];
            size_t body_len = 0;
            if (!srv_read_request(c, method, path, body, &body_len)) break;
            srv_reqs++;

            if (strcmp(path, "/first") == 0) {
                srv_respond(c, "200 OK", "Connection: keep-alive\r\n", "one");
            } else if (strcmp(path, "/second") == 0) {
                srv_respond(c, "200 OK", "Connection: keep-alive\r\n", "two");
            } else if (strcmp(path, "/echo") == 0) {
                char hdr[128];
                snprintf(hdr, sizeof(hdr),
                         "Content-Type: application/octet-stream\r\n"
                         "Connection: keep-alive\r\n");
                srv_respond(c, "200 OK", hdr, body_len ? body : "");
            } else if (strcmp(path, "/head") == 0) {
                /* HEAD: real CL in headers, no body bytes on the wire. */
                const char *h = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                                "Connection: keep-alive\r\n\r\n";
                send(c, h, strlen(h), 0);
            } else if (strcmp(path, "/page204") == 0) {
                const char *h = "HTTP/1.1 204 No Content\r\n"
                                "Connection: keep-alive\r\n\r\n";
                send(c, h, strlen(h), 0);
            } else if (strcmp(path, "/chunked") == 0) {
                const char *r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                "Connection: keep-alive\r\n\r\n"
                                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
                send(c, r, strlen(r), 0);
            } else if (strcmp(path, "/redirect") == 0) {
                srv_respond(c, "301 Moved Permanently",
                            "Location: /landed\r\nConnection: keep-alive\r\n", "");
            } else if (strcmp(path, "/landed") == 0) {
                srv_respond(c, "200 OK", "Connection: keep-alive\r\n", "landed");
            } else if (strcmp(path, "/rel/base") == 0) {
                srv_respond(c, "302 Found",
                            "Location: ../landed2?q=2\r\nConnection: keep-alive\r\n", "");
            } else if (strcmp(path, "/landed2") == 0) {
                srv_respond(c, "200 OK", "Connection: keep-alive\r\n", "l2");
            } else if (strcmp(path, "/r307") == 0) {
                srv_respond(c, "307 Temporary Redirect",
                            "Location: /echo\r\nConnection: keep-alive\r\n", "");
            } else if (strcmp(path, "/loopa") == 0) {
                srv_respond(c, "301 Moved Permanently",
                            "Location: /loopb\r\nConnection: keep-alive\r\n", "");
            } else if (strcmp(path, "/loopb") == 0) {
                srv_respond(c, "301 Moved Permanently",
                            "Location: /loopa\r\nConnection: keep-alive\r\n", "");
            } else if (strcmp(path, "/drop1") == 0) {
                /* Respond, then close without Connection: close — the
                 * client caches a socket that is already dead. */
                srv_respond(c, "200 OK", "", "d");
                close(c);
                break;
            } else if (strcmp(path, "/closehdr") == 0) {
                srv_respond(c, "200 OK", "Connection: close\r\n", "c");
                close(c);
                break;
            } else {
                srv_respond(c, "404 Not Found", "Connection: keep-alive\r\n", "nf");
            }
            if (srv_reqs >= srv_expected) { close(c); break; }
        }
        if (srv_reqs >= srv_expected) break;
    }
    close(srv_listen_fd);
    srv_listen_fd = -1;
    return NULL;
}

/* Start a fresh server thread that will serve exactly `expected`
 * requests.  Fills *port_out. */
static void srv_start(int expected) {
    srv_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    bind(srv_listen_fd, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t sl = sizeof(sa);
    getsockname(srv_listen_fd, (struct sockaddr *)&sa, &sl);
    srv_port = ntohs(sa.sin_port);
    listen(srv_listen_fd, 4);
    srv_conns = 0; srv_reqs = 0; srv_expected = expected; srv_overdose = 0;
}

static void srv_join(pthread_t th) {
    pthread_join(th, NULL);
}

/* Build a loopback URL for the current server. */
static void srv_url(char *out, size_t cap, const char *path) {
    snprintf(out, cap, "http://127.0.0.1:%d%s", srv_port, path);
}

/* ---- X6 keep-alive client tests ---- */

static void test_keepalive_reuse(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/first");
    ahttp_response *r1 = ahttp_client_get(c, url);
    int ok = r1 && r1->status_code == 200 && r1->body_len == 3 &&
             memcmp(r1->body, "one", 3) == 0 && r1->reused_connection == 0;
    if (r1) ahttp_response_free(r1);

    srv_url(url, sizeof(url), "/second");
    ahttp_response *r2 = ahttp_client_get(c, url);
    ok = ok && r2 && r2->status_code == 200 && r2->body_len == 3 &&
         memcmp(r2->body, "two", 3) == 0 && r2->reused_connection == 1;
    if (r2) ahttp_response_free(r2);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: GET /first + /second on one socket");
    CHECK(srv_conns == 1 && srv_reqs == 2,
          "keep-alive: server saw 1 connection, 2 requests");
}

static void test_post_put_echo(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/echo");
    const char *payload = "hello, keep-alive world";
    ahttp_response *r1 = ahttp_client_request(c, "POST", url,
        "text/plain", payload, strlen(payload));
    int ok = r1 && r1->status_code == 200 &&
             r1->body_len == strlen(payload) &&
             memcmp(r1->body, payload, strlen(payload)) == 0;
    if (r1) ahttp_response_free(r1);

    const char *put_body = "PUT body 12345";
    ahttp_response *r2 = ahttp_client_request(c, "PUT", url,
        "text/plain", put_body, strlen(put_body));
    ok = ok && r2 && r2->status_code == 200 &&
         r2->reused_connection == 1 &&
         r2->body_len == strlen(put_body) &&
         memcmp(r2->body, put_body, strlen(put_body)) == 0;
    if (r2) ahttp_response_free(r2);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: POST + PUT echo bodies");
    CHECK(srv_conns == 1, "keep-alive: POST/PUT reused the socket");
}

static void test_stale_reopen(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/drop1");
    ahttp_response *r1 = ahttp_client_get(c, url);
    int ok = r1 && r1->status_code == 200;
    if (r1) ahttp_response_free(r1);

    /* The server closed the cached socket without saying so.  The next
     * GET must detect the stale socket silently and reopen. */
    srv_url(url, sizeof(url), "/second");
    ahttp_response *r2 = ahttp_client_get(c, url);
    ok = ok && r2 && r2->status_code == 200 && r2->body_len == 3 &&
         memcmp(r2->body, "two", 3) == 0 && r2->reused_connection == 0;
    if (r2) ahttp_response_free(r2);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: stale socket transparently reopened");
    CHECK(srv_conns == 2 && srv_reqs == 2,
          "keep-alive: stale cost exactly one extra connection");
}

static void test_connection_close_hdr(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/closehdr");
    ahttp_response *r1 = ahttp_client_get(c, url);
    int ok = r1 && r1->status_code == 200 && r1->body_len == 1 &&
             r1->body[0] == 'c';
    if (r1) ahttp_response_free(r1);

    srv_url(url, sizeof(url), "/second");
    ahttp_response *r2 = ahttp_client_get(c, url);
    ok = ok && r2 && r2->status_code == 200 && r2->reused_connection == 0;
    if (r2) ahttp_response_free(r2);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: Connection: close retires the socket");
    CHECK(srv_conns == 2, "keep-alive: no reuse after Connection: close");
}

static void test_head_and_204_framing(void) {
    char url[128];
    srv_start(3);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/head");
    ahttp_response *r1 = ahttp_client_request(c, "HEAD", url, NULL, NULL, 0);
    int ok = r1 && r1->status_code == 200 && r1->body_len == 0;
    if (r1) ahttp_response_free(r1);

    srv_url(url, sizeof(url), "/page204");
    ahttp_response *r2 = ahttp_client_get(c, url);
    ok = ok && r2 && r2->status_code == 204 && r2->body_len == 0 &&
         r2->reused_connection == 1;
    if (r2) ahttp_response_free(r2);

    /* Stream must still be aligned: the next response parses cleanly. */
    srv_url(url, sizeof(url), "/second");
    ahttp_response *r3 = ahttp_client_get(c, url);
    ok = ok && r3 && r3->status_code == 200 && r3->body_len == 3 &&
         memcmp(r3->body, "two", 3) == 0 && r3->reused_connection == 1;
    if (r3) ahttp_response_free(r3);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: HEAD and 204 leave the stream aligned");
    CHECK(srv_conns == 1 && srv_reqs == 3,
          "keep-alive: HEAD/204 kept the socket");
}

static void test_chunked_reuse(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/chunked");
    ahttp_response *r1 = ahttp_client_get(c, url);
    int ok = r1 && r1->status_code == 200 && r1->body_len == 11 &&
             memcmp(r1->body, "hello world", 11) == 0;
    if (r1) ahttp_response_free(r1);

    srv_url(url, sizeof(url), "/second");
    ahttp_response *r2 = ahttp_client_get(c, url);
    ok = ok && r2 && r2->status_code == 200 && r2->reused_connection == 1;
    if (r2) ahttp_response_free(r2);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "keep-alive: chunked body, then reuse");
    CHECK(srv_conns == 1, "keep-alive: chunked kept the socket");
}

static void test_redirect_chain(void) {
    char url[128], expect[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/redirect");
    ahttp_response *r = ahttp_client_get(c, url);
    srv_url(expect, sizeof(expect), "/landed");
    int ok = r && r->status_code == 200 && r->redirects_used == 1 &&
             r->body_len == 6 && memcmp(r->body, "landed", 6) == 0 &&
             r->final_url && strcmp(r->final_url, expect) == 0;
    if (r) ahttp_response_free(r);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "redirect: 301 followed, one hop, final_url reported");
    CHECK(srv_conns == 1 && srv_reqs == 2,
          "redirect: hop reused the same socket");
}

static void test_relative_redirect(void) {
    char url[128], expect[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/rel/base");
    ahttp_response *r = ahttp_client_get(c, url);
    srv_url(expect, sizeof(expect), "/landed2?q=2");
    int ok = r && r->status_code == 200 && r->redirects_used == 1 &&
             r->body_len == 2 && memcmp(r->body, "l2", 2) == 0 &&
             r->final_url && strcmp(r->final_url, expect) == 0;
    if (r) ahttp_response_free(r);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "redirect: relative Location resolved against base");
}

static void test_307_repost(void) {
    char url[128];
    srv_start(2);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/r307");
    const char *payload = "xy";
    ahttp_response *r = ahttp_client_request(c, "POST", url, NULL,
                                             payload, strlen(payload));
    /* 307 keeps the method and re-sends the body: /echo answers with it. */
    int ok = r && r->status_code == 200 && r->redirects_used == 1 &&
             r->body_len == 2 && memcmp(r->body, "xy", 2) == 0;
    if (r) ahttp_response_free(r);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "redirect: 307 re-sends POST body");
}

static void test_redirect_loop_guard(void) {
    char url[128];
    srv_start(AHTTP_MAX_REDIRECTS + 1);
    pthread_t th;
    pthread_create(&th, NULL, srv_main, NULL);

    ahttp_client *c = ahttp_client_new();
    srv_url(url, sizeof(url), "/loopa");
    ahttp_response *r = ahttp_client_get(c, url);
    int ok = r && r->error == AHTTP_ERR_REDIRECT &&
             r->redirects_used == AHTTP_MAX_REDIRECTS + 1;
    if (r) ahttp_response_free(r);
    ahttp_client_free(c);
    srv_join(th);

    CHECK(ok, "redirect: loop stopped after AHTTP_MAX_REDIRECTS+1 hops");
}

static void test_method_validation(void) {
    ahttp_client *c = ahttp_client_new();
    char dummy[16] = {0};
    CHECK(ahttp_client_request(c, "FOO", "http://x/", NULL, NULL, 0) == NULL,
          "methods: unknown method refused");
    CHECK(ahttp_client_request(c, "GET", "http://x/", NULL, dummy, 4) == NULL,
          "methods: GET with body refused");
    {
        /* Lowercase is normalized (no connection attempted: bad scheme). */
        ahttp_response *r = ahttp_client_request(c, "post",
                                                 "ftp://x/", NULL, dummy, 4);
        CHECK(r && r->error == AHTTP_ERR_URL,
              "methods: lowercase post normalized, bad URL surfaces");
        if (r) ahttp_response_free(r);
    }
    CHECK(ahttp_client_request(c, "POST", "http://x/", NULL, NULL, 4) == NULL,
          "methods: body_len without body refused");
    CHECK(ahttp_client_request(c, "POST", "http://x/", NULL, dummy,
                               AHTTP_MAX_REQ_BODY + 1) == NULL,
          "methods: oversized body refused");
    ahttp_client_free(c);
}

/* ---- Main ---- */

int main(void) {
    printf("=== N6/X6 HTTP Client Test Suite ===\n\n");

    signal(SIGPIPE, SIG_IGN);

    test_url_parse();
    test_resolve_redirect();
    test_keepalive_reuse();
    test_post_put_echo();
    test_stale_reopen();
    test_connection_close_hdr();
    test_head_and_204_framing();
    test_chunked_reuse();
    test_redirect_chain();
    test_relative_redirect();
    test_307_repost();
    test_redirect_loop_guard();
    test_method_validation();

    printf("\n=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
