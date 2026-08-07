/*
 * wv_url.c — URL parsing and resolution for the AuraLite web view
 * (WEBVIEW_PLAN W6).  See wv_url.h.
 *
 * The parser is tolerant in the way browsers are: a missing scheme means
 * "http", a missing path means "/", extra whitespace is trimmed.  It is
 * strict about the things that matter: the host must be non-empty and
 * printable, the port numeric, and https is flagged so the caller can
 * refuse it with an explanation rather than a hang.
 */

#include <string.h>
#include "wv_url.h"

static int wv_url_isalpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int wv_url_isdigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

/* Trim leading/trailing whitespace of the input. */
static const char *wv_trim(const char *s, size_t *len) {
    while (*len > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' ||
                        s[0] == '\r')) { s++; (*len)--; }
    while (*len > 0 && (s[*len - 1] == ' ' || s[*len - 1] == '\t' ||
                        s[*len - 1] == '\n' || s[*len - 1] == '\r')) (*len)--;
    return s;
}

static void wv_url_clear(wv_url_t *u) {
    memset(u, 0, sizeof(*u));
    u->port = 80;
    u->path[0] = '/';
    u->ok = 0;
}

/* Validate a host: printable chars, no spaces/slashes. */
static int wv_host_ok(const char *h, size_t len) {
    if (len == 0 || len > WV_URL_MAX_HOST) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c < 0x21 || c > 0x7E) return 0;
        if (c == '/' || c == ' ' || c == '\t') return 0;
    }
    return 1;
}

int wv_url_parse(const char *url, wv_url_t *out) {
    if (!url || !out) return 0;
    wv_url_clear(out);
    size_t len = strlen(url);
    const char *s = wv_trim(url, &len);
    if (len == 0) return 0;

    /* scheme */
    size_t i = 0;
    while (i < len && s[i] != ':') {
        if (!wv_url_isalpha(s[i]) && i > 0 && !wv_url_isdigit(s[i]) &&
            s[i] != '+' && s[i] != '-' && s[i] != '.')
            break;
        i++;
    }
    if (i > 0 && i < len && s[i] == ':' && i + 2 < len &&
        s[i + 1] == '/' && s[i + 2] == '/') {
        if (i >= sizeof(out->scheme)) return 0;
        for (size_t k = 0; k < i; k++)
            out->scheme[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
        out->scheme[i] = 0;
        if (strcmp(out->scheme, "http") != 0 && strcmp(out->scheme, "https") != 0)
            return 0;
        out->is_https = (out->scheme[4] == 's');   /* "https" */
        out->port = out->is_https ? 443 : 80;
        i += 3;                                     /* past "://" */
    } else {
        /* no scheme: default to http */
        strcpy(out->scheme, "http");
        out->is_https = 0;
        out->port = 80;
        i = 0;
    }

    /* host[:port] */
    size_t hstart = i;
    while (i < len && s[i] != '/' && s[i] != ':' && s[i] != '?' && s[i] != '#')
        i++;
    size_t hlen = i - hstart;
    if (!wv_host_ok(s + hstart, hlen)) return 0;
    memcpy(out->host, s + hstart, hlen);
    out->host[hlen] = 0;

    if (i < len && s[i] == ':') {
        i++;
        uint32_t port = 0;
        size_t pd = 0;
        while (i < len && wv_url_isdigit(s[i]) && pd < 5) {
            port = port * 10 + (uint32_t)(s[i] - '0');
            i++;
            pd++;
        }
        if (pd == 0 || port == 0 || port > 65535) return 0;
        out->port = (uint16_t)port;
    }

    /* path (up to '?' — query is dropped for now) */
    if (i < len && s[i] == '/') {
        size_t pstart = i;
        while (i < len && s[i] != '?' && s[i] != '#') i++;
        size_t plen = i - pstart;
        if (plen == 0) plen = 1;                    /* "/" */
        if (plen >= sizeof(out->path)) plen = sizeof(out->path) - 1;
        memcpy(out->path, s + pstart, plen);
        out->path[plen] = 0;
    } else {
        strcpy(out->path, "/");
    }

    out->ok = 1;
    return 1;
}

void wv_url_format(const wv_url_t *u, char *out, size_t cap) {
    if (!u || !out || cap == 0) return;
    if (!u->ok) { out[0] = 0; return; }
    size_t p = 0;
    const char *scheme = u->is_https ? "https://" : "http://";
    while (scheme[p] && p + 1 < cap) { out[p] = scheme[p]; p++; }
    size_t hl = strlen(u->host);
    for (size_t k = 0; k < hl && p + 1 < cap; k++) out[p++] = u->host[k];
    int def_port = u->is_https ? 443 : 80;
    if (u->port != def_port && p + 8 < cap) {
        out[p++] = ':';
        char pb[6];
        int pn = 0;
        uint32_t v = u->port;
        do { pb[pn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (pn > 0 && p + 1 < cap) out[p++] = pb[--pn];
    }
    size_t pl = strlen(u->path);
    for (size_t k = 0; k < pl && p + 1 < cap; k++) out[p++] = u->path[k];
    out[p] = 0;
}

int wv_url_resolve(const char *href, const wv_url_t *base, wv_url_t *out) {
    if (!href || !base || !out || !base->ok) return 0;
    size_t len = strlen(href);
    const char *s = wv_trim(href, &len);

    /* absolute (scheme://...) or protocol-relative (//host/path) */
    if (len >= 8 && strncmp(s, "http://", 7) == 0) return wv_url_parse(s, out);
    if (len >= 9 && strncmp(s, "https://", 8) == 0) return wv_url_parse(s, out);
    if (len >= 2 && s[0] == '/' && s[1] == '/') {
        char tmp[WV_URL_MAX_URL];
        size_t t = 0;
        const char *sch = base->is_https ? "https:" : "http:";
        while (sch[t] && t + 1 < sizeof(tmp)) { tmp[t] = sch[t]; t++; }
        for (size_t k = 0; k < len && t + 1 < sizeof(tmp); k++)
            tmp[t++] = s[k];
        tmp[t] = 0;
        return wv_url_parse(tmp, out);
    }

    /* fragment-only: keep the same document */
    if (len > 0 && s[0] == '#') {
        *out = *base;
        return 1;
    }
    if (len == 0) {
        *out = *base;
        return 1;
    }

    /* root-relative */
    if (s[0] == '/') {
        wv_url_t u = *base;
        size_t pl = len;
        if (pl >= sizeof(u.path)) pl = sizeof(u.path) - 1;
        memcpy(u.path, s, pl);
        u.path[pl] = 0;
        u.ok = 1;
        *out = u;
        return 1;
    }

    /* directory-relative: resolve against the base path's directory.
     * "a/b" + base "/x/y/z" -> "/x/y/a/b"; leading "../" climbs. */
    char dir[WV_URL_MAX_PATH + 1];
    size_t dl = 0;
    const char *bp = base->path;
    size_t bpl = strlen(bp);
    /* drop the last segment */
    size_t last = bpl;
    while (last > 0 && bp[last - 1] != '/') last--;
    if (last == 0) { dir[dl++] = '/'; }
    else { for (size_t k = 0; k < last && dl < WV_URL_MAX_PATH; k++) dir[dl++] = bp[k]; }

    /* walk "../" segments */
    size_t i = 0;
    while (i + 2 < len && s[i] == '.' && s[i + 1] == '.' && s[i + 2] == '/') {
        /* climb one directory: strip a trailing '/', then the last
         * segment, then the '/' in front of it */
        while (dl > 0 && dir[dl - 1] == '/') dl--;
        while (dl > 0 && dir[dl - 1] != '/') dl--;
        if (dl > 0) dl--;              /* the '/' before the segment */
        if (dl == 0) dir[dl++] = '/';
        i += 3;
    }
    /* "./" segments are skipped */
    while (i + 1 < len && s[i] == '.' && s[i + 1] == '/') i += 2;

    wv_url_t u = *base;
    size_t pl = 0;
    char *path = u.path;
    for (size_t k = 0; k < dl && pl < WV_URL_MAX_PATH; k++) path[pl++] = dir[k];
    for (size_t k = i; k < len && pl < WV_URL_MAX_PATH; k++) path[pl++] = s[k];
    path[pl] = 0;
    if (pl == 0) { path[0] = '/'; path[1] = 0; }
    u.ok = 1;
    *out = u;
    return 1;
}
