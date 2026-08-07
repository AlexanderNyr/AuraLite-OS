/*
 * wv_url.h — URL parsing for the AuraLite web view (WEBVIEW_PLAN W6).
 *
 * Pure C, no libc dependencies beyond memcpy/memset, so the same file is
 * unit-tested on the host and compiled into the guest.  The parser is
 * deliberately small: scheme://host[:port]/path with defaults, plus
 * relative-URL resolution against a base (the "href" of a link).
 *
 * https:// URLs parse fine but are flagged: W6 shows an explanation page
 * instead of attempting a connection (TLS is INTERNET_PLAN's job).
 */

#ifndef AURALITE_WV_URL_H
#define AURALITE_WV_URL_H

#include <stddef.h>
#include <stdint.h>

#define WV_URL_MAX_HOST 255
#define WV_URL_MAX_PATH 1023
#define WV_URL_MAX_URL  (WV_URL_MAX_HOST + WV_URL_MAX_PATH + 32)

typedef struct {
    char     scheme[8];      /* "http" or "https" (lower-cased) */
    char     host[WV_URL_MAX_HOST + 1];
    uint16_t port;           /* 80 for http, 443 for https when absent */
    char     path[WV_URL_MAX_PATH + 1];
    int      is_https;
    int      ok;             /* 1 when the URL is well-formed enough to use */
} wv_url_t;

/* Parse an absolute URL.  Returns 1 on success (0 on garbage). */
int wv_url_parse(const char *url, wv_url_t *out);

/* Resolve a (possibly relative) href against a base URL.  Returns 1 on
 * success.  Base must be absolute.  Handles scheme-absolute hrefs
 * (http://...), root-relative (/path), directory-relative (../x, x) and
 * empty (#fragment is dropped). */
int wv_url_resolve(const char *href, const wv_url_t *base, wv_url_t *out);

/* Render an absolute URL back to text (normalised). */
void wv_url_format(const wv_url_t *u, char *out, size_t cap);

#endif /* AURALITE_WV_URL_H */
