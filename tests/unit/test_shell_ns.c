/* WR-1 (W32RUN_PLAN.md) — the SHELL32 namespace, host unit test.
 *
 * Exercises w32/src/shell32_ns.c directly (amalgamated) against a scripted
 * VFS: the ONLY kernel32 surface the namespace touches — FindFirstFileW /
 * FindNextFileW / FindClose — is mocked here so the test links shell32_ns.c
 * alone (no kernel32_fs.c). The mock serves a small tree and supports both
 * the "parent\\*" enumeration form and the exact-path existence query the
 * code emits, so the same code path a real 7-Zip FM consumer drives is what
 * runs under ASan/UBSan.
 *
 * Tree:
 *     C:\fat            (dir)
 *     C:\readme.txt     (file)
 *     C:\sub            (dir)
 *     C:\fat\7zFM.exe   (file)
 *     C:\fat\DOCS       (dir)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "w32/shell32_priv.h"
#include "w32/kernel32.h"

/* ---- the scripted VFS -------------------------------------------------- */
struct ent { const char *path; int is_dir; };
static const struct ent TREE[] = {
    { "C:\\fat",          1 },
    { "C:\\readme.txt",   0 },
    { "C:\\sub",          1 },
    { "C:\\fat\\7zFM.exe", 0 },
    { "C:\\fat\\DOCS",    1 },
};
enum { TREE_N = (int)(sizeof TREE / sizeof TREE[0]) };

static int ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
/* directory that owns an entry: everything before the last backslash */
static void parent_of(const char *path, char *out) {
    strcpy(out, path);
    char *slash = strrchr(out, '\\');
    if (slash) *slash = 0; else out[0] = 0;
}
/* leaf name of an entry: everything after the last backslash */
static const char *leaf_of(const char *path) {
    const char *slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}
/* the directory a "...\\*" pattern lists (strip trailing '*' then any sep) */
static void dir_from_pattern(const char *pat, char *out) {
    strcpy(out, pat);
    size_t n = strlen(out);
    if (n && out[n - 1] == '*') out[--n] = 0;
    if (n && (out[n - 1] == '\\' || out[n - 1] == '/')) out[--n] = 0;
}

/* wide<->ascii for the mock boundary */
static void w2a(const uint16_t *w, char *a, size_t cap) {
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) a[i] = (char)(w[i] & 0xFF);
    a[i] = 0;
}
static void a2w(const char *a, uint16_t *w, size_t cap) {
    size_t i = 0;
    for (; a[i] && i + 1 < cap; i++) w[i] = (uint8_t)a[i];
    w[i] = 0;
}

/* the find handle: a list of matched TREE indices + a cursor */
struct mockfind { int idx[16]; int n; int pos; };

static void fill(W32_WIN32_FIND_DATAW *out, int e) {
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = TREE[e].is_dir ? W32_FILE_ATTRIBUTE_DIRECTORY : 0x80u /*NORMAL*/;
    a2w(leaf_of(TREE[e].path), out->cFileName, 260);
}

/* counters, so the test can prove FindClose balances FindFirst */
static int g_open = 0, g_closed = 0;

W32ABI W32_HANDLE FindFirstFileW(W32_LPCWSTR name, W32_WIN32_FIND_DATAW *out) {
    char a[512];
    w2a(name, a, sizeof a);
    struct mockfind mf; mf.n = 0; mf.pos = 0;

    size_t la = strlen(a);
    int wildcard = la && a[la - 1] == '*';
    if (wildcard) {
        char dir[512];
        dir_from_pattern(a, dir);
        for (int i = 0; i < TREE_N; i++) {
            char par[512];
            parent_of(TREE[i].path, par);
            if (ci_eq(par, dir)) mf.idx[mf.n++] = i;
        }
    } else {
        for (int i = 0; i < TREE_N; i++)
            if (ci_eq(TREE[i].path, a)) { mf.idx[mf.n++] = i; break; }
    }
    if (mf.n == 0) return W32_INVALID_HANDLE_VALUE;

    struct mockfind *h = (struct mockfind *)malloc(sizeof *h);
    *h = mf;
    fill(out, h->idx[0]);
    h->pos = 1;
    g_open++;
    return (W32_HANDLE)h;
}
W32ABI W32_BOOL FindNextFileW(W32_HANDLE handle, W32_WIN32_FIND_DATAW *out) {
    struct mockfind *h = (struct mockfind *)handle;
    if (!h || h->pos >= h->n) return 0;
    fill(out, h->idx[h->pos++]);
    return 1;
}
W32ABI W32_BOOL FindClose(W32_HANDLE handle) {
    if (handle && handle != W32_INVALID_HANDLE_VALUE) { free(handle); g_closed++; return 1; }
    return 0;
}

/* ---- the object under test -------------------------------------------- */
#include "../../w32/src/shell32_ns.c"

static int n_checks, n_fail;
#define CHECK(x) do { n_checks++; if (!(x)) { n_fail++; fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); } } while (0)

/* convenient casts to the real vtbl wrappers */
#define SF(p)  ((W32_IShellFolder *)(p))
#define EN(p)  ((W32_IEnumIDList *)(p))
#define SFV(p) (SF(p)->vtbl)
#define ENV(p) (EN(p)->vtbl)

static int wide_eq_a(const uint16_t *w, const char *a) {
    size_t i = 0;
    for (; a[i]; i++) if (w[i] != (uint16_t)(uint8_t)a[i]) return 0;
    return w[i] == 0;
}

/* pull every pidl out of an enumerator (one at a time, S_FALSE terminates) */
static size_t drain(void *en, void **buf, size_t cap) {
    size_t got = 0;
    for (;;) {
        void *pidl = NULL; W32_UINT f = 0;
        W32_LONG hr = ENV(en)->Next(en, 1, &pidl, &f);
        if (hr != 0 /*S_OK*/ || f != 1) { if (pidl) free(pidl); break; }
        if (got < cap) buf[got] = pidl; else free(pidl);
        got++;
    }
    return got;
}

int main(void) {
    /* --- SHGetDesktopFolder --- */
    void *desktop = NULL;
    CHECK(ns_get_desktop_folder(&desktop) == 0 && desktop);
    CHECK(ns_get_desktop_folder(NULL) == (W32_LONG)0x80004003uL); /* E_POINTER */

    /* QueryInterface: IShellFolder, IUnknown yes; garbage no */
    void *qi = NULL;
    CHECK(SFV(desktop)->QueryInterface(desktop, W32_IID_IShellFolder, &qi) == 0 && qi == desktop);
    SFV(desktop)->Release(desktop); /* balance the AddRef QI did */
    CHECK(SFV(desktop)->QueryInterface(desktop, W32_IID_IUnknown, &qi) == 0 && qi == desktop);
    SFV(desktop)->Release(desktop);
    uint8_t bad[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    qi = (void *)1;
    CHECK(SFV(desktop)->QueryInterface(desktop, bad, &qi) == (W32_LONG)0x80004002uL && qi == NULL);

    /* --- Desktop enumerates exactly one child: My Computer --- */
    void *en = NULL;
    CHECK(SFV(desktop)->EnumObjects(desktop, NULL, W32_SHCONTF_FOLDERS, &en) == 0 && en);
    void *comp_pidl[4];
    size_t nc = drain(en, comp_pidl, 4);
    CHECK(nc == 1);
    ENV(en)->Release(en);

    /* My Computer displays as "Computer"; is a browsable folder */
    W32_STRRET sr; memset(&sr, 0, sizeof sr);
    CHECK(SFV(desktop)->GetDisplayNameOf(desktop, comp_pidl[0], W32_SHGDN_NORMAL, &sr) == 0);
    CHECK(sr.uType == W32_STRRET_WSTR && wide_eq_a(sr.pOleStr, "Computer"));
    free(sr.pOleStr);
    { const void *ap[1] = { comp_pidl[0] }; W32_UINT a = 0xFFFFFFFFu;
      CHECK(SFV(desktop)->GetAttributesOf(desktop, 1, ap, &a) == 0);
      CHECK((a & W32_SFGAO_FOLDER) && (a & W32_SFGAO_HASSUBFOLDER) && !(a & W32_SFGAO_FILESYSTEM)); }

    /* --- BindToObject(My Computer) -> the Computer folder, enum -> C:\ --- */
    void *computer = NULL;
    CHECK(SFV(desktop)->BindToObject(desktop, comp_pidl[0], NULL, W32_IID_IShellFolder, &computer) == 0 && computer);
    CHECK(SFV(computer)->EnumObjects(computer, NULL, W32_SHCONTF_FOLDERS | W32_SHCONTF_NONFOLDERS, &en) == 0);
    void *drive_pidl[4];
    size_t nd = drain(en, drive_pidl, 4);
    CHECK(nd == 1);
    ENV(en)->Release(en);
    /* the drive shows as "C:" normally, "C:\" for parsing */
    CHECK(SFV(computer)->GetDisplayNameOf(computer, drive_pidl[0], W32_SHGDN_NORMAL, &sr) == 0 && wide_eq_a(sr.pOleStr, "C:"));
    free(sr.pOleStr);
    CHECK(SFV(computer)->GetDisplayNameOf(computer, drive_pidl[0], W32_SHGDN_FORPARSING, &sr) == 0 && wide_eq_a(sr.pOleStr, "C:\\"));
    free(sr.pOleStr);

    /* --- BindToObject(C:\) -> the CFSFolder, enum -> root children --- */
    void *root = NULL;
    CHECK(SFV(computer)->BindToObject(computer, drive_pidl[0], NULL, W32_IID_IShellFolder, &root) == 0 && root);
    CHECK(SFV(root)->EnumObjects(root, NULL, W32_SHCONTF_FOLDERS | W32_SHCONTF_NONFOLDERS, &en) == 0);
    void *kids[8];
    size_t nk = drain(en, kids, 8);
    CHECK(nk == 3); /* fat, readme.txt, sub */
    ENV(en)->Release(en);

    /* folders-only filter drops the file */
    CHECK(SFV(root)->EnumObjects(root, NULL, W32_SHCONTF_FOLDERS, &en) == 0);
    void *fo[8]; size_t nfo = drain(en, fo, 8);
    CHECK(nfo == 2);
    ENV(en)->Release(en);
    for (size_t i = 0; i < nfo; i++) free(fo[i]);

    /* find the "fat" child, check display + attributes (a folder w/ subfolders) */
    void *fat_pidl = NULL, *file_pidl = NULL;
    for (size_t i = 0; i < nk; i++) {
        memset(&sr, 0, sizeof sr);
        SFV(root)->GetDisplayNameOf(root, kids[i], W32_SHGDN_INFOLDER, &sr);
        if (wide_eq_a(sr.pOleStr, "fat")) fat_pidl = kids[i];
        if (wide_eq_a(sr.pOleStr, "readme.txt")) file_pidl = kids[i];
        free(sr.pOleStr);
    }
    CHECK(fat_pidl && file_pidl);
    /* full parse name resolves to the absolute path */
    CHECK(SFV(root)->GetDisplayNameOf(root, fat_pidl, W32_SHGDN_FORPARSING, &sr) == 0 && wide_eq_a(sr.pOleStr, "C:\\fat"));
    free(sr.pOleStr);
    { const void *ap[1] = { fat_pidl }; W32_UINT a = 0xFFFFFFFFu;
      SFV(root)->GetAttributesOf(root, 1, ap, &a);
      CHECK((a & W32_SFGAO_FOLDER) && (a & W32_SFGAO_FILESYSTEM) && (a & W32_SFGAO_HASSUBFOLDER)); }
    { const void *ap[1] = { file_pidl }; W32_UINT a = 0xFFFFFFFFu;
      SFV(root)->GetAttributesOf(root, 1, ap, &a);
      CHECK(!(a & W32_SFGAO_FOLDER) && (a & W32_SFGAO_STREAM) && (a & W32_SFGAO_CANDELETE)); }

    /* CompareIDs: fat < readme.txt < (itself == 0) */
    CHECK((int16_t)SFV(root)->CompareIDs(root, 0, fat_pidl, file_pidl) < 0);
    CHECK((int16_t)SFV(root)->CompareIDs(root, 0, fat_pidl, fat_pidl) == 0);

    /* --- BindToObject(fat) -> subfolder, enum -> its children --- */
    void *fatf = NULL;
    CHECK(SFV(root)->BindToObject(root, fat_pidl, NULL, W32_IID_IShellFolder, &fatf) == 0 && fatf);
    CHECK(SFV(fatf)->EnumObjects(fatf, NULL, W32_SHCONTF_FOLDERS | W32_SHCONTF_NONFOLDERS, &en) == 0);
    void *gk[8]; size_t ng = drain(en, gk, 8);
    CHECK(ng == 2); /* 7zFM.exe, DOCS */
    /* enumerator Skip/Reset behave */
    ENV(en)->Reset(en);
    CHECK(ENV(en)->Skip(en, 1) == 0);
    void *one = NULL; W32_UINT f1 = 0;
    CHECK(ENV(en)->Next(en, 1, &one, &f1) == 0 && f1 == 1);
    free(one);
    CHECK(ENV(en)->Next(en, 1, &one, &f1) == 1 /*S_FALSE*/ && f1 == 0);
    ENV(en)->Release(en);
    for (size_t i = 0; i < ng; i++) free(gk[i]);

    /* --- ParseDisplayName: relative hit, absolute hit, miss --- */
    void *pp = NULL; W32_UINT eaten = 0, pattrs = 0xFFFFFFFFu;
    uint16_t wname[64];
    a2w("7zFM.exe", wname, 64);
    CHECK(SFV(fatf)->ParseDisplayName(fatf, NULL, NULL, wname, &eaten, &pp, &pattrs) == 0 && pp);
    CHECK(pattrs & W32_SFGAO_FILESYSTEM);
    free(pp);
    a2w("C:\\readme.txt", wname, 64); pp = NULL;
    CHECK(SFV(root)->ParseDisplayName(root, NULL, NULL, wname, NULL, &pp, NULL) == 0 && pp);
    free(pp);
    a2w("nope.dat", wname, 64); pp = (void *)1;
    CHECK(SFV(fatf)->ParseDisplayName(fatf, NULL, NULL, wname, NULL, &pp, NULL) == (W32_LONG)0x80070002uL && pp == NULL);

    /* --- the deliberately-unimplemented GUI verbs fail clean, named --- */
    void *v = (void *)1;
    CHECK(SFV(root)->CreateViewObject(root, NULL, W32_IID_IUnknown, &v) == (W32_LONG)0x80004001uL && v == NULL);
    { const void *ap[1] = { fat_pidl }; void *u = (void *)1;
      CHECK(SFV(root)->GetUIObjectOf(root, NULL, 1, ap, W32_IID_IUnknown, NULL, &u) == (W32_LONG)0x80004001uL && u == NULL); }
    v = (void *)1;
    CHECK(SFV(root)->BindToStorage(root, fat_pidl, NULL, W32_IID_IUnknown, &v) == (W32_LONG)0x80004001uL && v == NULL);
    void *op = (void *)1;
    a2w("x", wname, 64);
    CHECK(SFV(root)->SetNameOf(root, NULL, fat_pidl, wname, 0, &op) == (W32_LONG)0x80004001uL && op == NULL);

    /* --- ns_pidl_to_path: FS -> path, Computer -> 0, garbage -> -1 --- */
    uint16_t path[64];
    CHECK(ns_pidl_to_path(fat_pidl, path, 64) == 6 && wide_eq_a(path, "C:\\fat"));
    CHECK(ns_pidl_to_path(comp_pidl[0], path, 64) == 0);
    uint16_t junk[2] = { 3 /*cb too small*/, 0 };
    CHECK(ns_pidl_to_path(junk, path, 64) == -1);

    /* --- release the whole graph; then prove Find handles all closed --- */
    SFV(fatf)->Release(fatf);
    SFV(root)->Release(root);
    SFV(computer)->Release(computer);
    SFV(desktop)->Release(desktop);
    for (size_t i = 0; i < nk; i++) free(kids[i]);
    free(drive_pidl[0]);
    free(comp_pidl[0]);
    CHECK(g_open == g_closed && g_open > 0);

    fprintf(stderr, "shell-ns: %d checks, %d failures (finds opened=%d closed=%d)\n",
            n_checks, n_fail, g_open, g_closed);
    return n_fail ? 1 : 0;
}
