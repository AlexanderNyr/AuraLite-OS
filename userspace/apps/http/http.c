/*
 * http.c — HTTP/HTTPS client for AuraLite OS (REALINTERNET_PLAN X2).
 *
 * Built on libahttp (ahttp_get), which handles HTTP/1.1 and HTTPS/TLS 1.3
 * with full certificate chain validation against the shipped trust store
 * (/etc/ssl/roots.pem).
 *
 * Usage: run /http
 * Then type a URL (e.g. http://example.com/ or https://example.com/)
 * to fetch it.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "ahttp/http.h"
#include "atls/pem.h"

#define MAX_URL 512
static char g_url[MAX_URL];

/* Load the trust store from a PEM file into caller-owned arrays.
 * Returns 0 on success, -1 if it cannot be read or decoded. */
static int load_trust_roots(const char *path, atls_trust_root *roots,
                            uint8_t *derbuf, int max_roots,
                            size_t derbuf_size, int *out_count) {
    int fd = open(path, 0);
    if (fd < 0) return -1;
    char pem[16384];
    size_t n = 0;
    for (;;) {
        ssize_t got = read(fd, pem + n, sizeof(pem) - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
        if (n >= sizeof(pem) - 1) break;
    }
    close(fd);
    pem[n] = 0;

    /* Decode each CERTIFICATE block.  The caller's DER buffer is one
     * contiguous region; each root points into a slice of it. */
    int count = 0;
    size_t pos = 0;
    size_t boff = 0;
    while ((size_t)n > pos) {
        size_t dlen = 0;
        int rc = atls_pem_cert_to_der(pem + pos, (size_t)n - pos,
                                      derbuf + boff, derbuf_size - boff,
                                      &dlen);
        if (rc != ATLS_OK) break;
        if (count >= max_roots) break;
        roots[count].der = derbuf + boff;
        roots[count].der_len = dlen;
        boff += dlen;
        count++;
        /* Advance past this block's END marker. */
        const char *e = strstr(pem + pos, "-----END CERTIFICATE-----");
        if (!e) break;
        pos = (size_t)(e - pem) + strlen("-----END CERTIFICATE-----") + 1;
    }
    *out_count = count;
    return count > 0 ? 0 : -1;
}

/* Fetch a single URL non-interactively and print the result. */
static void fetch_once(const char *url) {
    printf("Fetching %s ...\n", url);
    ahttp_response *r = ahttp_get(url);
    if (!r) { puts("Error: no response object (out of memory?)."); return; }
    if (r->error != AHTTP_OK) {
        printf("Fetch failed (error %d)\n", r->error);
        ahttp_response_free(r);
        return;
    }
    printf("--- Response: %d, %u bytes body ---\n",
           r->status_code, (unsigned)r->body_len);
    int show = r->body_len < 1000 ? (int)r->body_len : 1000;
    if (show > 0) write(1, r->body, (size_t)show);
    if (r->body_len > 1000)
        printf("\n... (%u more bytes truncated)\n",
               (unsigned)(r->body_len - 1000));
    puts("\n--- End ---");
    ahttp_response_free(r);
}

int main(int argc, char **argv) {
    /* Non-interactive mode: http [roots.pem] <url>.  Used by the X2
     * integration test, which points the trust store at a test CA. */
    if (argc == 2) {
        const char *url = argv[1];
        static atls_trust_root roots[16];
        static uint8_t root_der[16384];
        int num_roots = 0;
        if (load_trust_roots("/etc/ssl/roots.pem", roots, root_der, 16,
                             sizeof(root_der), &num_roots) == 0) {
            ahttp_set_trust_roots(roots, num_roots, NULL);
        }
        fetch_once(url);
        return 0;
    }
    if (argc == 3) {
        const char *roots_path = argv[1];
        const char *url = argv[2];
        static atls_trust_root roots[16];
        static uint8_t root_der[16384];
        int num_roots = 0;
        if (load_trust_roots(roots_path, roots, root_der, 16,
                             sizeof(root_der), &num_roots) == 0) {
            ahttp_set_trust_roots(roots, num_roots, NULL);
        }
        fetch_once(url);
        return 0;
    }

    puts("=== AuraLite HTTP Client (HTTP/1.1 + HTTPS) ===");
    puts("Type a URL to fetch, e.g. https://example.com/");
    puts("Or type 'quit' to exit.");
    puts("");

    /* Load the trust store once, at startup. */
    static atls_trust_root roots[16];
    static uint8_t root_der[16384];
    int num_roots = 0;
    if (load_trust_roots("/etc/ssl/roots.pem", roots, root_der, 16,
                         sizeof(root_der), &num_roots) == 0) {
        printf("Loaded %d trust root(s) from /etc/ssl/roots.pem\n",
               num_roots);
        ahttp_set_trust_roots(roots, num_roots, NULL);
    } else {
        puts("WARNING: could not load /etc/ssl/roots.pem — ");
        puts("HTTPS will verify only the server's signature.");
    }

    for (;;) {
        write(1, "fetch> ", 7);
        int64_t n = read(0, g_url, sizeof(g_url) - 1);
        if (n <= 0) continue;
        g_url[n] = 0;
        if (n > 0 && g_url[n-1] == '\n') g_url[n-1] = 0;
        if (g_url[0] == 0) continue;

        if (strcmp(g_url, "quit") == 0 || strcmp(g_url, "q") == 0) break;

        /* Normalise: if no scheme given, default to http:// */
        if (strncmp(g_url, "http://", 7) != 0 &&
            strncmp(g_url, "https://", 8) != 0) {
            char tmp[MAX_URL];
            snprintf(tmp, sizeof(tmp), "http://%s", g_url);
            strncpy(g_url, tmp, sizeof(g_url) - 1);
            g_url[sizeof(g_url) - 1] = 0;
        }

        printf("Fetching %s ...\n", g_url);
        ahttp_response *r = ahttp_get(g_url);
        if (!r) {
            puts("Error: no response object (out of memory?).");
            continue;
        }
        if (r->error != AHTTP_OK) {
            printf("Fetch failed (error %d)\n", r->error);
            ahttp_response_free(r);
            continue;
        }
        printf("--- Response: %d, %u bytes body ---\n",
               r->status_code, (unsigned)r->body_len);
        int show = r->body_len < 1000 ? (int)r->body_len : 1000;
        if (show > 0) write(1, r->body, (size_t)show);
        if (r->body_len > 1000)
            printf("\n... (%u more bytes truncated)\n",
                   (unsigned)(r->body_len - 1000));
        puts("\n--- End ---");
        ahttp_response_free(r);
        puts("");
    }

    puts("Goodbye from HTTP client!");
    return 0;
}
