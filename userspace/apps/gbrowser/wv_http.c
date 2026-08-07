/*
 * wv_http.c — HTTP client pieces (WEBVIEW_PLAN W6).  See wv_http.h.
 *
 * All parsers are bounded and defensive: they never read past `len`, they
 * cap values (a 10 MB Content-Length is a number, not a trust boundary),
 * and malformed input returns -1 rather than looping.  The chunked
 * decoder is a small state machine over hex chunk sizes — no recursion,
 * no heap of its own beyond the caller's buffer.
 */

#include <string.h>
#include <stdlib.h>
#include "wv_http.h"

/* ---- request ---- */

int wv_http_build_request(const wv_url_t *u, char *out, size_t cap) {
    if (!u || !u->ok || !out) return -1;
    size_t p = 0;
    const char *get = "GET ";
    const char *ver = " HTTP/1.1\r\nHost: ";
    const char *tail = "\r\nConnection: close\r\n\r\n";
    size_t i;
    for (i = 0; get[i]; i++) { if (p + 1 >= cap) return -1; out[p++] = get[i]; }
    for (i = 0; u->path[i]; i++) { if (p + 1 >= cap) return -1; out[p++] = u->path[i]; }
    for (i = 0; ver[i]; i++) { if (p + 1 >= cap) return -1; out[p++] = ver[i]; }
    for (i = 0; u->host[i]; i++) { if (p + 1 >= cap) return -1; out[p++] = u->host[i]; }
    int def_port = u->is_https ? 443 : 80;
    if (u->port != def_port) {
        char pb[6];
        int pn = 0;
        uint32_t v = u->port;
        do { pb[pn++] = (char)('0' + v % 10); v /= 10; } while (v);
        if (p + 1 + pn + 1 >= cap) return -1;
        out[p++] = ':';
        while (pn > 0) out[p++] = pb[--pn];
    }
    for (i = 0; tail[i]; i++) { if (p + 1 >= cap) return -1; out[p++] = tail[i]; }
    out[p] = 0;
    return (int)p;
}

/* ---- growing response buffer ---- */

void wv_resp_init(wv_resp_t *r, char *buf, size_t initial_cap) {
    r->data = buf;
    r->cap = initial_cap ? initial_cap : WV_HTTP_INITIAL_CAP;
    r->len = 0;
    r->refused = 0;
}

int wv_resp_append(wv_resp_t *r, const char *data, size_t n) {
    if (!r || !data) return 0;
    if (r->refused) return 0;
    if (r->len + n > r->cap) {
        /* grow: double until it fits, or refuse past the ceiling */
        size_t want = r->len + n;
        size_t nc = r->cap ? r->cap : WV_HTTP_INITIAL_CAP;
        while (nc < want) {
            size_t n2 = nc * 2;
            if (n2 <= nc || n2 > WV_HTTP_MAX_CAP) { n2 = WV_HTTP_MAX_CAP; }
            nc = n2;
            if (nc >= want) break;
            if (nc >= WV_HTTP_MAX_CAP) break;
        }
        if (nc > WV_HTTP_MAX_CAP) nc = WV_HTTP_MAX_CAP;
        if (nc < want) {
            /* keep the prefix, mark the refusal */
            r->refused = 1;
            return 0;
        }
        char *nd = (char *)realloc(r->data, nc);
        if (!nd) { r->refused = 1; return 0; }
        r->data = nd;
        r->cap = nc;
    }
    memcpy(r->data + r->len, data, n);
    r->len += n;
    return 1;
}

/* ---- response parsing ---- */

static int wv_line(const char *buf, size_t len, size_t *pos, char *out,
                   size_t out_cap) {
    /* copy one CRLF-terminated line; returns 1 and advances *pos */
    size_t i = *pos;
    size_t p = 0;
    while (i < len && buf[i] != '\n') {
        if (buf[i] != '\r' && p + 1 < out_cap) out[p++] = buf[i];
        i++;
    }
    if (i >= len) return 0;              /* line not complete yet */
    i++;                                 /* past '\n' */
    out[p] = 0;
    *pos = i;
    return 1;
}

int wv_http_parse_headers(const char *buf, size_t len, wv_http_meta_t *m) {
    memset(m, 0, sizeof(*m));
    m->status = -1;
    if (!buf || len < 4) return 0;
    /* find CRLFCRLF */
    size_t he = (size_t)-1;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') { he = i + 4; break; }
    }
    if (he == (size_t)-1) return 0;
    m->header_len = he;

    /* status line */
    size_t pos = 0;
    char line[256];
    if (!wv_line(buf, he, &pos, line, sizeof(line))) return 0;
    if (strncmp(line, "HTTP/", 5) != 0) return 0;
    int code = 0;
    size_t i = 5;
    while (line[i] && line[i] != ' ') i++;
    while (line[i] == ' ') i++;
    while (line[i] >= '0' && line[i] <= '9') {
        code = code * 10 + (line[i] - '0');
        i++;
    }
    m->status = code;

    /* headers */
    while (pos < he) {
        if (!wv_line(buf, he, &pos, line, sizeof(line))) break;
        if (line[0] == 0) break;
        /* header name */
        size_t c = 0;
        while (line[c] && line[c] != ':') c++;
        if (line[c] != ':') continue;
        size_t v = c + 1;
        while (line[v] == ' ') v++;
        /* lowercase name compare */
        if (line[0] == 'c' || line[0] == 'C' || line[0] == 't' || line[0] == 'T' ||
            line[0] == 'l' || line[0] == 'L') {
            char name[64];
            size_t nl = c < sizeof(name) - 1 ? c : sizeof(name) - 1;
            for (size_t k = 0; k < nl; k++)
                name[k] = (line[k] >= 'A' && line[k] <= 'Z') ? (char)(line[k] + 32) : line[k];
            name[nl] = 0;
            if (strcmp(name, "content-length") == 0) {
                m->has_content_len = 1;
                m->content_len = 0;
                for (size_t k = v; line[k] && line[k] >= '0' && line[k] <= '9'; k++) {
                    if (m->content_len > (size_t)1 << 40) break;
                    m->content_len = m->content_len * 10 + (size_t)(line[k] - '0');
                }
            } else if (strcmp(name, "transfer-encoding") == 0) {
                /* "chunked" anywhere in the value */
                for (size_t k = v; line[k]; k++) {
                    if ((line[k] == 'c' || line[k] == 'C') &&
                        strncmp(line + k, "chunked", 7) == 0) {
                        m->chunked = 1;
                        break;
                    }
                }
            }
        }
    }
    m->ok = 1;
    return 1;
}

int wv_http_decode_chunked(const char *buf, size_t len,
                           char *out, size_t out_cap, int *complete) {
    if (complete) *complete = 0;
    size_t i = 0, o = 0;
    int saw_term = 0;
    while (i < len) {
        /* chunk size line: hex until CRLF or ';' */
        uint32_t size = 0;
        int any = 0;
        while (i < len) {
            unsigned char c = (unsigned char)buf[i];
            if (c == '\r' || c == '\n' || c == ';') break;
            int hv;
            if (c >= '0' && c <= '9') hv = c - '0';
            else if (c >= 'a' && c <= 'f') hv = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hv = c - 'A' + 10;
            else return -1;
            if (size > 0xFFFFFFFu) return -1;
            size = size * 16 + (uint32_t)hv;
            any = 1;
            i++;
        }
        if (!any) return -1;
        while (i < len && buf[i] != '\n') i++;   /* skip to EOL */
        if (i >= len) return -1;
        i++;                                      /* past '\n' */
        if (size == 0) { saw_term = 1; break; }
        if (o + size > out_cap) return -1;        /* does not fit */
        if (i + size > len) return -1;            /* chunk incomplete */
        memcpy(out + o, buf + i, size);
        o += size;
        i += size;
        /* trailing CRLF */
        if (i + 1 < len && buf[i] == '\r' && buf[i + 1] == '\n') i += 2;
        else if (i < len && buf[i] == '\n') i += 1;
        else if (i >= len) return -1;
    }
    if (!saw_term) return -1;                     /* stream not terminated */
    if (complete) *complete = 1;
    return (int)o;
}

int wv_http_body(const char *buf, size_t len, const wv_http_meta_t *m,
                 char *out, size_t out_cap, int *done) {
    if (done) *done = 0;
    if (!m->ok) return -1;
    size_t body_start = m->header_len;
    if (body_start > len) return -1;
    const char *body = buf + body_start;
    size_t body_len = len - body_start;

    if (m->chunked) {
        int complete = 0;
        int n = wv_http_decode_chunked(body, body_len, out, out_cap, &complete);
        if (done) *done = complete;
        return n;
    }
    if (m->has_content_len) {
        size_t want = m->content_len;
        if (body_len < want) return -1;           /* need more data */
        if (want > out_cap) want = out_cap;
        memcpy(out, body, want);
        if (done) *done = 1;
        return (int)want;
    }
    /* no length: body runs to EOF (Connection: close) */
    size_t n = body_len;
    if (n > out_cap) n = out_cap;
    memcpy(out, body, n);
    if (done) *done = 1;
    return (int)n;
}
