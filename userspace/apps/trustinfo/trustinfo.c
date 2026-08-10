/* trustinfo.c — REALINTERNET_PLAN X8: show the shipped trust store.
 *
 * Reads /etc/ssl/roots.pem and prints, for every root certificate, its
 * common name and its not-after expiry.  This is what makes "the chain moved
 * to a root we don't carry / a root we shipped has expired" read as a
 * diagnosable trust-store issue rather than a TLS bug (REALINTERNET_PLAN X8).
 *
 * It links libatls so it parses the same X.509 code the TLS stack uses.
 */

#include "atls/pem.h"
#include "atls/x509.h"
#include "atls/certval.h"
#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define MAX_ROOTS 16
#define DERBUF    (16 * 4096)

/* Extract the first commonName (2.5.4.3 = 0x55 0x04 0x03) string value from
 * a DER-encoded X.501 Name.  Returns 0 on success, -1 if not found.  The
 * result is NUL-terminated and truncated to fit the buffer. */
static int name_cn(const atls_span *name, char *out, size_t outlen) {
    if (outlen == 0) return -1;
    out[0] = '\0';
    if (!name->data || name->len < 4) return -1;
    /* scan the Name bytes for the commonName OID 55 04 03 */
    const uint8_t *p = name->data;
    size_t n = name->len;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0x55 && p[i + 1] == 0x04 && p[i + 2] == 0x03) {
            size_t j = i + 3;
            /* the value is a DER TLV: tag(1) len(variable) contents */
            if (j >= n) return -1;
            uint8_t tag = p[j++];
            size_t vlen = 0;
            if (j >= n) return -1;
            uint8_t lenb = p[j++];
            if (lenb & 0x80) {
                int nb = lenb & 0x7F;
                if (nb < 1 || nb > 4 || j + nb > n) return -1;
                for (int k = 0; k < nb; k++) vlen = (vlen << 8) | p[j++];
            } else {
                vlen = lenb;
            }
            if (j + vlen > n) return -1;
            size_t take = vlen;
            if (take >= outlen) take = outlen - 1;
            memcpy(out, p + j, take);
            out[take] = '\0';
            (void)tag;
            return 0;
        }
    }
    return -1;
}

static void print_date(const atls_x509_time *t, char *out, size_t outlen) {
    snprintf(out, outlen, "%04d-%02d-%02d %02d:%02d:%02d",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
}

int main(void) {
    static char pem[16384];
    static uint8_t der[DERBUF];
    static atls_trust_root roots[MAX_ROOTS];

    printf("AuraLite OS trust store: /etc/ssl/roots.pem\n");
    printf("  %-30s  %-28s  %s\n", "common name", "not-after (UTC)", "provenance");

    int fd = open("/etc/ssl/roots.pem", 0);
    if (fd < 0) {
        puts("  (cannot open /etc/ssl/roots.pem — trust store missing)");
        return 1;
    }
    size_t n = 0;
    for (;;) {
        ssize_t got = read(fd, pem + n, sizeof(pem) - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
        if (n >= sizeof(pem) - 1) break;
    }
    close(fd);
    pem[n] = 0;

    int count = 0;
    size_t pos = 0, boff = 0;
    while (pos < n) {
        size_t dlen = 0;
        int rc = atls_pem_cert_to_der(pem + pos, n - pos,
                                      der + boff, DERBUF - boff, &dlen);
        if (rc != ATLS_OK) break;
        if (count >= MAX_ROOTS) break;
        roots[count].der = der + boff;
        roots[count].der_len = dlen;
        boff += dlen;
        count++;

        atls_x509_cert c;
        if (atls_x509_parse(roots[count - 1].der, roots[count - 1].der_len,
                            &c) == ATLS_OK) {
            char cn[64], na[32];
            name_cn(&c.subject, cn, sizeof(cn));
            print_date(&c.not_after, na, sizeof(na));
            printf("  %-30s  %-28s  shipped root\n", cn, na);
        } else {
            printf("  %-30s  %-28s  (unparseable)\n", "<root>", "?");
        }

        const char *e = strstr(pem + pos, "-----END CERTIFICATE-----");
        if (!e) break;
        pos = (size_t)(e - pem) + strlen("-----END CERTIFICATE-----") + 1;
    }

    if (count == 0) {
        puts("  (no certificates decoded)");
        return 1;
    }
    printf("%d trust root(s).\n", count);
    puts("  See docs/trust_store.md for provenance, sources and expiry.");
    return 0;
}
