/* ahttp.c — HTTP/1.1 client over plain TCP or TLS (INTERNET_PLAN.md N6,
 * REALINTERNET_PLAN.md X2 + X6).
 *
 * X6 ("HTTP completeness"):
 *   - Persistent connections: ahttp_client caches one live connection per
 *     origin (scheme+host+port) and reuses it, logging every reuse/reopen
 *     with a "[ahttp] keep-alive:" line so the integration gate can assert
 *     socket reuse from the serial log.  Stale sockets are transparently
 *     reopened exactly once for idempotent methods (RFC 7230 6.3.1).
 *   - Request bodies: POST/PUT with Content-Length behind the bounded
 *     AHTTP_MAX_REQ_BODY interface.
 *   - Redirect resolution that matches what real servers send: absolute,
 *     protocol-relative, absolute-path and relative Location values, with
 *     RFC 3986 dot-segment removal.  Only http/https targets are followed.
 *   - ahttp_get(url) keeps its one-shot semantics on top of a throwaway
 *     client, so pre-X6 callers behave exactly as before.
 *
 * Uses BSD sockets internally and libatls for TLS.  Never touches the
 * kernel's legacy net_* API.
 */

#include "ahttp/http.h"
#include "atls/atls.h"
#include "atls/tls.h"
#include "atls/pem.h"
#include "atls/certval.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Heap (freestanding guest libc). */
extern void *malloc(size_t);
extern void  free(void *);
extern void *realloc(void *, size_t);

/* Socket/POSIX (guest libc has different signatures). */
#ifdef __AURALITE__
extern int socket(int, int, int);
extern int connect(int, uint32_t, uint16_t);
extern int send(int, const void *, uint32_t);
extern int recv(int, void *, uint32_t);
extern int closesocket(int);
extern int getentropy(void *, size_t);
#include "unistd.h"   /* open/read/close + connectaddr */
#include "sys/socket.h"
#include "netinet/in.h"
#else
/* Host: use standard POSIX. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define closesocket close
static int host_connect_ip(int fd, uint32_t ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    uint32_t ip_be = htonl(ip);
    memcpy(&addr.sin_addr, &ip_be, 4);
    return connect(fd, (struct sockaddr *)&addr, sizeof(addr));
}
static int host_send(int fd, const void *data, size_t len) {
    /* MSG_NOSIGNAL: a peer that already closed must not kill the whole
     * host process with SIGPIPE while we probe a stale keep-alive socket. */
    return send(fd, data, len, MSG_NOSIGNAL);
}
static int host_recv(int fd, void *buf, size_t cap) {
    return recv(fd, buf, cap, 0);
}
#endif

/* DNS resolver (AuraLite libc). */
extern uint32_t dns_resolve(const char *hostname);
#ifdef __AURALITE__
extern int dns_resolve_aaaa(const char *hostname, uint8_t out[16]);
#else
/* Host tests stub AAAA unless they provide their own. */
static int dns_resolve_aaaa(const char *hostname, uint8_t out[16]) {
    (void)hostname; (void)out;
    return -1;
}
#endif

#include "kernel/net/dualstack.h"

/* ---- small ascii helpers (case-insensitive, NUL-safe) ---- */

static char ci_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}
static int ci_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (ci_lower(*s) != ci_lower(*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

/* IP in host byte order (AuraLite format: a<<24 | b<<16 | c<<8 | d). */
static uint32_t parse_ip(const char *s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

/* RFC 4291 textual IPv6 (including ::).  Returns 0 and writes 16 octets. */
static int parse_ip6(const char *s, uint8_t out[16]) {
    unsigned parts[8];
    int n = 0, skip = -1;
    if (!s || !out) return -1;
    if (*s == ':') {
        if (s[1] != ':') return -1;
        skip = 0;
        s += 2;
        if (!*s) {
            memset(out, 0, 16);
            return 0;
        }
    }
    while (*s) {
        if (*s == ':') {
            if (skip >= 0) return -1;
            skip = n;
            s++;
            if (!*s) break;
            continue;
        }
        unsigned v = 0;
        int digits = 0;
        while (*s && *s != ':') {
            int h = -1;
            if (*s >= '0' && *s <= '9') h = *s - '0';
            else if (*s >= 'a' && *s <= 'f') h = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') h = *s - 'A' + 10;
            else return -1;
            v = (v << 4) | (unsigned)h;
            if (v > 0xFFFFu) return -1;
            s++;
            digits++;
            if (digits > 4) return -1;
        }
        if (n >= 8) return -1;
        parts[n++] = v;
        if (*s == ':') {
            s++;
            if (!*s) return -1;
        }
    }
    if (skip < 0) {
        if (n != 8) return -1;
    } else {
        int z = 8 - n;
        if (z <= 0) return -1;
        for (int i = n - 1; i >= skip; i--) parts[i + z] = parts[i];
        for (int i = 0; i < z; i++) parts[skip + i] = 0;
    }
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = (uint8_t)(parts[i] >> 8);
        out[i * 2 + 1] = (uint8_t)(parts[i] & 0xFF);
    }
    return 0;
}

static int ahttp_connect_v4(int *fd_out, uint32_t ip, uint16_t port) {
#ifdef __AURALITE__
    int fd = socket(2, 1, 0);
    if (fd < 0) return -1;
    if (connect(fd, ip, port) != 0) { closesocket(fd); return -1; }
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (host_connect_ip(fd, ip, port) != 0) { close(fd); return -1; }
#endif
    *fd_out = fd;
    return 0;
}

static int ahttp_connect_v6(int *fd_out, const uint8_t addr[16], uint16_t port) {
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons((uint16_t)port);
    memcpy(sa.sin6_addr.s6_addr, addr, 16);
#ifdef __AURALITE__
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connectaddr(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        closesocket(fd);
        return -1;
    }
#else
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
#endif
    *fd_out = fd;
    return 0;
}

/* ---- URL parsing ---- */

int ahttp_url_parse(const char *url, ahttp_url *out) {
    if (!url || !out) return AHTTP_ERR_URL;
    memset(out, 0, sizeof(*out));

    /* Scheme. */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->port = 80;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        strcpy(out->scheme, "https");
        out->port = 443;
        p += 8;
    } else {
        return AHTTP_ERR_URL;
    }

    /* Host[:port], or RFC 3986 [IPv6][:port].  A colon inside the
     * brackets is the address, not the port separator. */
    if (*p == '[') {
        p++;
        const char *host_start = p;
        while (*p && *p != ']') p++;
        if (*p != ']') return AHTTP_ERR_URL;
        size_t host_len = (size_t)(p - host_start);
        if (host_len == 0 || host_len >= sizeof(out->host)) return AHTTP_ERR_URL;
        for (size_t i = 0; i < host_len; i++) out->host[i] = host_start[i];
        out->host[host_len] = 0;
        p++; /* skip ']' */
        if (*p == ':') {
            p++;
            out->port = 0;
            int saw_digit = 0;
            while (*p >= '0' && *p <= '9') {
                out->port = out->port * 10 + (*p - '0');
                p++;
                saw_digit = 1;
            }
            if (!saw_digit) return AHTTP_ERR_URL;
        }
    } else {
        const char *host_start = p;
        while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
        size_t host_len = (size_t)(p - host_start);
        if (host_len == 0 || host_len >= sizeof(out->host)) return AHTTP_ERR_URL;
        for (size_t i = 0; i < host_len; i++) out->host[i] = host_start[i];
        out->host[host_len] = 0;

        if (*p == ':') {
            p++;
            out->port = 0;
            while (*p >= '0' && *p <= '9') {
                out->port = out->port * 10 + (*p - '0');
                p++;
            }
        }
    }

    /* Path. */
    if (*p == '/' || *p == '?' || *p == '#') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(out->path)) path_len = sizeof(out->path) - 1;
        for (size_t i = 0; i < path_len; i++) out->path[i] = p[i];
        out->path[path_len] = 0;
    } else {
        strcpy(out->path, "/");
    }

    return AHTTP_OK;
}

/* ---- Trust store (REALINTERNET_PLAN X2) ---- */
static const atls_trust_root *g_roots;
static int                    g_num_roots;
static const atls_time_now   *g_now;

void ahttp_set_trust_roots(const atls_trust_root *roots, int num_roots,
                           const atls_time_now *now) {
    g_roots = roots;
    g_num_roots = num_roots;
    g_now = now;
}

/* ---- Trust-store loader (X6) ----
 * Shared by /http, gbrowser and any future client so the PEM walk lives
 * in exactly one place. */

int ahttp_load_trust_roots(const char *path, atls_trust_root *roots,
                           uint8_t *derbuf, int max_roots,
                           size_t derbuf_size, int *out_count) {
    if (!path || !roots || !derbuf || !out_count || max_roots <= 0) return -1;
    int fd = open(path, 0);
    if (fd < 0) return -1;
    static char pem[16384];
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
    while (pos < n) {
        size_t dlen = 0;
        int rc = atls_pem_cert_to_der(pem + pos, n - pos,
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

/* ---- Transport layer (plain TCP or TLS) ---- */

typedef struct {
    int fd;
    int is_tls;
    atls_tls *tls;
    /* Buffered socket reader for the TLS transport callbacks. */
    uint8_t rbuf[4096];
    size_t  rlen, roff;
} transport;

/* TLS transport callbacks: adapt the socket to libatls's send/recv
 * interface (REALINTERNET_PLAN X2).  recv buffers one socket read so TLS
 * records spanning TCP segments and multiple records per segment both work. */
static int tls_send_cb(void *io, const uint8_t *data, size_t len) {
    transport *t = io;
    size_t off = 0;
    while (off < len) {
#ifdef __AURALITE__
        int n = send(t->fd, data + off, (uint32_t)(len - off));
#else
        int n = host_send(t->fd, data + off, len - off);
#endif
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (int)len;
}
static int tls_recv_cb(void *io, uint8_t *data, size_t cap) {
    transport *t = io;
    if (t->roff >= t->rlen) {
#ifdef __AURALITE__
        int n = recv(t->fd, t->rbuf, sizeof(t->rbuf));
#else
        int n = host_recv(t->fd, t->rbuf, sizeof(t->rbuf));
#endif
        if (n <= 0) return n;
        t->rlen = (size_t)n;
        t->roff = 0;
    }
    size_t n = t->rlen - t->roff;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) data[i] = t->rbuf[t->roff + i];
    t->roff += n;
    return (int)n;
}

static int transport_connect(transport *t, const char *host, int port, int use_tls,
                             const atls_trust_root *roots, int num_roots,
                             const atls_time_now *now) {
    t->is_tls = use_tls;
    t->tls = NULL;

    /* Y4: resolve A + AAAA, pick (v6 preferred when an AAAA exists),
     * dial v6, fall back to v4 serially. */
    uint8_t aaaa[16];
    int have_aaaa = 0, have_a = 0;
    uint32_t ip4 = parse_ip(host);
    if (ip4) have_a = 1;
    if (parse_ip6(host, aaaa) == 0) have_aaaa = 1;
    if (!have_a && !have_aaaa) {
        ip4 = dns_resolve(host);
        if (ip4) have_a = 1;
        if (dns_resolve_aaaa(host, aaaa) == 0) have_aaaa = 1;
    }
    if (!have_a && !have_aaaa) return AHTTP_ERR_DNS;

    int pick = dualstack_pick(have_aaaa ? 1 : 0, have_aaaa, have_a);
    t->fd = -1;
    if (pick == DS_V6) {
        printf("[ahttp] dial v6 %s:%d\n", host, port);
        if (ahttp_connect_v6(&t->fd, aaaa, (uint16_t)port) != 0) {
            if (have_a) {
                printf("[ahttp] v6 failed, falling back to v4\n");
                if (ahttp_connect_v4(&t->fd, ip4, (uint16_t)port) != 0)
                    return AHTTP_ERR_CONNECT;
            } else {
                return AHTTP_ERR_CONNECT;
            }
        }
    } else if (pick == DS_V4) {
        printf("[ahttp] dial v4 %s:%d\n", host, port);
        if (ahttp_connect_v4(&t->fd, ip4, (uint16_t)port) != 0)
            return AHTTP_ERR_CONNECT;
    } else {
        return AHTTP_ERR_DNS;
    }

    /* TLS handshake (REALINTERNET_PLAN X2). */
    if (use_tls) {
        atls_tls_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        /* SNI still needs a name (atls_tls_new refuses NULL).  CATCH,
         * named at TLS: atls hostname match is DNS-SAN only, so an
         * IP-literal fetch would fail ATLS_CERTVAL_ERR_HOSTNAME if a
         * trust store were loaded.  Guest https6 carries no roots —
         * CertificateVerify still runs; SAN match is skipped. */
        cfg.hostname = host;
        cfg.alpn = "http/1.1";
        cfg.roots = roots;
        cfg.num_roots = num_roots;
        cfg.now = now;
        t->tls = atls_tls_new(&cfg, tls_send_cb, tls_recv_cb, t);
        if (!t->tls) { closesocket(t->fd); return AHTTP_ERR_TLS; }
        int hrc = atls_tls_handshake(t->tls);
        if (hrc != ATLS_OK) {
            /* X8: surface the *reason* for a certificate-validation failure,
             * not a generic handshake error — an untrusted chain must read as
             * "root not in trust store", not "TLS broken". */
            if (hrc == ATLS_CERTVAL_ERR_UNKNOWN_ROOT) {
                printf("[ahttp] TLS: server chain root is not in the trust "
                       "store (root not in trust store)\n");
            } else if (hrc == ATLS_CERTVAL_ERR_EXPIRED) {
                printf("[ahttp] TLS: certificate chain expired/not yet valid\n");
            } else if (hrc == ATLS_CERTVAL_ERR_HOSTNAME) {
                printf("[ahttp] TLS: certificate hostname mismatch\n");
            } else {
                printf("[ahttp] TLS handshake failed hrc=%d alert_sent=%d "
                       "alert_recv=%d\n", hrc,
                       atls_tls_last_alert_sent(t->tls),
                       atls_tls_last_alert_received(t->tls));
            }
            atls_tls_free(t->tls);
            t->tls = NULL;
            closesocket(t->fd);
            return AHTTP_ERR_TLS;
        }
    }

    return AHTTP_OK;
}

static int transport_send(transport *t, const void *data, size_t len) {
    if (t->is_tls && t->tls) {
        return atls_tls_write(t->tls, data, len);
    }
    size_t off = 0;
    while (off < len) {
#ifdef __AURALITE__
        int n = send(t->fd, (const uint8_t *)data + off, (uint32_t)(len - off));
#else
        int n = host_send(t->fd, (const uint8_t *)data + off, len - off);
#endif
        if (n <= 0) return AHTTP_ERR_REQUEST;
        off += (size_t)n;
    }
    return (int)len;
}

static int transport_recv(transport *t, void *buf, size_t cap) {
    if (t->is_tls && t->tls) {
        size_t n = 0;
        int rc = atls_tls_read(t->tls, buf, cap, &n);
        if (rc != ATLS_OK) return -1;
        return (int)n;
    }
#ifdef __AURALITE__
    return recv(t->fd, buf, (uint32_t)cap);
#else
    return host_recv(t->fd, buf, cap);
#endif
}

static void transport_close(transport *t) {
    if (t->tls) { atls_tls_close(t->tls); atls_tls_free(t->tls); t->tls = NULL; }
    if (t->fd >= 0) { closesocket(t->fd); t->fd = -1; }
}

/* ---- Buffered reader ----
 * The reader MUST live alongside the cached connection (X6): a plain-TCP
 * fill can prefetch bytes of the next response only when we over-read,
 * which the framing code never does — but the TLS callback layer does
 * legitimately hold leftover record bytes in transport.rbuf.  Keeping rbuf
 * AND the reader state inside the cached connection makes reuse correct
 * for both transports. */

typedef struct {
    transport *t;
    uint8_t buf[4096];
    size_t len, off;
} reader;

static void reader_init(reader *r, transport *t) {
    r->t = t; r->len = 0; r->off = 0;
}

static int reader_fill(reader *r) {
    if (r->off < r->len) return 1;
    int n = transport_recv(r->t, r->buf, sizeof(r->buf));
    if (n <= 0) return n;
    r->len = (size_t)n;
    r->off = 0;
    return 1;
}

static int reader_read(reader *r, void *dst, size_t n) {
    uint8_t *p = dst;
    size_t done = 0;
    while (done < n) {
        if (r->off >= r->len) {
            int rc = reader_fill(r);
            if (rc <= 0) return done > 0 ? (int)done : rc;
        }
        size_t take = r->len - r->off;
        if (take > n - done) take = n - done;
        for (size_t i = 0; i < take; i++) p[done + i] = r->buf[r->off + i];
        r->off += take;
        done += take;
    }
    return (int)done;
}

/* Read one line (up to \n, inclusive). Returns length or -1. */
static int reader_readline(reader *r, char *dst, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        if (r->off >= r->len) {
            int rc = reader_fill(r);
            if (rc <= 0) { if (pos > 0) break; return -1; }
        }
        dst[pos++] = (char)r->buf[r->off++];
        if (dst[pos - 1] == '\n') break;
    }
    dst[pos] = 0;
    return (int)pos;
}

/* ---- Growing buffer ---- */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} growbuf;

static int growbuf_init(growbuf *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = (uint8_t *)malloc(b->cap);
    return b->data ? 0 : -1;
}

static int growbuf_append(growbuf *b, const uint8_t *data, size_t len) {
    if (b->len + len > AHTTP_MAX_BODY) return AHTTP_ERR_TOO_LARGE;
    while (b->len + len > b->cap) {
        size_t newcap = b->cap * 2;
        if (newcap > AHTTP_MAX_BODY) newcap = AHTTP_MAX_BODY;
        uint8_t *p = (uint8_t *)realloc(b->data, newcap);
        if (!p) return AHTTP_ERR_NOMEM;
        b->data = p;
        b->cap = newcap;
    }
    for (size_t i = 0; i < len; i++) b->data[b->len + i] = data[i];
    b->len += len;
    return 0;
}

/* ---- HTTP response parsing ---- */

/* Parse status line: "HTTP/1.x NNN reason\r\n" */
static int parse_status_line(const char *line) {
    if (strncmp(line, "HTTP/1.", 7) != 0) return -1;
    const char *p = line + 7;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return -1;
    p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    return code;
}

/* Find a header value (case-insensitive). Returns pointer to value or NULL. */
static const char *find_header(const char *headers, const char *name) {
    size_t name_len = strlen(name);
    const char *p = headers;
    while (*p) {
        /* Case-insensitive compare. */
        int match = 1;
        for (size_t i = 0; i < name_len; i++) {
            char a = p[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ') p++;
            return p;
        }
        /* Skip to next line. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

/* Does this header value (a possibly comma-separated token list) contain
 * the given token, case-insensitively?  Token-aware so "keep-alive, close"
 * and "Close" both register. */
static int header_has_token(const char *value, const char *token) {
    if (!value) return 0;
    size_t tl = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t run = (size_t)(end - p);
        while (run > 0 && (p[run - 1] == ' ' || p[run - 1] == '\r' ||
                           p[run - 1] == '\n')) run--;
        if (run == tl) {
            int match = 1;
            for (size_t i = 0; i < tl; i++) {
                if (ci_lower(p[i]) != ci_lower(token[i])) { match = 0; break; }
            }
            if (match) return 1;
        }
        p = (*end == ',') ? end + 1 : end;
    }
    return 0;
}

/* Read headers into a growbuf. Returns status code or -1. */
static int read_headers(reader *r, growbuf *hb) {
    char line[4096];
    int status = -1;
    while (1) {
        int n = reader_readline(r, line, sizeof(line));
        if (n <= 0) return -1;
        /* End of headers. */
        if (line[0] == '\r' && line[1] == '\n') break;
        if (line[0] == '\n') break;
        /* First line is status line. */
        if (status < 0) {
            status = parse_status_line(line);
            if (status < 0) return -1;
        }
        growbuf_append(hb, (const uint8_t *)line, (size_t)n);
    }
    return status;
}

/* Read body with Content-Length. */
static int read_body_content_length(reader *r, growbuf *bb, size_t content_len) {
    while (bb->len < content_len) {
        uint8_t tmp[4096];
        size_t want = content_len - bb->len;
        if (want > sizeof(tmp)) want = sizeof(tmp);
        int n = reader_read(r, tmp, want);
        if (n <= 0) break;
        int rc = growbuf_append(bb, tmp, (size_t)n);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Read body with chunked transfer encoding. */
static int read_body_chunked(reader *r, growbuf *bb) {
    char line[256];
    while (1) {
        int n = reader_readline(r, line, sizeof(line));
        if (n <= 0) return AHTTP_ERR_RESPONSE;
        /* Parse chunk size (hex). */
        size_t chunk_size = 0;
        for (const char *p = line; *p && *p != '\r' && *p != '\n'; p++) {
            char c = *p;
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) break;
            chunk_size = chunk_size * 16 + (size_t)v;
        }
        if (chunk_size == 0) {
            /* Last chunk. Skip trailing CRLF. */
            reader_readline(r, line, sizeof(line));
            break;
        }
        /* Read chunk data. */
        int rc = read_body_content_length(r, bb, bb->len + chunk_size);
        if (rc != 0) return rc;
        /* Skip trailing CRLF. */
        reader_readline(r, line, sizeof(line));
    }
    return 0;
}

/* ---- Redirect resolution (X6) ---- */

/* RFC 3986 5.2.2 remove_dot_segments, in place over a NUL-terminated
 * path string (query must already be cut off by the caller). */
static void remove_dot_segments(char *path) {
    char in[2048];
    char out[2048];
    size_t ilen = strlen(path);
    if (ilen >= sizeof(in)) return;   /* pathological; leave untouched */
    memcpy(in, path, ilen + 1);
    size_t ip = 0, op = 0;
    out[0] = 0;
    while (ip < ilen) {
        /* A: "../" or "./" prefix → drop */
        if (in[ip] == '.' && in[ip+1] == '.' && in[ip+2] == '/') { ip += 3; continue; }
        if (in[ip] == '.' && in[ip+1] == '/') { ip += 2; continue; }
        /* B: "/./" → "/", or trailing "/." → "/" */
        if (in[ip] == '/' && in[ip+1] == '.' && in[ip+2] == '/') { ip += 2; continue; }
        if (in[ip] == '/' && in[ip+1] == '.' && in[ip+2] == 0) { ip += 1; in[ip] = '/'; in[ip+1] = 0; ilen = ip + 1; continue; }
        /* C: "/../" → "/" + pop last output segment, ditto trailing "/.." */
        if (in[ip] == '/' && in[ip+1] == '.' && in[ip+2] == '.' &&
            (in[ip+3] == '/' || in[ip+3] == 0)) {
            int trailing = (in[ip+3] == 0);
            ip += 3;
            if (trailing) { in[ip-1] = '/'; in[ip] = 0; ilen = ip; }
            /* remove last segment from out */
            if (op > 0) {
                size_t o = op;
                /* spec keeps a leading '/' rooted at position 0 */
                while (o > 0 && out[o-1] != '/') o--;
                op = (o > 0) ? o - 1 : 0;
                /* keep the slash we stopped at */
                if (op > 0 && out[op] != '/') { /* nothing */ }
            }
            out[op] = 0;
            continue;
        }
        /* D: bare "." or ".." → drop */
        if (in[ip] == '.' && in[ip+1] == 0) { ip += 1; continue; }
        if (in[ip] == '.' && in[ip+1] == '.' && in[ip+2] == 0) { ip += 2; continue; }
        /* E: copy first path segment (including leading '/') */
        {
            size_t start = ip;
            size_t j = ip;
            if (in[j] == '/') j++;
            while (in[j] && in[j] != '/') j++;
            size_t seg = j - start;
            if (op + seg >= sizeof(out)) break;
            memcpy(out + op, in + start, seg);
            op += seg;
            out[op] = 0;
            ip = j;
        }
    }
    strcpy(path, out);
}

/* Append helper with bound check. */
static int rcat(char *out, size_t cap, size_t *pos, const char *s) {
    while (*s) {
        if (*pos + 1 >= cap) return -1;
        out[(*pos)++] = *s++;
    }
    out[*pos] = 0;
    return 0;
}

int ahttp_resolve_redirect(char *out, size_t cap,
                           const char *current_url, const char *location) {
    if (!out || !current_url || !location || cap < 16) return AHTTP_ERR_REDIRECT;
    out[0] = 0;

    /* Strip surrounding whitespace/control chars servers love to add.
     * The value comes from find_header() as a pointer INTO the header
     * block, so it runs into the following header lines — cut it at the
     * first CR/LF before doing anything else. */
    while (*location == ' ' || *location == '\t') location++;
    const char *vend = location;
    while (*vend && *vend != '\r' && *vend != '\n') vend++;
    size_t loc_len = (size_t)(vend - location);
    while (loc_len > 0 && location[loc_len-1] == ' ') loc_len--;
    if (loc_len == 0 || loc_len > 1800) return AHTTP_ERR_REDIRECT;
    char loc[1900];
    memcpy(loc, location, loc_len);
    loc[loc_len] = 0;

    /* Only http/https targets are followed.  A ':' before any '/' means
     * some other scheme (data:, javascript:, ...) → refuse (D7). */
    int saw_colon = 0;
    for (const char *p = loc; *p && *p != '/'; p++) {
        if (*p == ':') { saw_colon = 1; break; }
    }
    if (saw_colon && !ci_prefix(loc, "http://") && !ci_prefix(loc, "https://"))
        return AHTTP_ERR_REDIRECT;

    /* 1) absolute URL */
    if (ci_prefix(loc, "http://") || ci_prefix(loc, "https://")) {
        if (loc_len + 1 > cap) return AHTTP_ERR_REDIRECT;
        strcpy(out, loc);
        return 0;
    }

    /* Need the origin of the current URL for everything below. */
    ahttp_url cur;
    if (ahttp_url_parse(current_url, &cur) != 0) return AHTTP_ERR_REDIRECT;
    char authority[320];
    int an = snprintf(authority, sizeof(authority), "%s://%s", cur.scheme, cur.host);
    int def_port = (strcmp(cur.scheme, "https") == 0) ? 443 : 80;
    if (cur.port != def_port)
        an = snprintf(authority, sizeof(authority), "%s://%s:%d",
                      cur.scheme, cur.host, cur.port);
    if (an <= 0 || (size_t)an >= sizeof(authority)) return AHTTP_ERR_REDIRECT;

    size_t pos = 0;

    /* 2) protocol-relative //host/path */
    if (loc[0] == '/' && loc[1] == '/') {
        if (rcat(out, cap, &pos, cur.scheme) != 0) return AHTTP_ERR_REDIRECT;
        if (rcat(out, cap, &pos, ":") != 0) return AHTTP_ERR_REDIRECT;
        if (rcat(out, cap, &pos, loc) != 0) return AHTTP_ERR_REDIRECT;
        return 0;
    }

    char merged[2048];
    if (loc[0] == '/') {
        /* 3) absolute path on the same origin */
        if (strlen(loc) >= sizeof(merged)) return AHTTP_ERR_REDIRECT;
        strcpy(merged, loc);
    } else {
        /* 4) relative reference: merge against the current path's dir. */
        const char *slash = strrchr(cur.path, '/');
        size_t dir_len = slash ? (size_t)(slash - cur.path) + 1 : 1;
        if (dir_len + strlen(loc) >= sizeof(merged)) return AHTTP_ERR_REDIRECT;
        memcpy(merged, cur.path, dir_len);
        strcpy(merged + dir_len, loc);
    }

    /* Remove dot segments from the path part only (keep query intact). */
    char *q = strchr(merged, '?');
    char query[1024];
    query[0] = 0;
    if (q) {
        if (strlen(q) < sizeof(query)) strcpy(query, q);
        *q = 0;
    }
    remove_dot_segments(merged);
    if (merged[0] == 0) strcpy(merged, "/");
    if (query[0]) {
        if (strlen(merged) + strlen(query) >= sizeof(merged)) return AHTTP_ERR_REDIRECT;
        strcat(merged, query);
    }

    if (rcat(out, cap, &pos, authority) != 0) return AHTTP_ERR_REDIRECT;
    if (rcat(out, cap, &pos, merged) != 0) return AHTTP_ERR_REDIRECT;
    return 0;
}

/* ---- Keep-alive client (X6) ----
 * One cached connection.  The struct embeds the transport and its reader
 * so buffered state across responses is preserved exactly. */

typedef struct {
    transport t;
    reader    rd;
    char      scheme[8];
    char      host[256];
    int       port;
} ahttp_conn;

struct ahttp_client {
    const atls_trust_root *roots;
    int                    num_roots;
    const atls_time_now   *now;
    ahttp_conn conn;
    int        conn_open;
};

ahttp_client *ahttp_client_new(void) {
    ahttp_client *c = (ahttp_client *)malloc(sizeof(ahttp_client));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->conn.t.fd = -1;
    /* Inherit the global trust store so one-shot ahttp_get() keeps its
     * pre-X6 behaviour bit for bit. */
    c->roots = g_roots;
    c->num_roots = g_num_roots;
    c->now = g_now;
    return c;
}

void ahttp_client_set_trust_roots(ahttp_client *c,
                                  const atls_trust_root *roots, int num_roots,
                                  const atls_time_now *now) {
    if (!c) return;
    c->roots = roots;
    c->num_roots = num_roots;
    c->now = now;
}

static void client_conn_drop(ahttp_client *c) {
    if (!c || !c->conn_open) return;
    transport_close(&c->conn.t);
    c->conn_open = 0;
}

void ahttp_client_free(ahttp_client *c) {
    if (!c) return;
    client_conn_drop(c);
    free(c);
}

/* Is this method retry-safe on a stale reused socket (RFC 7230 6.3.1)? */
static int method_safe(const char *m) {
    return (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0);
}

/* One request/response exchange over the client's connection, opening or
 * reusing it as appropriate.  Fills resp fields (status, headers, body)
 * on success; returns an AHTTP_ERR_* on failure.  *out_reused is set to 1
 * when a cached socket carried the exchange. */
static int client_exchange(ahttp_client *c, const ahttp_url *u,
                           const char *method, const char *content_type,
                           const void *body, size_t body_len,
                           ahttp_response *resp, int *out_reused) {
    int use_tls = (strcmp(u->scheme, "https") == 0);
    int want_reuse = c->conn_open &&
                     strcmp(c->conn.scheme, u->scheme) == 0 &&
                     strcmp(c->conn.host, u->host) == 0 &&
                     c->conn.port == u->port;

    /* Mismatched cached connection: close it before opening a new one. */
    if (c->conn_open && !want_reuse) client_conn_drop(c);

    int attempt_reused = want_reuse;

    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = (attempt_reused && attempt == 0);
        if (attempt == 1) {
            /* Only idempotent methods may be retried on a fresh socket
             * after a stale one failed. */
            if (!method_safe(method)) break;
            client_conn_drop(c);
        }

        if (!c->conn_open) {
            int rc = transport_connect(&c->conn.t, u->host, u->port, use_tls,
                                       c->roots, c->num_roots, c->now);
            if (rc != 0) { c->conn_open = 0; return rc; }
            /* ahttp_url fields are the same sizes as the cache key fields
             * (bounded by ahttp_url_parse), so strcpy cannot truncate. */
            strcpy(c->conn.scheme, u->scheme);
            strcpy(c->conn.host, u->host);
            c->conn.port = u->port;
            reader_init(&c->conn.rd, &c->conn.t);
            c->conn_open = 1;
            reused = 0;
        } else if (reused && attempt == 0) {
            printf("[ahttp] keep-alive: reusing connection to %s:%d\n",
                   u->host, u->port);
        }

        reader *rd = &c->conn.rd;

        /* Build request.  Pathological path lengths are refused loudly.
         * IPv6 Host is [addr] (RFC 3986 / RFC 9110). */
        char hosthdr[300];
        if (strchr(u->host, ':')) {
            int def = (strcmp(u->scheme, "https") == 0) ? 443 : 80;
            if (u->port != def)
                snprintf(hosthdr, sizeof hosthdr, "[%s]:%d", u->host, u->port);
            else
                snprintf(hosthdr, sizeof hosthdr, "[%s]", u->host);
        } else {
            snprintf(hosthdr, sizeof hosthdr, "%s", u->host);
        }
        char req[4096];
        int rlen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: keep-alive\r\n"
            "Accept: */*\r\n",
            method, u->path, hosthdr);
        if (rlen <= 0 || (size_t)rlen >= sizeof(req)) return AHTTP_ERR_URL;
        size_t off = (size_t)rlen;
        if (body_len > 0) {
            int hn;
            if (content_type && content_type[0]) {
                hn = snprintf(req + off, sizeof(req) - off,
                              "Content-Type: %s\r\n", content_type);
                if (hn <= 0 || (size_t)hn >= sizeof(req) - off) return AHTTP_ERR_URL;
                off += (size_t)hn;
            }
            hn = snprintf(req + off, sizeof(req) - off,
                          "Content-Length: %u\r\n", (unsigned)body_len);
            if (hn <= 0 || (size_t)hn >= sizeof(req) - off) return AHTTP_ERR_URL;
            off += (size_t)hn;
        }
        if (off + 2 >= sizeof(req)) return AHTTP_ERR_URL;
        req[off++] = '\r';
        req[off++] = '\n';

        if (transport_send(&c->conn.t, req, off) < 0) goto retry_or_fail;
        if (body_len > 0) {
            if (transport_send(&c->conn.t, body, body_len) < 0) goto retry_or_fail;
        }

        /* Read the response header block, skipping 1xx interim replies
         * (100-continue arrives without us sending Expect when a proxy is
         * eager; RFC 7231 6.2 says clients MUST parse past them). */
        growbuf hb, bb;
        if (growbuf_init(&hb) != 0 || growbuf_init(&bb) != 0) {
            client_conn_drop(c);
            return AHTTP_ERR_NOMEM;
        }
        int status = -1;
        int interim = 0;
        for (;;) {
            status = read_headers(rd, &hb);
            if (status < 0) { free(hb.data); free(bb.data); goto retry_or_fail; }
            if (status >= 100 && status < 200 && status != 101 &&
                ++interim <= 8) {
                hb.len = 0;   /* drop the interim header block, keep reading */
                hb.data[0] = 0;
                continue;
            }
            break;
        }

        resp->status_code = status;
        resp->headers = (char *)hb.data;
        resp->headers_len = hb.len;

        /* Decide framing.  Reusable only when the body is self-delimiting
         * (Content-Length or chunked) or provably empty; responses that
         * run until EOF end the connection by definition. */
        int no_body = (strcmp(method, "HEAD") == 0) ||
                      (status >= 100 && status < 200) ||
                      status == 204 || status == 304;
        const char *te = no_body ? NULL : find_header(resp->headers, "Transfer-Encoding");
        const char *cl = no_body ? NULL : find_header(resp->headers, "Content-Length");
        int close_after = 0;
        int brc = 0;

        if (no_body) {
            /* nothing to read */
        } else if (te && strstr(te, "chunked")) {
            brc = read_body_chunked(rd, &bb);
        } else if (cl) {
            size_t content_len = (size_t)atoi(cl);
            brc = read_body_content_length(rd, &bb, content_len);
        } else {
            /* No framing: body runs to EOF → the server closes. */
            close_after = 1;
            uint8_t tmp[4096];
            for (;;) {
                int n = reader_read(rd, tmp, sizeof(tmp));
                if (n <= 0) break;
                int erc = growbuf_append(&bb, tmp, (size_t)n);
                if (erc != 0) { brc = erc; break; }
            }
        }

        /* Connection: close (or HTTP/1.0 without keep-alive) also retires
         * the socket after this response. */
        const char *connh = find_header(resp->headers, "Connection");
        if (header_has_token(connh, "close")) close_after = 1;
        if (strncmp(resp->headers, "HTTP/1.0", 8) == 0 &&
            !header_has_token(connh, "keep-alive")) close_after = 1;

        if (brc != 0) {
            /* Broken framing: the stream position is unknown → close. */
            client_conn_drop(c);
            free(bb.data);
            free(resp->headers); resp->headers = NULL; resp->headers_len = 0;
            return brc;
        }

        resp->body = bb.data;
        resp->body_len = bb.len;

        if (close_after) {
            if (reused || c->conn_open)
                printf("[ahttp] keep-alive: closed by server (%s:%d)\n",
                       u->host, u->port);
            client_conn_drop(c);
        }

        *out_reused = reused;
        return AHTTP_OK;

retry_or_fail:
        client_conn_drop(c);
        if (reused && attempt == 0 && method_safe(method)) {
            /* loop reopens and retries exactly once (logged for the gate) */
            printf("[ahttp] keep-alive: stale connection to %s:%d, reopening\n",
                   u->host, u->port);
            continue;
        }
        return AHTTP_ERR_RESPONSE;
    }

    return AHTTP_ERR_RESPONSE;
}

/* Up to AHTTP_MAX_REDIRECTS+1 exchanges following 3xx.  Method/body rules:
 * 301/302 → GET with no body (what browsers have done for 25 years);
 * 307/308 → method and body re-sent.  Every hop is logged. */
static ahttp_response *client_do(ahttp_client *c, const char *method,
                                 const char *url, const char *content_type,
                                 const void *body, size_t body_len) {
    ahttp_response *resp = (ahttp_response *)malloc(sizeof(ahttp_response));
    if (!resp) return NULL;
    memset(resp, 0, sizeof(*resp));
    resp->error = AHTTP_ERR_URL;

    char current_url[2304];
    if (strlen(url) >= sizeof(current_url)) { resp->error = AHTTP_ERR_URL; return resp; }
    strcpy(current_url, url);

    char cur_method[8];
    strncpy(cur_method, method, sizeof(cur_method) - 1);
    cur_method[sizeof(cur_method) - 1] = 0;
    const void  *cur_body = body;
    size_t       cur_body_len = body_len;
    const char  *cur_ct = content_type;

    for (int hop = 0; hop <= AHTTP_MAX_REDIRECTS; hop++) {
        ahttp_url parsed;
        int rc = ahttp_url_parse(current_url, &parsed);
        if (rc != 0) { resp->error = rc; return resp; }

        int reused = 0;
        rc = client_exchange(c, &parsed, cur_method, cur_ct,
                             cur_body, cur_body_len, resp, &reused);
        if (rc != 0) { resp->error = rc; return resp; }
        resp->reused_connection = reused;

        int status = resp->status_code;
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            const char *location = find_header(resp->headers, "Location");
            if (!location) { resp->error = AHTTP_ERR_REDIRECT; return resp; }
            char next_url[2304];
            if (ahttp_resolve_redirect(next_url, sizeof(next_url),
                                       current_url, location) != 0) {
                printf("[ahttp] redirect: refusing Location target from %s\n",
                       current_url);
                resp->error = AHTTP_ERR_REDIRECT;
                return resp;
            }
            printf("[ahttp] redirect: %s -> %s\n", current_url, next_url);
            /* 301/302 rewind to GET; 307/308 keep method + body. */
            if (status == 301 || status == 302) {
                strcpy(cur_method, "GET");
                cur_body = NULL;
                cur_body_len = 0;
                cur_ct = NULL;
            }
            strcpy(current_url, next_url);
            free(resp->headers); resp->headers = NULL;
            free(resp->body); resp->body = NULL;
            resp->headers_len = 0; resp->body_len = 0;
            resp->redirects_used++;
            continue;
        }

        resp->final_url = (char *)malloc(strlen(current_url) + 1);
        if (resp->final_url) strcpy(resp->final_url, current_url);
        resp->error = AHTTP_OK;
        return resp;
    }

    printf("[ahttp] redirect: giving up after %d hops\n", AHTTP_MAX_REDIRECTS);
    resp->error = AHTTP_ERR_REDIRECT;
    return resp;
}

static int method_allowed(const char *m) {
    return (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0 ||
            strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 ||
            strcmp(m, "DELETE") == 0);
}

ahttp_response *ahttp_client_request(ahttp_client *c, const char *method,
                                     const char *url,
                                     const char *content_type,
                                     const void *body, size_t body_len) {
    if (!c || !method || !url) return NULL;
    char m[8];
    size_t ml = strlen(method);
    if (ml == 0 || ml >= sizeof(m)) return NULL;
    for (size_t i = 0; i < ml; i++) {
        char ch = method[i];
        m[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    m[ml] = 0;
    if (!method_allowed(m)) return NULL;
    if (body_len > AHTTP_MAX_REQ_BODY) return NULL;
    if (body_len > 0 && strcmp(m, "POST") != 0 && strcmp(m, "PUT") != 0)
        return NULL;   /* bodies only behind POST/PUT (bounded interface) */
    if (body_len > 0 && !body) return NULL;

    return client_do(c, m, url, content_type, body, body_len);
}

ahttp_response *ahttp_client_get(ahttp_client *c, const char *url) {
    return ahttp_client_request(c, "GET", url, NULL, NULL, 0);
}

/* ---- One-shot entry point (pre-X6 API, kept for compatibility) ---- */

ahttp_response *ahttp_get(const char *url) {
    ahttp_client *c = ahttp_client_new();
    if (!c) return NULL;
    ahttp_response *r = client_do(c, "GET", url, NULL, NULL, 0);
    ahttp_client_free(c);
    return r;
}

void ahttp_response_free(ahttp_response *r) {
    if (!r) return;
    if (r->headers) free(r->headers);
    if (r->body) free(r->body);
    if (r->final_url) free(r->final_url);
    free(r);
}
