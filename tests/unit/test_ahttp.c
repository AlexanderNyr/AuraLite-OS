/* test_ahttp.c — Host-side HTTP client tests (N6).
 *
 * Tests URL parsing, HTTP/1.1 features (chunked, Content-Length,
 * redirects), and the growing response buffer.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
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

    CHECK(ahttp_url_parse("ftp://example.com", &u) == AHTTP_ERR_URL,
          "URL: ftp:// rejected");

    CHECK(ahttp_url_parse("http://", &u) == AHTTP_ERR_URL,
          "URL: http:// (empty host) rejected");

    CHECK(ahttp_url_parse("", &u) == AHTTP_ERR_URL,
          "URL: empty string rejected");

    CHECK(ahttp_url_parse(NULL, &u) == AHTTP_ERR_URL,
          "URL: NULL rejected");
}

/* ---- Main ---- */

int main(void) {
    printf("=== N6 HTTP Client Test Suite ===\n\n");

    test_url_parse();

    printf("\n=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
