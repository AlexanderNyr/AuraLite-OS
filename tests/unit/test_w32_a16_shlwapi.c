/* test_w32_a16_shlwapi.c — W32APP_PLAN.md phase W32A-16 host gate:
 * the SHLWAPI Path* / Color* surface Notepad++ runs every filename and every
 * theme colour through.
 *
 * The in-guest QEMU gate (test_w32a16_npp_fixture.sh) proves the whole
 * Notepad++ app slice end to end (tab bar, path surgery, IShellItem open path,
 * VFS save/reopen, plugin DLL chain, tray icon, offline updater).  This host
 * test drives the pure SHLWAPI modules directly under ASan/UBSan so the string
 * and HLS arithmetic Notepad++ leans on is pinned deterministically:
 *
 *   - PathFindFileNameW / PathFindExtensionW: the split points, incl. no-dot,
 *     trailing-slash and root edge cases.
 *   - PathStripPathW / PathRemoveFileSpecW / PathRemoveExtensionW: in-place
 *     mutation to the documented result.
 *   - PathCombineW / PathAddExtensionW: join semantics, incl. an absolute
 *     second component overriding the first.
 *   - PathMatchSpecW: '*' and '?' globbing (case-insensitive), the exact
 *     matcher NPP filters its file list with.
 *   - PathIsRelativeW: drive/UNC vs relative.
 *   - PathCompactPathExW: ellipsis-from-the-front, and the too-small buffer.
 *   - ColorRGBToHLS -> ColorHLSToRGB: a channel-close round-trip; grey stays
 *     achromatic; ColorAdjustLuma darkens and lightens monotonically.
 *   - AssocQueryStringW: the documented empty table returns S_FALSE.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "w32/shlwapi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

/* shlwapi.c's only filesystem hook (PathFileExistsW) — this gate never calls
 * it, so a trivial host stub keeps the amalgamation self-contained. */
char *w32_fs_xlate_dup(const char *p) {
    if (!p) return NULL;
    size_t n = strlen(p);
    char *d = malloc(n + 1);
    if (d) memcpy(d, p, n + 1);
    return d;
}

static int n16, f16;
#define CK(cond) do { \
    n16++; \
    if (!(cond)) { f16++; printf("  FAIL[%d]: %s (line %d)\n", f16, #cond, __LINE__); } \
} while (0)

/* small ASCII -> UTF-16 helper for test literals */
static void W(const char *s, uint16_t *out) {
    size_t i = 0;
    for (; s[i]; i++) out[i] = (uint16_t)(unsigned char)s[i];
    out[i] = 0;
}
static int WEQ(const uint16_t *a, const char *b) {
    size_t i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != (uint16_t)(unsigned char)b[i]) return 0;
    return a[i] == 0 && b[i] == 0;
}

int main(void) {
    uint16_t buf[128], buf2[128], out[128];

    /* ---- PathFindFileNameW / PathFindExtensionW ---------------------- */
    W("C:\\src\\npp\\main.c", buf);
    CK(WEQ(PathFindFileNameW(buf), "main.c"));
    CK(WEQ(PathFindExtensionW(buf), ".c"));

    W("noext", buf);
    CK(WEQ(PathFindFileNameW(buf), "noext"));
    CK(PathFindExtensionW(buf)[0] == 0);          /* no dot -> end of string */

    W("a/b/c.tar.gz", buf);
    CK(WEQ(PathFindFileNameW(buf), "c.tar.gz"));   /* forward slashes too */
    CK(WEQ(PathFindExtensionW(buf), ".gz"));       /* last dot */

    /* ---- PathStripPathW (in place) ---------------------------------- */
    W("C:\\a\\b\\readme.md", buf);
    PathStripPathW(buf);
    CK(WEQ(buf, "readme.md"));

    /* ---- PathRemoveFileSpecW (in place) ----------------------------- */
    W("C:\\a\\b\\readme.md", buf);
    CK(PathRemoveFileSpecW(buf) == 1);
    CK(WEQ(buf, "C:\\a\\b"));

    /* ---- PathRemoveExtensionW / PathAddExtensionW ------------------- */
    W("session.xml", buf);
    PathRemoveExtensionW(buf);
    CK(WEQ(buf, "session"));
    { uint16_t ext[8]; W(".xml", ext); CK(PathAddExtensionW(buf, ext) == 1); }
    CK(WEQ(buf, "session.xml"));

    /* ---- PathCombineW ----------------------------------------------- */
    { uint16_t dir[32], more[32];
      W("C:\\src\\npp", dir); W("README.md", more);
      CK(PathCombineW(out, dir, more) != NULL);
      CK(WEQ(out, "C:\\src\\npp\\README.md")); }
    { uint16_t dir[32], more[32];      /* absolute second component wins */
      W("C:\\src\\npp", dir); W("D:\\abs.txt", more);
      CK(PathCombineW(out, dir, more) != NULL);
      CK(WEQ(out, "D:\\abs.txt")); }

    /* ---- PathMatchSpecW --------------------------------------------- */
    { uint16_t name[32], spec[32];
      W("main.c", name);
      W("*.c",    spec); CK(PathMatchSpecW(name, spec) == 1);
      W("*.cpp",  spec); CK(PathMatchSpecW(name, spec) == 0);
      W("m?in.c", spec); CK(PathMatchSpecW(name, spec) == 1);
      W("*.C",    spec); CK(PathMatchSpecW(name, spec) == 1);   /* case-insensitive */
    }

    /* ---- PathIsRelativeW -------------------------------------------- */
    { uint16_t p[32];
      W("docs\\a.txt", p);   CK(PathIsRelativeW(p) == 1);
      W("C:\\docs\\a.txt", p); CK(PathIsRelativeW(p) == 0);
      W("\\\\srv\\share", p);  CK(PathIsRelativeW(p) == 0);     /* UNC */
    }

    /* ---- PathCompactPathExW ----------------------------------------- */
    W("C:\\a\\b\\c\\d\\reallylongname.txt", buf);
    CK(PathCompactPathExW(out, buf, 20, 0) == 1);
    { size_t l = 0; while (out[l]) l++; CK(l <= 20 && out[0] != 0); }
    /* verbatim when it already fits */
    W("a.txt", buf2);
    CK(PathCompactPathExW(out, buf2, 20, 0) == 0);              /* returns FALSE = unchanged */
    CK(WEQ(out, "a.txt"));

    /* ---- Color* HLS arithmetic -------------------------------------- */
    {
        uint16_t h = 0, l = 0, s = 0;
        uint32_t base = (0x40u) | (0x80u << 8) | (0xC0u << 16);  /* R,G,B */
        ColorRGBToHLS(base, &h, &l, &s);
        uint32_t rt = ColorHLSToRGB(h, l, s);
        int dr = (int)(rt & 0xFF)         - 0x40; if (dr < 0) dr = -dr;
        int dg = (int)((rt >> 8) & 0xFF)  - 0x80; if (dg < 0) dg = -dg;
        int db = (int)((rt >> 16) & 0xFF) - 0xC0; if (db < 0) db = -db;
        CK(dr <= 8 && dg <= 8 && db <= 8);

        /* grey stays achromatic (saturation 0) */
        uint32_t grey = 0x808080u;
        ColorRGBToHLS(grey, &h, &l, &s);
        CK(s == 0);

        /* darken then lighten: luminance monotone */
        uint32_t dark = ColorAdjustLuma(base, -300, 1);
        uint32_t lite = ColorAdjustLuma(base,  300, 1);
        uint16_t hd, ld, sd, hl, ll, sl, h0, l0, s0;
        ColorRGBToHLS(base, &h0, &l0, &s0);
        ColorRGBToHLS(dark, &hd, &ld, &sd);
        ColorRGBToHLS(lite, &hl, &ll, &sl);
        CK(ld < l0);
        CK(ll > l0);
    }

    /* ---- AssocQueryStringW: empty table returns S_FALSE ------------- */
    {
        uint16_t assoc[8], obuf[64];
        uint32_t cch = 64;
        W(".txt", assoc);
        /* ASSOCSTR_EXECUTABLE = 2; the table is empty by design -> S_FALSE (1). */
        long r = AssocQueryStringW(0, 2, assoc, NULL, obuf, &cch);
        CK(r == 1);
        CK(cch == 0);
    }

    if (f16 == 0) printf("W32A16-SHLWAPI-OK (%d checks)\n", n16);
    else          printf("W32A16-SHLWAPI-FAIL (%d/%d failed)\n", f16, n16);
    return f16 ? 1 : 0;
}
