/*
 * http.c — HTTP/HTTPS client for AuraLite OS (REALINTERNET_PLAN X2, X6).
 *
 * Built on libahttp's keep-alive client (ahttp_client), which handles
 * HTTP/1.1 and HTTPS/TLS 1.3 with full certificate chain validation
 * against the shipped trust store (/etc/ssl/roots.pem), connection
 * reuse, POST/PUT bodies and relative redirect resolution.
 *
 * Usage: run /http
 * Then type a URL (e.g. http://example.com/ or https://example.com/)
 * to fetch it.  Repeated fetches against the same origin reuse the
 * socket ([ahttp] keep-alive lines land on the serial log).
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "ahttp/http.h"

#define MAX_URL 512
static char g_url[MAX_URL];

/* One client for the process lifetime: this is what makes interactive
 * re-fetches to the same host reuse the connection (X6). */
static ahttp_client *g_client;

/* Fetch a single URL non-interactively and print the result. */
static void fetch_once(const char *url) {
    printf("Fetching %s ...\n", url);
    ahttp_response *r = ahttp_client_get(g_client, url);
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

/* Attach a PEM trust store to the client.  Returns the root count, 0
 * when the file cannot be used (CertificateVerify-only fallback). */
static int client_load_roots(ahttp_client *c, const char *path) {
    static atls_trust_root roots[16];
    static uint8_t root_der[16384];
    int num_roots = 0;
    if (ahttp_load_trust_roots(path, roots, root_der, 16,
                               sizeof(root_der), &num_roots) == 0) {
        ahttp_client_set_trust_roots(c, roots, num_roots, NULL);
        return num_roots;
    }
    return 0;
}

int main(int argc, char **argv) {
    g_client = ahttp_client_new();
    if (!g_client) {
        puts("Error: out of memory.");
        return 1;
    }

    /* Non-interactive mode: http [roots.pem] <url>.  Used by the X2
     * integration test, which points the trust store at a test CA. */
    if (argc == 2) {
        const char *url = argv[1];
        client_load_roots(g_client, "/etc/ssl/roots.pem");
        fetch_once(url);
        ahttp_client_free(g_client);
        return 0;
    }
    if (argc == 3) {
        const char *roots_path = argv[1];
        const char *url = argv[2];
        client_load_roots(g_client, roots_path);
        fetch_once(url);
        ahttp_client_free(g_client);
        return 0;
    }

    puts("=== AuraLite HTTP Client (HTTP/1.1 + HTTPS) ===");
    puts("Type a URL to fetch, e.g. https://example.com/");
    puts("Or type 'quit' to exit.");
    puts("");

    /* Load the trust store once, at startup. */
    int num_roots = client_load_roots(g_client, "/etc/ssl/roots.pem");
    if (num_roots > 0) {
        printf("Loaded %d trust root(s) from /etc/ssl/roots.pem\n",
               num_roots);
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

        fetch_once(g_url);
        puts("");
    }

    puts("Goodbye from HTTP client!");
    ahttp_client_free(g_client);
    return 0;
}
