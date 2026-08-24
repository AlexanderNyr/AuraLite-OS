/*
 * https6.c — REALINTERNET2 Y4 guest gate.
 *
 * Fetches an HTTPS URL through libahttp.  The integration case points
 * this at https://[fec0::2]:8446/ (SLIRP's ipv6-host, same-port
 * listener as Y3's tcp6).  Receipt:
 *   [https6] PASS: status N body M via v6
 *
 * Usage: run https6 [url]
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ahttp/http.h"

int main(int argc, char **argv) {
    const char *url = (argc >= 2 && argv[1] && argv[1][0])
        ? argv[1] : "https://[fec0::2]:8446/";
    printf("[https6] fetching %s\n", url);
    ahttp_response *r = ahttp_get(url);
    if (!r) {
        printf("[https6] FAIL: no response object\n");
        return 1;
    }
    if (r->error != AHTTP_OK) {
        printf("[https6] FAIL: ahttp error %d\n", r->error);
        ahttp_response_free(r);
        return 1;
    }
    if (r->status_code < 200 || r->status_code >= 300) {
        printf("[https6] FAIL: HTTP %d (body %u)\n",
               r->status_code, (unsigned)r->body_len);
        ahttp_response_free(r);
        return 1;
    }
    printf("[https6] PASS: status %d body %u via v6\n",
           r->status_code, (unsigned)r->body_len);
    ahttp_response_free(r);
    return 0;
}
