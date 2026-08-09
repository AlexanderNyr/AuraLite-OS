/* test_ahttp_https.c — Host-side HTTPS client tests
 * (REALINTERNET_PLAN.md phase X2).
 *
 * Runs a local openssl s_server with an ECDSA P-256 chain and fetches a
 * page through libahttp's ahttp_get() over TLS.  Exercises the whole X2
 * stack end to end: the real libahttp sources + libatls + the real TLS
 * transport, chain validation against a pinned root, and the failure
 * when the server's chain is not trusted.
 *
 * Because this needs a live TLS server, it is gated behind the openssl
 * CLI being present (same pattern as test_atls_tls).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "ahttp/http.h"
#include "atls/pem.h"

/* Host stubs for guest-only functions.  dns_resolve is real enough to
 * map "localhost" to the loopback address used by the s_server. */
uint32_t dns_resolve(const char *h) {
    if (h && strcmp(h, "localhost") == 0) return 0x7f000001; /* 127.0.0.1 */
    return 0;
}
int closesocket(int fd) { return close(fd); }

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while (0)

/* ---- s_server management ---- */
static pid_t s_server_pid = 0;
static char cert_dir[256];
static char ca_pem[512], leaf_pem[512], leaf_key[512];
static int  test_port = 9443;

static void cleanup_s_server(void) {
    if (s_server_pid > 0) {
        kill(s_server_pid, SIGTERM);
        waitpid(s_server_pid, NULL, 0);
        s_server_pid = 0;
    }
}

/* Generate an ECDSA P-256 CA + leaf chain and start s_server. */
static int start_https_server(void) {
    snprintf(cert_dir, sizeof(cert_dir), "/tmp/ahttp_x2_%d", (int)getpid());
    snprintf(ca_pem, sizeof(ca_pem), "%s/ca.pem", cert_dir);
    snprintf(leaf_pem, sizeof(leaf_pem), "%s/leaf.pem", cert_dir);
    snprintf(leaf_key, sizeof(leaf_key), "%s/leaf.key", cert_dir);
    mkdir(cert_dir, 0700);

    char cmd[4096];
    /* CA (self-signed, P-256, CA:TRUE). */
    snprintf(cmd, sizeof(cmd),
        "openssl ecparam -name prime256v1 -genkey -noout -out %s/ca.key 2>/dev/null && "
        "openssl req -new -key %s/ca.key -x509 -out %s "
        "-days 10 -nodes -subj '/CN=X2 Test Root' "
        "-addext 'basicConstraints=critical,CA:TRUE' 2>/dev/null",
        cert_dir, cert_dir, ca_pem);
    if (system(cmd) != 0) return -1;

    /* Leaf (P-256, SAN localhost, signed by CA). */
    snprintf(cmd, sizeof(cmd),
        "openssl ecparam -name prime256v1 -genkey -noout -out %s 2>/dev/null && "
        "openssl req -new -key %s -out %s/leaf.csr -subj '/CN=localhost' 2>/dev/null && "
        "printf 'subjectAltName=DNS:localhost\\nbasicConstraints=CA:FALSE\\nkeyUsage=digitalSignature' > %s/ext.txt && "
        "openssl x509 -req -in %s/leaf.csr -CA %s -CAkey %s/ca.key -CAcreateserial "
        "-out %s -days 10 -extfile %s/ext.txt 2>/dev/null",
        leaf_key, leaf_key, cert_dir, cert_dir, cert_dir,
        ca_pem, cert_dir, leaf_pem, cert_dir);
    if (system(cmd) != 0) return -1;

    s_server_pid = fork();
    if (s_server_pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", test_port);
        execlp("openssl", "openssl", "s_server",
               "-accept", port_str,
               "-cert", leaf_pem, "-key", leaf_key,
               "-www", "-alpn", "http/1.1", "-quiet",
               (char *)NULL);
        _exit(1);
    }
    if (s_server_pid < 0) return -1;
    usleep(600000);
    return 0;
}

/* ---- Trust-root loader from the generated CA PEM ---- */
static atls_trust_root g_root;
static uint8_t g_root_der[8192];

static int load_ca_root(void) {
    FILE *f = fopen(ca_pem, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *pem = (char *)malloc((size_t)sz + 1);
    if (!pem) { fclose(f); return -1; }
    size_t n = fread(pem, 1, (size_t)sz, f);
    pem[n] = 0;
    fclose(f);

    size_t dlen = 0;
    if (atls_pem_cert_to_der(pem, n, g_root_der, sizeof(g_root_der), &dlen)
        != ATLS_OK) { free(pem); return -1; }
    g_root.der = g_root_der;
    g_root.der_len = dlen;
    free(pem);
    return 0;
}

/* ---- Tests ---- */

/* Current time + 1h, so freshly-generated certs are within validity. */
static atls_time_now get_now(void) {
    time_t t = time(NULL) + 3600;
    struct tm *tm = gmtime(&t);
    atls_time_now now = {
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec
    };
    return now;
}

static void test_https_valid_chain(void) {
    if (load_ca_root() != 0) { CHECK(0, "load CA root"); return; }
    atls_time_now now = get_now();
    ahttp_set_trust_roots(&g_root, 1, &now);

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/", test_port);
    ahttp_response *r = ahttp_get(url);
    CHECK(r != NULL, "https fetch returns a response");
    if (r) {
        CHECK(r->error == AHTTP_OK, "https fetch succeeds (chain verified)");
        CHECK(r->status_code == 200, "status 200 from s_server");
        printf("  [https] status=%d body=%zu bytes\n",
               r->status_code, r->body_len);
        ahttp_response_free(r);
    }
    ahttp_set_trust_roots(NULL, 0, NULL);
}

static void test_https_untrusted_chain(void) {
    /* No trust store: validation disabled, but CertificateVerify still
     * verifies, so the fetch should succeed (legacy behaviour). */
    ahttp_set_trust_roots(NULL, 0, NULL);
    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/", test_port);
    ahttp_response *r = ahttp_get(url);
    CHECK(r != NULL && r->error == AHTTP_OK,
          "https without trust store still fetches (CV-only)");
    if (r) ahttp_response_free(r);
}

static void test_https_wrong_root(void) {
    /* A trust store that does NOT contain the server's CA must cause the
     * chain validation to fail the handshake. */
    atls_trust_root bogus;
    uint8_t bogus_der[8192];
    memset(bogus_der, 0x11, sizeof(bogus_der));
    bogus.der = bogus_der;
    bogus.der_len = sizeof(bogus_der);
    atls_time_now now = get_now();
    ahttp_set_trust_roots(&bogus, 1, &now);

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/", test_port);
    ahttp_response *r = ahttp_get(url);
    CHECK(r != NULL && r->error == AHTTP_ERR_TLS,
          "https with wrong root is refused (chain validation)");
    if (r) ahttp_response_free(r);
    ahttp_set_trust_roots(NULL, 0, NULL);
}

int main(void) {
    printf("=== HTTPS client test (X2) ===\n");
    if (system("openssl version >/dev/null 2>&1") != 0) {
        printf("SKIP: openssl not available (X2 HTTPS gate)\n");
        return 0;   /* pass silently; CI has openssl, local may not */
    }
    if (start_https_server() != 0) {
        printf("SKIP: cannot start openssl s_server\n");
        return 0;
    }

    test_https_valid_chain();
    test_https_untrusted_chain();
    test_https_wrong_root();

    cleanup_s_server();
    printf("=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
