/* shlwapi.c — W32APP_PLAN.md phase W32A-10: the SHLWAPI pure modules.
 *
 * The Path* family is UTF-16 string surgery with the documented
 * Win32 path semantics (slash = \ or /, drive = [A-Za-z]:); the Color*
 * family is the documented HLS arithmetic with 0..240 WORD ranges;
 * AssocQueryStringW answers from an association table that is empty
 * by design.  Nothing here touches the filesystem except
 * PathFileExistsW, and nothing here is stubbed.
 *
 * Licensed Apache-2.0.  Interface facts only (see shlwapi.h,
 * w32/PROVENANCE.md).
 */

#include "w32/shlwapi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* The Win32->host path translation the file API uses (kernel32_fs.c). */
extern char *w32_fs_xlate_dup(const char *p);

/* ---- shared helpers ---------------------------------------------------------- */

static int p_is_slash(uint16_t c) { return c == '\\' || c == '/'; }

static size_t p_len(const uint16_t *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Fold one ASCII character (the documented locale-free contract). */
static uint16_t p_fold(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c - 'A' + 'a');
    return c;
}

static int p_eq(uint16_t a, uint16_t b) { return p_fold(a) == p_fold(b); }

static int p_has_drive(const uint16_t *s) {
    if (!s) return 0;
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':')
        return 1;
    return 0;
}

static int p_is_abs(const uint16_t *s) {
    if (!s) return 0;
    if (p_has_drive(s)) return 1;
    return p_is_slash(s[0]);
}

/* Copy at most cap-1 units + NUL.  Returns units written. */
static size_t p_copy(uint16_t *dst, const uint16_t *src, size_t cap) {
    size_t i = 0;
    if (!dst || cap == 0) return 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

static void p_cat(uint16_t *dst, const uint16_t *src, size_t cap) {
    size_t d = p_len(dst);
    size_t i = 0;
    while (src && src[i] && d + 1 < cap) dst[d++] = src[i++];
    dst[d] = 0;
}

/* ---- PathFindFileNameW / PathFindExtensionW ---------------------------------- */

W32_LPCWSTR W32ABI PathFindFileNameW(W32_LPCWSTR path) {
    if (!path) return NULL;
    size_t n = p_len(path);
    size_t i = n;
    while (i > 0) {
        i--;
        if (p_is_slash(path[i]))
            return path + i + 1;
    }
    return path;                    /* no slash: the whole string */
}

W32_LPCWSTR W32ABI PathFindExtensionW(W32_LPCWSTR path) {
    if (!path) return NULL;
    const uint16_t *name = PathFindFileNameW(path);
    size_t n = p_len(name);
    size_t i = n;
    while (i > 0) {
        i--;
        if (name[i] == '.') {
            if (i == 0)
                break;              /* ".profile" style: no extension */
            return name + i;
        }
    }
    return name + n;                /* the terminating NUL */
}

/* ---- PathRemoveFileSpecW / PathStripPathW ------------------------------------ */

W32_BOOL W32ABI PathRemoveFileSpecW(W32_LPWSTR path) {
    if (!path) return 0;
    size_t n = p_len(path);
    size_t i = n;
    while (i > 0) {
        i--;
        if (p_is_slash(path[i])) {
            path[i] = 0;
            return 1;
        }
    }
    return 0;
}

void W32ABI PathStripPathW(W32_LPWSTR path) {
    if (!path) return;
    const uint16_t *name = PathFindFileNameW(path);
    if (name == path) return;
    size_t i = 0;
    while (name[i]) {
        path[i] = name[i];
        i++;
    }
    path[i] = 0;
}

/* ---- PathAppendW / PathCombineW ----------------------------------------------- */

W32_BOOL W32ABI PathAppendW(W32_LPWSTR path, W32_LPCWSTR more) {
    if (!path || !more) return 0;
    if (!more[0]) return 1;
    if (p_is_abs(more)) {           /* an absolute `more` replaces */
        p_copy(path, more, W32_MAX_PATH);
        return 1;
    }
    size_t n = p_len(path);
    if (n && !p_is_slash(path[n - 1]) && n + 1 < W32_MAX_PATH)
        path[n++] = '\\';
    path[n] = 0;
    p_cat(path, more, W32_MAX_PATH);
    return 1;
}

W32_LPWSTR W32ABI PathCombineW(W32_LPWSTR dest, W32_LPCWSTR dir,
                               W32_LPCWSTR file) {
    if (!dest) return NULL;
    if (!file || !file[0]) {
        if (dir) p_copy(dest, dir, W32_MAX_PATH);
        else dest[0] = 0;
        return dest;
    }
    if (p_is_abs(file) || !dir || !dir[0]) {
        p_copy(dest, file, W32_MAX_PATH);
        return dest;
    }
    p_copy(dest, dir, W32_MAX_PATH);
    PathAppendW(dest, file);
    return dest;
}

/* ---- PathAddExtensionW / PathRemoveExtensionW --------------------------------- */

W32_BOOL W32ABI PathAddExtensionW(W32_LPWSTR path, W32_LPCWSTR ext) {
    if (!path) return 0;
    static const uint16_t dot_exe[] = { '.', 'e', 'x', 'e', 0 };
    const uint16_t *e = (ext && ext[0]) ? ext : dot_exe;   /* documented default */
    /* "If the pszPath points to a NULL string, the result will be the
     * file name extension only." */
    if (!path[0]) {
        p_copy(path, e, W32_MAX_PATH);
        return 1;
    }
    const uint16_t *cur = PathFindExtensionW(path);
    if (cur && cur[0] == '.')
        return 1;                   /* already has one: TRUE, unchanged */
    p_cat(path, e, W32_MAX_PATH);
    return 1;
}

void W32ABI PathRemoveExtensionW(W32_LPWSTR path) {
    if (!path) return;
    const uint16_t *cur = PathFindExtensionW(path);
    if (cur && cur[0] == '.')
        ((uint16_t *)cur)[0] = 0;
}

/* ---- PathIsRelativeW / PathIsNetworkPathW / PathGetDriveNumberW ---------------- */

W32_BOOL W32ABI PathIsRelativeW(W32_LPCWSTR path) {
    if (!path) return 1;
    if (p_is_abs(path)) return 0;
    return 1;
}

W32_BOOL W32ABI PathIsNetworkPathW(W32_LPCWSTR path) {
    if (!path) return 0;
    if (path[0] == '\\' && path[1] == '\\')
        return 1;
    if (path[0] == '/' && path[1] == '/')
        return 1;
    return 0;
}

int W32ABI PathGetDriveNumberW(W32_LPCWSTR path) {
    if (!p_has_drive(path)) return -1;
    uint16_t c = path[0];
    if (c >= 'a' && c <= 'z') return c - 'a';
    return c - 'A';
}

/* ---- PathMatchSpecW ------------------------------------------------------------- */

/* The classic wildcard matcher: * spans, ? is one character, fold
 * ASCII case.  Iterative with a backtrack point (no recursion, so a
 * hostile pattern cannot blow the stack). */
W32_BOOL W32ABI PathMatchSpecW(W32_LPCWSTR path, W32_LPCWSTR spec) {
    if (!path || !spec) return 0;
    const uint16_t *p = path;
    const uint16_t *s = spec;
    const uint16_t *star = NULL, *back = NULL;
    for (;;) {
        if (*s == 0) {
            if (*p == 0) return 1;
            if (!star) return 0;
            s = star + 1;
            p = ++back;
            continue;
        }
        if (*s == '*') {
            star = s++;
            back = p;
            continue;
        }
        if (*p == 0) return 0;
        if (*s != '?' && !p_eq(*p, *s)) {
            if (!star) return 0;
            s = star + 1;
            p = ++back;
            continue;
        }
        p++;
        s++;
    }
}

/* ---- PathFileExistsW ------------------------------------------------------------ */

W32_BOOL W32ABI PathFileExistsW(W32_LPCWSTR path) {
    if (!path || !path[0]) return 0;
    char a[512];
    if (w32_utf16z_to_utf8(path, a, (int32_t)sizeof a) <= 0) return 0;
    char *host = w32_fs_xlate_dup(a);
    if (!host) return 0;            /* bad spelling: does not exist */
    struct stat st;
    int ok = (stat(host, &st) == 0);
    free(host);
    return ok ? 1 : 0;
}

/* ---- PathCompactPathExW ----------------------------------------------------------- */

W32_BOOL W32ABI PathCompactPathExW(W32_LPWSTR out, W32_LPCWSTR path,
                                   W32_UINT cchMax, W32_DWORD flags) {
    (void)flags;
    if (!out || !path) return 0;
    if (cchMax == 0) {
        out[0] = 0;
        return 1;
    }
    size_t n = p_len(path);
    if (n <= cchMax) {              /* fits: verbatim */
        p_copy(out, path, (size_t)cchMax + 1);
        return 0;
    }
    if (cchMax <= 3) {              /* nothing to show but ellipses */
        size_t i = 0;
        while (i + 1 < (size_t)cchMax + 1 && i < 3) {
            out[i] = '.';
            i++;
        }
        out[i < cchMax ? i : cchMax - 1] = 0;
        if (cchMax == 1) out[0] = 0;
        return 1;
    }
    static const uint16_t ell[] = { '.', '.', '.' };
    /* Keep the final component; eat leading components while it still
     * does not fit, prefixing "...\" — the documented
     * ellipses-from-the-front shape. */
    const uint16_t *name = PathFindFileNameW(path);
    size_t nn = p_len(name);
    size_t room = cchMax - 4;       /* "..." + '\' + name + NUL */
    if (nn <= room) {
        size_t i = 0;
        for (i = 0; i < 3; i++) out[i] = ell[i];
        out[3] = '\\';
        p_copy(out + 4, name, (size_t)cchMax - 3);
        return 1;
    }
    /* The name alone is too long: keep its head + "...". */
    size_t keep = cchMax - 3;
    size_t i = 0;
    for (i = 0; i < keep; i++) out[i] = name[i];
    for (i = 0; i < 3; i++) out[keep + i] = ell[i];
    out[keep + 3] = 0;
    return 1;
}

/* ---- Color* (the documented HLS arithmetic, 0..240) ------------------------------- */

static int c_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int c_min3(int a, int b, int c) { int m = a; if (b < m) m = b; if (c < m) m = c; return m; }
static int c_max3(int a, int b, int c) { int m = a; if (b > m) m = b; if (c > m) m = c; return m; }

void W32ABI ColorRGBToHLS(W32_DWORD rgb, W32_WORD *hue, W32_WORD *lum,
                          W32_WORD *sat) {
    int r = (int)(rgb & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)((rgb >> 16) & 0xFF);
    int mx = c_max3(r, g, b);
    int mn = c_min3(r, g, b);
    int l = (mx + mn) * 120 / 255;          /* 0..240 */
    int s = 0;
    int h = 0;
    if (mx != mn) {
        int d = mx - mn;
        int ll = mx + mn;
        s = (ll <= 255) ? (d * 240 / ll) : (d * 240 / (510 - ll));
        if (mx == r)
            h = 40 * (g - b) / d;
        else if (mx == g)
            h = 40 * (b - r) / d + 80;
        else
            h = 40 * (r - g) / d + 160;
        h = c_clamp(h, 0, 239);             /* the documented 0..240 range */
        if (h < 0) h += 240;
    }
    if (hue) *hue = (W32_WORD)(mx == mn ? 0 : h);
    if (lum) *lum = (W32_WORD)l;
    if (sat) *sat = (W32_WORD)s;
}

W32_DWORD W32ABI ColorHLSToRGB(W32_WORD h, W32_WORD l, W32_WORD s) {
    if (s == 0) {                           /* achromatic */
        int v = l * 255 / 240;
        v = c_clamp(v, 0, 255);
        return (W32_DWORD)v | ((W32_DWORD)v << 8) | ((W32_DWORD)v << 16);
    }
    /* The documented anchor points: hue sector 0..240, magenta 200
     * wraps to red.  Six sectors of 40 units. */
    int hh = h % 240;
    int sector = hh / 40;
    int rem = hh % 40;
    /* luminance 0..240 -> value scale; the classic integer HLS shape */
    int l255 = (int)l * 255 / 240;
    int s255 = (int)s * 255 / 240;
    int maxc, minc;
    if (l255 <= 127) {
        maxc = l255 * (255 + s255) / 255;
        minc = l255 * (255 - s255) / 255;
    } else {
        maxc = l255 + (255 - l255) * s255 / 255;
        minc = l255 - (255 - l255) * s255 / 255;
    }
    int span = maxc - minc;
    int r, g, b;
    int frac = rem * span / 40;
    switch (sector) {
    case 0: r = maxc; g = minc + frac; b = minc; break;
    case 1: r = minc + (span - frac); g = maxc; b = minc; break;
    case 2: r = minc; g = maxc; b = minc + frac; break;
    case 3: r = minc; g = minc + (span - frac); b = maxc; break;
    case 4: r = minc + frac; g = minc; b = maxc; break;
    default: r = maxc; g = minc; b = minc + (span - frac); break;
    }
    r = c_clamp(r, 0, 255);
    g = c_clamp(g, 0, 255);
    b = c_clamp(b, 0, 255);
    return (W32_DWORD)r | ((W32_DWORD)g << 8) | ((W32_DWORD)b << 16);
}

W32_DWORD W32ABI ColorAdjustLuma(W32_DWORD rgb, int n, W32_BOOL scaled) {
    W32_WORD h = 0, l = 0, s = 0;
    ColorRGBToHLS(rgb, &h, &l, &s);
    /* n is in 0.1% of the full range; the full range is 240 units. */
    int delta = n * 240 / 1000;
    int nl;
    if (scaled)
        nl = (int)l + delta;
    else
        nl = delta;
    nl = c_clamp(nl, 0, 240);
    return ColorHLSToRGB(h, (W32_WORD)nl, s);
}

/* ---- AssocQueryStringW ------------------------------------------------------------ */

static int assoc_noted;

W32_LONG W32ABI AssocQueryStringW(W32_DWORD flags, W32_DWORD str,
                                  W32_LPCWSTR assoc, W32_LPCWSTR extra,
                                  W32_LPWSTR out, W32_DWORD *pcchOut) {
    (void)flags;
    (void)str;
    (void)assoc;
    (void)extra;
    if (!assoc_noted) {
        assoc_noted = 1;
        printf("w32: [shlwapi] AssocQueryStringW: the association table is "
               "empty by design (docs/win32.md)\n");
    }
    if (pcchOut)
        *pcchOut = 0;
    if (out)
        out[0] = 0;
    /* S_FALSE (0x00000001): the documented no-association result. */
    return 1;
}
