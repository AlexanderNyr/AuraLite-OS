/* w32aux.c — W32APP_PLAN.md phase W32A-10: the small modules.
 *
 *   WININET  InternetCrackUrlW — the pure URL parser;
 *   DBGHELP  ImageNtHeader — pointer arithmetic over a mapped module;
 *   DWMAPI   the pair — composition-off constants, reasons recorded;
 *   SENSAPI  the pair — real best-effort TCP probes, never hardcoded;
 *   WINTRUST WinVerifyTrust — signature status unknown;
 *   CRYPT32  the eight — FAIL-CLEAN, no store, no crypt message.
 *
 * Licensed Apache-2.0.  Interface facts only (see w32aux.h,
 * w32/PROVENANCE.md).
 */

#include "w32/w32aux.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- notes -------------------------------------------------------------------- */

static int dwm_noted, trust_noted, crypt_noted;

static void aux_note(int *flag, const char *what) {
    if (*flag) return;
    *flag = 1;
    printf("w32: %s\n", what);
}

/* The socket primitives (kernel-shaped: host-order u32 ip). */
extern int socket(int domain, int type, int protocol);
extern int connect(int sock, uint32_t ip, uint16_t port);
extern int closesocket(int sock);

static size_t aux_wcslen(const uint16_t *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* "a.b.c.d" -> host-order u32; 0 on a bad literal. */
static uint32_t aux_ip_parse(const char *a) {
    uint32_t ip = 0;
    int oct = 0, seen = 0;
    for (const char *p = a; ; p++) {
        if (*p >= '0' && *p <= '9') {
            oct = oct * 10 + (*p - '0');
            if (oct > 255) return 0;
            seen++;
        } else if (*p == '.' || *p == 0) {
            if (!seen || seen > 3) return 0;
            ip = (ip << 8) | (uint32_t)oct;
            oct = 0;
            seen = 0;
            if (*p == 0) break;
        } else {
            return 0;               /* not an IP literal */
        }
    }
    return ip;
}

/* The user-network gateway (QEMU SLIRP: 10.0.2.2).  A probe is a TCP
 * connect with the short path the stack gives us; the outcome is
 * reported, never assumed. */
static int aux_probe(const char *ip_str, uint16_t port) {
    uint32_t ip = aux_ip_parse(ip_str);
    if (!ip) return 0;
    int fd = socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (fd < 0) return 0;           /* no stack: offline, honestly */
    int ok = (connect(fd, ip, port) == 0);
    closesocket(fd);
    return ok;
}

/* ---- WININET: InternetCrackUrlW -------------------------------------------------- */

static int url_scheme_of(const uint16_t *s, size_t n, int *def_port) {
    static const struct { const char *name; int scheme; int port; } tab[] = {
        { "ftp",     W32_INTERNET_SCHEME_FTP,    21 },
        { "gopher",  W32_INTERNET_SCHEME_GOPHER, 70 },
        { "http",    W32_INTERNET_SCHEME_HTTP,   80 },
        { "https",   W32_INTERNET_SCHEME_HTTPS,  443 },
        { "file",    W32_INTERNET_SCHEME_FILE,   0 },
    };
    for (size_t t = 0; t < sizeof tab / sizeof tab[0]; t++) {
        if (n != strlen(tab[t].name)) continue;
        size_t i;
        for (i = 0; i < n; i++) {
            uint16_t c = s[i];
            if (c >= 'A' && c <= 'Z') c = (uint16_t)(c - 'A' + 'a');
            if (c != (uint16_t)(unsigned char)tab[t].name[i]) break;
        }
        if (i == n) {
            if (def_port) *def_port = tab[t].port;
            return tab[t].scheme;
        }
    }
    return W32_INTERNET_SCHEME_DEFAULT;
}

/* Fill one component: pointer+length in/out, NUL when it fits.
 * Returns 0 on too-small (with the needed length left behind). */
static int url_comp(uint16_t *buf, W32_DWORD *cap, const uint16_t *src,
                    size_t n) {
    if (!buf) {
        *cap = (W32_DWORD)n;
        return 1;
    }
    if ((size_t)*cap < n + 1) {
        *cap = (W32_DWORD)n;
        return 0;
    }
    memcpy(buf, src, n * 2u);
    buf[n] = 0;
    *cap = (W32_DWORD)n;
    return 1;
}

W32_BOOL W32ABI InternetCrackUrlW(W32_LPCWSTR url, W32_DWORD len,
                                  W32_DWORD flags, W32_URL_COMPONENTSW *parts) {
    (void)flags;
    if (!url || !parts) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (parts->dwStructSize < sizeof *parts) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t n = len ? (size_t)len : aux_wcslen(url);
    if (n == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* scheme://[user[:pass]@]host[:port]/path?extra */
    const uint16_t *p = url, *end = url + n;
    const uint16_t *colon = NULL;
    for (const uint16_t *q = p; q < end && *q != '/'; q++)
        if (*q == ':') { colon = q; break; }
    if (!colon || colon == p) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);  /* no scheme */
        return 0;
    }
    int def_port = 0;
    int scheme = url_scheme_of(p, (size_t)(colon - p), &def_port);
    const uint16_t *rest = colon + 1;
    if (rest + 1 < end && rest[0] == '/' && rest[1] == '/')
        rest += 2;
    else {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);  /* no "//" */
        return 0;
    }

    /* authority: up to the first /, ? or end */
    const uint16_t *auth = rest;
    const uint16_t *pathq = end;
    for (const uint16_t *q = rest; q < end; q++) {
        if (*q == '/' || *q == '?') { pathq = q; break; }
    }
    /* user:pass@ split */
    const uint16_t *hostp = auth;
    const uint16_t *user = NULL, *userend = NULL, *pass = NULL, *passend = NULL;
    for (const uint16_t *q = auth; q < pathq; q++) {
        if (*q == '@') {
            user = auth;
            userend = q;
            for (const uint16_t *r = auth; r < q; r++)
                if (*r == ':') { pass = r + 1; passend = q; userend = r; break; }
            hostp = q + 1;
            break;
        }
    }
    /* port */
    int port = def_port;
    const uint16_t *portp = NULL;
    for (const uint16_t *q = hostp; q < pathq; q++) {
        if (*q == ':') { portp = q + 1; break; }
    }
    const uint16_t *hostrealend = portp ? (portp - 1) : pathq;
    if (portp) {
        port = 0;
        for (const uint16_t *q = portp; q < pathq; q++) {
            if (*q < '0' || *q > '9' || port > 65535) {
                w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
                return 0;
            }
            port = port * 10 + (*q - '0');
        }
        if (port == 0) port = def_port;   /* "host:" spells the default */
    }
    /* path / extra split */
    const uint16_t *path = pathq, *pathend = end;
    const uint16_t *extra = NULL, *extraend = NULL;
    for (const uint16_t *q = pathq; q < end; q++) {
        if (*q == '?') { pathend = q; extra = q + 1; extraend = end; break; }
    }
    if (pathq == end) { path = end; pathend = end; }

    int ok = 1;
    if (parts->lpszScheme)
        ok &= url_comp(parts->lpszScheme, &parts->dwSchemeLength, p,
                       (size_t)(colon - p));
    else
        parts->dwSchemeLength = 0;
    parts->nScheme = scheme;
    if (parts->lpszUserName)
        ok &= user ? url_comp(parts->lpszUserName, &parts->dwUserNameLength,
                              user, (size_t)(userend - user)) : 0;
    else
        parts->dwUserNameLength = 0;
    if (parts->lpszPassword)
        ok &= pass ? url_comp(parts->lpszPassword, &parts->dwPasswordLength,
                              pass, (size_t)(passend - pass)) : 0;
    else
        parts->dwPasswordLength = 0;
    if (parts->lpszHostName)
        ok &= url_comp(parts->lpszHostName, &parts->dwHostNameLength,
                       hostp, (size_t)(hostrealend - hostp));
    else
        parts->dwHostNameLength = 0;
    parts->nPort = (W32_WORD)port;
    if (parts->lpszUrlPath)
        ok &= url_comp(parts->lpszUrlPath, &parts->dwUrlPathLength,
                       path, (size_t)(pathend - path));
    else
        parts->dwUrlPathLength = 0;
    if (parts->lpszExtraInfo)
        ok &= extra ? url_comp(parts->lpszExtraInfo, &parts->dwExtraInfoLength,
                               extra, (size_t)(extraend - extra)) : 0;
    else
        parts->dwExtraInfoLength = 0;

    if (!ok) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return 1;
}

/* ---- DBGHELP: ImageNtHeader ------------------------------------------------------- */

const void *W32ABI ImageNtHeader(const void *base) {
    if (!base) return NULL;
    const uint8_t *b = (const uint8_t *)base;
    if (b[0] != 'M' || b[1] != 'Z') return NULL;
    uint32_t off = (uint32_t)b[0x3C] | ((uint32_t)b[0x3D] << 8) |
                   ((uint32_t)b[0x3E] << 16) | ((uint32_t)b[0x3F] << 24);
    /* e_lfanew sanity: the NT headers must start inside the image; the
     * callers here hand us mapped modules, so a wild offset is refused
     * rather than dereferenced. */
    if (off < 0x40 || off > 0x00100000u) return NULL;
    const uint8_t *nt = b + off;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0)
        return NULL;
    return nt;
}

/* ---- DWMAPI ------------------------------------------------------------------------ */

W32_LONG W32ABI DwmGetColorizationColor(W32_DWORD *color, W32_BOOL *opaque) {
    aux_note(&dwm_noted,
             "[dwmapi] DwmGetColorizationColor: composition is off; the "
             "default colourisation (opaque black) is reported, a constant");
    if (color) *color = 0xFF000000u;
    if (opaque) *opaque = 1;
    return 0;                       /* S_OK */
}

W32_LONG W32ABI DwmSetWindowAttribute(W32_HANDLE hwnd, W32_DWORD attr,
                                      const void *val, W32_DWORD size) {
    (void)hwnd; (void)attr; (void)val; (void)size;
    aux_note(&dwm_noted,
             "[dwmapi] DwmSetWindowAttribute: no composition to configure "
             "(E_NOTIMPL)");
    return 0x80004001uL;            /* E_NOTIMPL */
}

/* ---- SENSAPI: the probes -------------------------------------------------------------- */

W32_BOOL W32ABI IsNetworkAlive(W32_DWORD *flags) {
    if (flags) *flags = 0;          /* no subnetwork semantics to report */
    return aux_probe("10.0.2.2", 53) ? 1 : 0;
}

W32_BOOL W32ABI IsDestinationReachableW(W32_LPCWSTR dest, void *info) {
    if (info) {
        W32_QOCINFO *q = (W32_QOCINFO *)info;
        q->dwSize = sizeof *q;
        q->dwInSpeed = 0;           /* unknown: reported, not invented */
        q->dwOutSpeed = 0;
    }
    if (!dest || !dest[0]) return 0;
    char a[256];
    if (w32_utf16z_to_utf8(dest, a, (int32_t)sizeof a) <= 0) return 0;
    char ip[64];
    int port = 80;
    char *c = strchr(a, ':');
    if (c) {
        *c = 0;
        port = atoi(c + 1);
        if (port <= 0 || port > 65535) return 0;
    }
    if (strlen(a) >= sizeof ip) return 0;
    strcpy(ip, a);
    return aux_probe(ip, (uint16_t)port) ? 1 : 0;
}

/* ---- WINTRUST --------------------------------------------------------------------------- */

W32_LONG W32ABI WinVerifyTrust(void *hwnd, const void *action,
                               const void *data) {
    (void)hwnd; (void)action; (void)data;
    aux_note(&trust_noted,
             "[wintrust] WinVerifyTrust: signature status unknown "
             "(TRUST_E_NOSIGNATURE); no trust decision is made or implied");
    return W32_TRUST_E_NOSIGNATURE;
}

/* ---- CRYPT32: the eight ------------------------------------------------------------------- */

W32_BOOL W32ABI CryptQueryObject(W32_DWORD kind, const void *source,
                                 W32_DWORD *ct, W32_DWORD *cd,
                                 W32_DWORD *st, W32_DWORD *sd,
                                 void **hctx, const void **pv) {
    (void)kind; (void)source; (void)ct; (void)cd; (void)st; (void)sd;
    (void)hctx; (void)pv;
    aux_note(&crypt_noted,
             "[crypt32] CryptQueryObject: no signed-object support "
             "(W32A-10 fail-clean)");
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

void *W32ABI CertFindCertificateInStore(void *store, W32_DWORD enc,
                                        W32_DWORD flags, W32_DWORD type,
                                        const void *what, void *prev) {
    (void)enc; (void)flags; (void)type; (void)what; (void)prev;
    aux_note(&crypt_noted,
             "[crypt32] there is no certificate store (CRYPT_E_NOT_FOUND)");
    if (!store) w32_set_last_error(6);   /* invalid handle, like LsaClose */
    else w32_set_last_error(0x80092009u);/* CRYPT_E_NOT_FOUND */
    return NULL;
}

W32_BOOL W32ABI CertGetCertificateContextProperty(void *ctx, W32_DWORD prop,
                                                  void *data, W32_DWORD *len) {
    (void)prop; (void)data; (void)len;
    aux_note(&crypt_noted, "[crypt32] no certificate store (W32A-10)");
    w32_set_last_error(ctx ? 0x80092009u : 6u);
    return 0;
}

W32_DWORD W32ABI CertGetNameStringW(void *ctx, W32_DWORD type,
                                    W32_DWORD flags, void *type8,
                                    W32_LPWSTR out, W32_DWORD cch) {
    (void)type; (void)flags; (void)type8;
    aux_note(&crypt_noted, "[crypt32] no certificate store (W32A-10)");
    if (!ctx) {
        w32_set_last_error(6);
        return 0;
    }
    if (out && cch) out[0] = 0;
    return 0;                       /* zero characters: no name to give */
}

W32_BOOL W32ABI CertNameToStrW(W32_DWORD type, const void *blob,
                               W32_DWORD flags, W32_LPWSTR out, W32_DWORD cch) {
    (void)type; (void)blob; (void)flags;
    aux_note(&crypt_noted, "[crypt32] no certificate store (W32A-10)");
    w32_set_last_error(6);
    if (out && cch) out[0] = 0;
    return 0;
}

W32_BOOL W32ABI CryptMsgClose(W32_HANDLE msg) {
    (void)msg;
    aux_note(&crypt_noted, "[crypt32] no crypt message (W32A-10)");
    w32_set_last_error(6);
    return 0;
}

W32_BOOL W32ABI CryptMsgGetParam(W32_HANDLE msg, W32_DWORD type,
                                 W32_DWORD index, void *data, W32_DWORD *len) {
    (void)msg; (void)type; (void)index; (void)data; (void)len;
    aux_note(&crypt_noted, "[crypt32] no crypt message (W32A-10)");
    w32_set_last_error(6);
    return 0;
}

W32_BOOL W32ABI CertCloseStore(void *store, W32_DWORD flags) {
    (void)store; (void)flags;
    aux_note(&crypt_noted, "[crypt32] no certificate store (W32A-10)");
    w32_set_last_error(6);
    return 0;
}
