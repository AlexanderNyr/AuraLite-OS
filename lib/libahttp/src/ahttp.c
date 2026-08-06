/* ahttp.c — HTTP/1.1 client over plain TCP or TLS (INTERNET_PLAN.md N6).
 *
 * One entry point: ahttp_get(url).  Handles both http:// and https://.
 * Uses BSD sockets internally (socket/connect/send/recv) and libatls
 * for TLS.  Never touches the kernel's legacy net_* API.
 */

#include "ahttp/http.h"
#include "atls/atls.h"
#include "atls/tls.h"
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
#else
/* Host: use standard POSIX. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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
    return send(fd, data, len, 0);
}
static int host_recv(int fd, void *buf, size_t cap) {
    return recv(fd, buf, cap, 0);
}
#endif

/* DNS resolver (AuraLite libc). */
extern uint32_t dns_resolve(const char *hostname);

/* IP in host byte order (AuraLite format: a<<24 | b<<16 | c<<8 | d). */
static uint32_t parse_ip(const char *s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return (a << 24) | (b << 16) | (c << 8) | d;
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

    /* Host[:port]. */
    const char *host_start = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return AHTTP_ERR_URL;
    for (size_t i = 0; i < host_len; i++) out->host[i] = host_start[i];
    out->host[host_len] = 0;

    /* Port. */
    if (*p == ':') {
        p++;
        out->port = 0;
        while (*p >= '0' && *p <= '9') {
            out->port = out->port * 10 + (*p - '0');
            p++;
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

/* ---- Transport layer (plain TCP or TLS) ---- */

typedef struct {
    int fd;
    int is_tls;
    atls_tls *tls;
} transport;

static int transport_connect(transport *t, const char *host, int port, int use_tls) {
    t->is_tls = use_tls;
    t->tls = NULL;

    /* Resolve host. */
    uint32_t ip = parse_ip(host);
    if (!ip) ip = dns_resolve(host);
    if (!ip) return AHTTP_ERR_DNS;

    /* TCP connect. */
#ifdef __AURALITE__
    t->fd = socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
#else
    t->fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (t->fd < 0) return AHTTP_ERR_CONNECT;
#ifdef __AURALITE__
    int rc = connect(t->fd, ip, (uint16_t)port);
#else
    int rc = host_connect_ip(t->fd, ip, (uint16_t)port);
#endif
    if (rc != 0) { closesocket(t->fd); return AHTTP_ERR_CONNECT; }

    /* TLS handshake. */
    if (use_tls) {
        atls_tls_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.hostname = host;
        cfg.alpn = "http/1.1";
        t->tls = atls_tls_new(&cfg, NULL, NULL, NULL);
        if (!t->tls) { closesocket(t->fd); return AHTTP_ERR_TLS; }
        /* TODO: wire up send/recv callbacks to the socket.
         * For now, TLS support is deferred — the N6 gate is HTTP over
         * plain TCP, and HTTPS requires the guest TCP fix (N7). */
        atls_tls_free(t->tls);
        t->tls = NULL;
        closesocket(t->fd);
        return AHTTP_ERR_TLS;
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

/* ---- Buffered reader ---- */

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

/* ---- Main entry point ---- */

ahttp_response *ahttp_get(const char *url) {
    ahttp_response *resp = (ahttp_response *)malloc(sizeof(ahttp_response));
    if (!resp) return NULL;
    memset(resp, 0, sizeof(*resp));
    resp->error = AHTTP_ERR_URL;

    ahttp_url parsed;
    int rc = ahttp_url_parse(url, &parsed);
    if (rc != 0) { resp->error = rc; return resp; }

    /* Follow redirects. */
    char current_url[2048];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = 0;

    for (int hop = 0; hop <= AHTTP_MAX_REDIRECTS; hop++) {
        rc = ahttp_url_parse(current_url, &parsed);
        if (rc != 0) { resp->error = rc; return resp; }

        int use_tls = (strcmp(parsed.scheme, "https") == 0);

        transport t;
        memset(&t, 0, sizeof(t));
        t.fd = -1;
        rc = transport_connect(&t, parsed.host, parsed.port, use_tls);
        if (rc != 0) { resp->error = rc; return resp; }

        /* Build request. */
        char req[4096];
        int rlen = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Accept: */*\r\n"
            "\r\n",
            parsed.path, parsed.host);

        rc = transport_send(&t, req, (size_t)rlen);
        if (rc < 0) { transport_close(&t); resp->error = AHTTP_ERR_REQUEST; return resp; }

        /* Read response. */
        reader rd;
        reader_init(&rd, &t);

        growbuf hb, bb;
        if (growbuf_init(&hb) != 0 || growbuf_init(&bb) != 0) {
            transport_close(&t);
            resp->error = AHTTP_ERR_NOMEM;
            return resp;
        }

        int status = read_headers(&rd, &hb);
        if (status < 0) {
            transport_close(&t);
            free(hb.data); free(bb.data);
            resp->error = AHTTP_ERR_RESPONSE;
            return resp;
        }

        resp->status_code = status;
        resp->headers = (char *)hb.data;
        resp->headers_len = hb.len;

        /* Determine body reading mode. */
        const char *te = find_header(resp->headers, "Transfer-Encoding");
        const char *cl = find_header(resp->headers, "Content-Length");

        if (te && strstr(te, "chunked")) {
            resp->error = read_body_chunked(&rd, &bb);
        } else if (cl) {
            size_t content_len = (size_t)atoi(cl);
            resp->error = read_body_content_length(&rd, &bb, content_len);
        } else {
            /* Read until EOF. */
            uint8_t tmp[4096];
            while (1) {
                int n = reader_read(&rd, tmp, sizeof(tmp));
                if (n <= 0) break;
                int erc = growbuf_append(&bb, tmp, (size_t)n);
                if (erc != 0) { resp->error = erc; break; }
            }
        }

        transport_close(&t);

        resp->body = bb.data;
        resp->body_len = bb.len;

        /* Check for redirects. */
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            const char *location = find_header(resp->headers, "Location");
            if (!location) { resp->error = AHTTP_ERR_REDIRECT; return resp; }
            /* Copy location into current_url. */
            size_t loc_len = strlen(location);
            /* Strip trailing \r\n. */
            while (loc_len > 0 && (location[loc_len - 1] == '\r' || location[loc_len - 1] == '\n'))
                loc_len--;
            if (loc_len >= sizeof(current_url)) loc_len = sizeof(current_url) - 1;
            for (size_t i = 0; i < loc_len; i++) current_url[i] = location[i];
            current_url[loc_len] = 0;
            /* Free previous response data and continue. */
            free(resp->headers); resp->headers = NULL;
            free(resp->body); resp->body = NULL;
            resp->headers_len = 0; resp->body_len = 0;
            continue;
        }

        /* Not a redirect — done. */
        resp->final_url = (char *)malloc(strlen(current_url) + 1);
        if (resp->final_url) strcpy(resp->final_url, current_url);
        resp->error = AHTTP_OK;
        return resp;
    }

    resp->error = AHTTP_ERR_REDIRECT;
    return resp;
}

void ahttp_response_free(ahttp_response *r) {
    if (!r) return;
    if (r->headers) free(r->headers);
    if (r->body) free(r->body);
    if (r->final_url) free(r->final_url);
    free(r);
}
