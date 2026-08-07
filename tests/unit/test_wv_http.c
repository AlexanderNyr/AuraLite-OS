/*
 * test_wv_http.c — host unit tests for the web view URL/HTTP layers
 * (WEBVIEW_PLAN phase W6).
 *
 * Links the REAL userspace/apps/gbrowser/wv_url.c + wv_http.c (never
 * copies) and checks the plan's gate:
 *   - a chunked response decodes to the same bytes as an unchunked one;
 *   - an https:// URL produces the explanation path (flagged, parseable),
 *     not a hang;
 *   - the growing response buffer replaces the 16 KB static one and
 *     refuses past its cap with a diagnosed flag.
 *
 * Plus URL parsing/resolution (absolute, relative, ../, root-relative,
 * scheme-absolute, fragments), request building (HTTP/1.1 + Host), header
 * parsing (status, Content-Length, Transfer-Encoding) and malformed-input
 * tolerance.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "userspace/apps/gbrowser/wv_url.h"
#include "userspace/apps/gbrowser/wv_http.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

static void test_url_parse(void) {
    wv_url_t u;
    CK(wv_url_parse("http://example.com/", &u) && u.ok);
    CK(strcmp(u.host, "example.com") == 0);
    CK(u.port == 80 && u.is_https == 0);
    CK(strcmp(u.path, "/") == 0);

    CK(wv_url_parse("https://example.com/x", &u) && u.is_https == 1);
    CK(u.port == 443);
    CK(strcmp(u.path, "/x") == 0);

    CK(wv_url_parse("http://h:8080/p/q", &u) && u.port == 8080);
    CK(strcmp(u.path, "/p/q") == 0);

    /* no scheme -> http */
    CK(wv_url_parse("example.com/a", &u) && u.ok);
    CK(strcmp(u.host, "example.com") == 0 && strcmp(u.path, "/a") == 0);

    /* case-insensitive scheme */
    CK(wv_url_parse("HTTP://HOST/", &u) && strcmp(u.host, "HOST") == 0);

    /* trailing whitespace trimmed */
    CK(wv_url_parse("  http://x/  ", &u) && strcmp(u.host, "x") == 0);

    /* garbage */
    CK(wv_url_parse("", &u) == 0);
    CK(wv_url_parse("ftp://x/", &u) == 0);
    CK(wv_url_parse("http://", &u) == 0);
    CK(wv_url_parse("http://:80/", &u) == 0);
    CK(wv_url_parse("http://host:abc/", &u) == 0);
    CK(wv_url_parse("http://ho st/", &u) == 0);
    CK(wv_url_parse("http://host:99999/", &u) == 0);

    /* query dropped */
    CK(wv_url_parse("http://x/p?q=1", &u) && strcmp(u.path, "/p") == 0);
}

static void test_url_resolve(void) {
    wv_url_t base, u;
    CK(wv_url_parse("http://example.com/dir/page.html", &base));

    CK(wv_url_resolve("http://other.com/z", &base, &u) &&
       strcmp(u.host, "other.com") == 0 && strcmp(u.path, "/z") == 0);
    CK(wv_url_resolve("https://s/x", &base, &u) && u.is_https);

    CK(wv_url_resolve("/root", &base, &u) && strcmp(u.path, "/root") == 0);
    CK(wv_url_resolve("sibling.html", &base, &u) &&
       strcmp(u.path, "/dir/sibling.html") == 0);
    CK(wv_url_resolve("../up.html", &base, &u) &&
       strcmp(u.path, "/up.html") == 0);
    CK(wv_url_resolve("../../deep", &base, &u) &&
       strcmp(u.path, "/deep") == 0);
    CK(wv_url_resolve("./same.html", &base, &u) &&
       strcmp(u.path, "/dir/same.html") == 0);
    CK(wv_url_resolve("#frag", &base, &u) && strcmp(u.path, "/dir/page.html") == 0);
    CK(wv_url_resolve("", &base, &u) && strcmp(u.path, "/dir/page.html") == 0);
    CK(wv_url_resolve("//cdn.example.com/x", &base, &u) &&
       strcmp(u.host, "cdn.example.com") == 0 && u.is_https == 0);
}

static void test_request_build(void) {
    wv_url_t u;
    char req[512];
    CK(wv_url_parse("http://example.com/a/b", &u));
    int n = wv_http_build_request(&u, req, sizeof(req));
    CK(n > 0);
    CK(strstr(req, "GET /a/b HTTP/1.1\r\n") != NULL);
    CK(strstr(req, "Host: example.com\r\n") != NULL);
    CK(strstr(req, "Connection: close\r\n") != NULL);

    CK(wv_url_parse("http://h:8080/x", &u));
    n = wv_http_build_request(&u, req, sizeof(req));
    CK(n > 0 && strstr(req, "Host: h:8080\r\n") != NULL);
}

static void test_headers(void) {
    const char *r1 = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                     "Content-Length: 5\r\n\r\nhello";
    wv_http_meta_t m;
    CK(wv_http_parse_headers(r1, strlen(r1), &m));
    CK(m.status == 200);
    CK(m.has_content_len && m.content_len == 5);
    CK(m.chunked == 0);

    const char *r2 = "HTTP/1.1 404 Not Found\r\nTransfer-Encoding: chunked\r\n\r\n";
    CK(wv_http_parse_headers(r2, strlen(r2), &m));
    CK(m.status == 404 && m.chunked == 1);

    /* incomplete headers */
    const char *r3 = "HTTP/1.1 200 OK\r\nContent-Length: ";
    CK(wv_http_parse_headers(r3, strlen(r3), &m) == 0);

    /* garbage */
    CK(wv_http_parse_headers("NOT HTTP\r\n\r\n", 13, &m) == 0);
}

static void test_chunked_vs_plain(void) {
    /* The plan's gate: a chunked response decodes to the same bytes as an
     * unchunked one. */
    const char *plain_body = "Hello, chunked world! This is the payload. "
                             "It spans several chunks, including a trailing "
                             "empty chunk.\n";
    size_t plen = strlen(plain_body);

    char chunked[1024];
    size_t p = 0;
    size_t i = 0;
    while (i < plen) {
        size_t csz = (plen - i > 11) ? 11 : (plen - i);
        char sz[16];
        int sn = snprintf(sz, sizeof(sz), "%zx\r\n", csz);
        for (int k = 0; k < sn; k++) chunked[p++] = sz[k];
        memcpy(chunked + p, plain_body + i, csz);
        p += csz;
        chunked[p++] = '\r'; chunked[p++] = '\n';
        i += csz;
    }
    memcpy(chunked + p, "0\r\n\r\n", 5);
    p += 5;

    char out[1024];
    int complete = 0;
    int n = wv_http_decode_chunked(chunked, p, out, sizeof(out), &complete);
    CK(n == (int)plen && complete == 1);
    CK(memcmp(out, plain_body, plen) == 0);

    /* chunk extensions are tolerated */
    const char *ext = "4;foo=bar\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    n = wv_http_decode_chunked(ext, strlen(ext), out, sizeof(out), &complete);
    CK(n == 9 && memcmp(out, "Wikipedia", 9) == 0 && complete == 1);

    /* incomplete stream */
    const char *inc = "4\r\nWiki\r\n";
    CK(wv_http_decode_chunked(inc, strlen(inc), out, sizeof(out), &complete) == -1);

    /* garbage */
    CK(wv_http_decode_chunked("zzz\r\n", 5, out, sizeof(out), &complete) == -1);
}

static void test_body_extraction(void) {
    /* plain content-length */
    const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    wv_http_meta_t m;
    CK(wv_http_parse_headers(r, strlen(r), &m));
    char body[64];
    int done = 0;
    int n = wv_http_body(r, strlen(r), &m, body, sizeof(body), &done);
    CK(n == 5 && done == 1 && memcmp(body, "hello", 5) == 0);

    /* truncated body: not done */
    const char *r2 = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
    wv_http_meta_t m2;
    CK(wv_http_parse_headers(r2, strlen(r2), &m2));
    CK(wv_http_body(r2, strlen(r2), &m2, body, sizeof(body), &done) == -1);

    /* no length: body to EOF */
    const char *r3 = "HTTP/1.1 200 OK\r\n\r\nall of this";
    wv_http_meta_t m3;
    CK(wv_http_parse_headers(r3, strlen(r3), &m3));
    n = wv_http_body(r3, strlen(r3), &m3, body, sizeof(body), &done);
    CK(n == 11 && memcmp(body, "all of this", 11) == 0);

    /* chunked end-to-end */
    const char *r4 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n";
    wv_http_meta_t m4;
    CK(wv_http_parse_headers(r4, strlen(r4), &m4));
    n = wv_http_body(r4, strlen(r4), &m4, body, sizeof(body), &done);
    CK(n == 11 && done == 1 && memcmp(body, "Hello World", 11) == 0);
}

static void test_growing_buffer(void) {
    /* 100 KB of body through 2 KB appends into a buffer that starts at
     * 8 KB and must grow (realloc) without refusing */
    size_t total = 100 * 1024;
    char *seed = malloc(2048);
    CK(seed != NULL);
    if (!seed) return;
    for (int i = 0; i < 2048; i++) seed[i] = (char)('a' + (i % 26));

    char *buf = malloc(WV_HTTP_INITIAL_CAP);
    wv_resp_t r;
    wv_resp_init(&r, buf, WV_HTTP_INITIAL_CAP);
    size_t fed = 0;
    while (fed < total) {
        size_t n = total - fed > 2048 ? 2048 : total - fed;
        CK(wv_resp_append(&r, seed, n));
        fed += n;
    }
    CK(r.len == total);
    CK(r.refused == 0);
    CK(r.cap > WV_HTTP_INITIAL_CAP);
    CK(r.data[0] == 'a' && r.data[total - 1] == seed[2047]);

    /* refusal past the cap: init with a tiny fixed buffer that cannot
     * grow past WV_HTTP_MAX_CAP — simulate by pre-filling almost to the
     * ceiling with a large seed */
    char *big = malloc(WV_HTTP_MAX_CAP + 65536);
    wv_resp_t rb;
    wv_resp_init(&rb, big, WV_HTTP_MAX_CAP - 4096);
    size_t feed = 0;
    while (feed < WV_HTTP_MAX_CAP - 4096) {
        size_t n = 8192;
        if (feed + n > WV_HTTP_MAX_CAP - 4096) n = WV_HTTP_MAX_CAP - 4096 - feed;
        wv_resp_append(&rb, seed, n);
        feed += n;
    }
    /* one more append of 8 KB cannot fit (would exceed the ceiling) */
    int ok = wv_resp_append(&rb, seed, 8192);
    CK(ok == 0);
    CK(rb.refused == 1);
    CK(rb.len <= WV_HTTP_MAX_CAP);

    free(seed);
    free(r.data);      /* realloc'ed inside the test: buf is gone */
    free(rb.data);     /* rb.data is either big or a grown copy */

}

static uint32_t frng = 0xBADF00Du;
static uint32_t frand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

static void test_fuzz(void) {
    unsigned char buf[512];
    for (int iter = 0; iter < 1000; iter++) {
        size_t len = frand() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(frand() & 0xFF);
        wv_http_meta_t m;
        if (wv_http_parse_headers((const char *)buf, len, &m)) {
            char out[1024];
            int done = 0;
            (void)wv_http_body((const char *)buf, len, &m, out, sizeof(out), &done);
        }
        wv_url_t u;
        (void)wv_url_parse((const char *)buf, &u);
    }
    CK(1);
    printf("PASS: 1000 fuzz iterations (headers + URLs) terminated\n");
}

int main(void) {
    printf("== webview URL + HTTP (WEBVIEW_PLAN W6) ==\n");

    test_url_parse();
    test_url_resolve();
    test_request_build();
    test_headers();
    test_chunked_vs_plain();
    test_body_extraction();
    test_growing_buffer();
    test_fuzz();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
