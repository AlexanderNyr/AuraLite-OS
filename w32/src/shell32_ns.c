/* w32/src/shell32_ns.c — WR-1 (W32RUN_PLAN.md) the shell namespace.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Closes SHELL32 "plan section 7": a minimal-but-real IShellFolder /
 * IEnumIDList / PIDL namespace rooted at
 *
 *     Desktop  ->  My Computer  ->  C:  ->  the filesystem (CFSFolder)
 *
 * The filesystem folder enumerates the AuraLite VFS through the REAL
 * FindFirstFileW/FindNextFileW (kernel32_fs.c), so the one drive, C:, maps to
 * "/" exactly as every other path does. Scoped by measurement to the surface
 * the pinned 7-Zip FM imports (SHGetDesktopFolder + PIDL info) — W32RUN_PLAN
 * D-WR1: this surface, no more. The verbs a real consumer does not need on a
 * single-user machine (CreateViewObject, GetUIObjectOf, BindToStorage,
 * SetNameOf, Clone) fail clean with E_NOTIMPL — named, not faked (D-WR4).
 *
 * No foreign implementation code was consulted (see w32/LICENSING.md): the
 * vtable shapes are from the documented shobjidl ABI (mingw-w64 headers /
 * MSDN), which is a fact about an interface, the same class of fact
 * docs/win32.md already records.
 */

#include "w32/shell32_priv.h"
#include "w32/kernel32.h"        /* FindFirstFileW, WIN32_FIND_DATAW, attrs */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- HRESULTs (NS_ prefixed so a host build's <winerror.h> can't clash) -- */
#define NS_S_OK            0
#define NS_S_FALSE         1
#define NS_E_NOTIMPL       0x80004001uL
#define NS_E_NOINTERFACE   0x80004002uL
#define NS_E_POINTER       0x80004003uL
#define NS_E_OUTOFMEMORY   0x8007000EuL
#define NS_E_INVALIDARG    0x80070057uL
#define NS_E_FILENOTFOUND  0x80070002uL   /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */

/* The documented IIDs, little-endian in memory. */
const uint8_t W32_IID_IShellFolder[16] = {
    0xE6, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
const uint8_t W32_IID_IEnumIDList[16] = {
    0xF2, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
const uint8_t W32_IID_IUnknown[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};

/* ---- small wide-string helpers ---------------------------------------- */
static size_t nx_wlen(const uint16_t *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void   nx_wcpy(uint16_t *d, const uint16_t *s, size_t n) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
static int    nx_a2w(const char *a, uint16_t *w, size_t cap) {
    size_t i = 0;
    for (; a[i] && i + 1 < cap; i++) w[i] = (uint8_t)a[i];
    w[i] = 0;
    return (int)i;
}
static uint16_t nx_lower(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }
static int nx_wieq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    for (;; i++) {
        uint16_t ca = nx_lower(a[i]), cb = nx_lower(b[i]);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
static int nx_iid_eq(const void *riid, const uint8_t iid[16]) {
    return riid && memcmp(riid, iid, 16) == 0;
}
static int nx_is_sep(uint16_t c) { return c == '\\' || c == '/'; }

/* ---- the PIDL model (byte-compatible with shell32.c's single-item form) - */
/* Layout: { u16 cb, u16 kind, u16 units, path[units], u16 0(NUL), u16 0(term) }
 * cb counts the bytes AFTER cb, exactly as SHGetSpecialFolderLocation builds
 * them and SHGetPathFromIDListW decodes them. */
static uint16_t *nx_pidl_new(uint16_t kind, const uint16_t *path) {
    size_t units = path ? nx_wlen(path) : 0;
    size_t cb = 2 + 2 + (units + 1) * 2 + 2;
    uint16_t *p = (uint16_t *)malloc(cb + 2);
    if (!p) return NULL;
    p[0] = (uint16_t)cb;
    p[1] = kind;
    p[2] = (uint16_t)units;
    if (units) nx_wcpy(p + 3, path, units);
    p[3 + units] = 0;               /* path NUL */
    p[3 + units + 1] = 0;           /* terminating empty item (cb = 0) */
    return p;
}
static uint16_t *nx_pidl_clone(const uint16_t *p) {
    if (!p) return NULL;
    size_t total = (size_t)p[0] + 2;   /* cb field + cb bytes */
    uint16_t *c = (uint16_t *)malloc(total);
    if (c) memcpy(c, p, total);
    return c;
}

/* CSIDL constants for the virtual roots (documented values; the mapping is
 * ours). CSIDL_DRIVES ("My Computer") is a namespace root with no filesystem
 * path -- SHGetSpecialFolderLocation must hand back a PIDL for it, not E_FAIL,
 * or a real consumer (7-Zip FM enumerates drives from here) aborts at startup. */
#define NS_CSIDL_DRIVES        0x0011u
#define NS_CSIDL_NETWORK       0x0012u
#define NS_CSIDL_FLAG_MASK     0x7FFFu   /* strip CSIDL_FLAG_CREATE (0x8000) */

W32_LONG ns_special_pidl(W32_DWORD csidl, void **out) {
    if (!out) return NS_E_POINTER;
    *out = NULL;
    uint16_t kind;
    switch ((uint32_t)csidl & NS_CSIDL_FLAG_MASK) {
    case NS_CSIDL_DRIVES:  kind = W32_NS_PIDL_COMPUTER; break; /* My Computer */
    case NS_CSIDL_NETWORK: kind = W32_NS_PIDL_NETWORK;  break; /* Network root */
    default:               return NS_S_FALSE;  /* not a virtual root we model */
    }
    uint16_t *p = nx_pidl_new(kind, NULL);
    if (!p) return NS_E_OUTOFMEMORY;
    *out = p;
    return NS_S_OK;
}

int ns_pidl_to_path(const void *pidl, uint16_t *buf, size_t cap) {
    if (!pidl || !buf || cap == 0) return -1;
    const uint16_t *p = (const uint16_t *)pidl;
    uint16_t cb = p[0];
    if (cb < 8 || (cb & 1)) return -1;
    uint16_t kind = p[1];
    uint16_t units = p[2];
    if (kind == W32_NS_PIDL_COMPUTER || kind == W32_NS_PIDL_NETWORK ||
        units == 0) { buf[0] = 0; return 0; }
    if ((size_t)units + 1 > cap) units = (uint16_t)(cap - 1);
    nx_wcpy(buf, p + 3, units);
    buf[units] = 0;
    return (int)units;
}

/* ---- VFS existence / kind, over the REAL FindFirstFileW ----------------- */
static int nx_query(const uint16_t *path, int *is_dir) {
    size_t n = nx_wlen(path);
    if (n == 0) return 0;
    /* a drive root (C: or C:\) is a directory by construction */
    if (n <= 3 && path[1] == ':') { if (is_dir) *is_dir = 1; return 1; }

    uint16_t q[400];
    if (n > 399) n = 399;
    nx_wcpy(q, path, n); q[n] = 0;
    if (nx_is_sep(q[n - 1])) { q[--n] = 0; if (n <= 2) { if (is_dir) *is_dir = 1; return 1; } }

    W32_WIN32_FIND_DATAW fd;
    W32_HANDLE h = FindFirstFileW(q, &fd);
    if (h != W32_INVALID_HANDLE_VALUE) {
        if (is_dir) *is_dir = (fd.dwFileAttributes & W32_FILE_ATTRIBUTE_DIRECTORY) != 0;
        FindClose(h);
        return 1;
    }
    /* fall back: enumerate the parent and match the leaf (some VFS FindFirst
     * implementations only glob the last component) */
    size_t s = n;
    while (s > 0 && !nx_is_sep(q[s - 1])) s--;
    if (s == 0) return 0;
    uint16_t parent[400];
    nx_wcpy(parent, q, s);
    parent[s] = '*'; parent[s + 1] = 0;
    const uint16_t *leaf = q + s;
    W32_HANDLE h2 = FindFirstFileW(parent, &fd);
    if (h2 == W32_INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (nx_wieq(fd.cFileName, leaf)) {
            found = 1;
            if (is_dir) *is_dir = (fd.dwFileAttributes & W32_FILE_ATTRIBUTE_DIRECTORY) != 0;
            break;
        }
    } while (FindNextFileW(h2, &fd));
    FindClose(h2);
    return found;
}
static int nx_path_exists(const uint16_t *p) { return nx_query(p, NULL); }
static int nx_path_is_dir(const uint16_t *p) { int d = 0; if (!nx_query(p, &d)) return 0; return d; }

/* ---- the IEnumIDList object -------------------------------------------- */
typedef struct {
    const W32_IEnumIDListVtbl *vtbl;
    W32_DWORD refs;
    uint16_t **items;      /* owned pidls */
    size_t count, pos;
} ns_enum;

static const W32_IEnumIDListVtbl g_enum_vtbl;

static ns_enum *nx_enum_new(uint16_t **items, size_t n) {
    ns_enum *e = (ns_enum *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->vtbl = &g_enum_vtbl;
    e->refs = 1;
    e->items = items;
    e->count = n;
    e->pos = 0;
    return e;
}

static W32_LONG W32ABI en_qi(void *self, const void *riid, void **out) {
    if (!out) return NS_E_POINTER;
    *out = NULL;
    if (nx_iid_eq(riid, W32_IID_IEnumIDList) || nx_iid_eq(riid, W32_IID_IUnknown)) {
        *out = self; ((ns_enum *)self)->refs++; return NS_S_OK;
    }
    return NS_E_NOINTERFACE;
}
static W32_DWORD W32ABI en_addref(void *self) { return ++((ns_enum *)self)->refs; }
static W32_DWORD W32ABI en_release(void *self) {
    ns_enum *e = (ns_enum *)self;
    if (--e->refs == 0) {
        for (size_t i = 0; i < e->count; i++) free(e->items[i]);
        free(e->items);
        free(e);
        return 0;
    }
    return e->refs;
}
static W32_LONG W32ABI en_next(void *self, W32_UINT celt, void **rgelt, W32_UINT *fetched) {
    ns_enum *e = (ns_enum *)self;
    if (!rgelt) return NS_E_POINTER;
    W32_UINT got = 0;
    while (got < celt && e->pos < e->count) {
        uint16_t *c = nx_pidl_clone(e->items[e->pos]);
        if (!c) break;
        rgelt[got++] = c;
        e->pos++;
    }
    if (fetched) *fetched = got;
    return (got == celt) ? NS_S_OK : NS_S_FALSE;
}
static W32_LONG W32ABI en_skip(void *self, W32_UINT celt) {
    ns_enum *e = (ns_enum *)self;
    while (celt && e->pos < e->count) { e->pos++; celt--; }
    return celt ? NS_S_FALSE : NS_S_OK;
}
static W32_LONG W32ABI en_reset(void *self) { ((ns_enum *)self)->pos = 0; return NS_S_OK; }
static W32_LONG W32ABI en_clone(void *self, void **out) { (void)self; if (out) *out = NULL; return NS_E_NOTIMPL; }

static const W32_IEnumIDListVtbl g_enum_vtbl = {
    en_qi, en_addref, en_release, en_next, en_skip, en_reset, en_clone
};

/* ---- the IShellFolder object ------------------------------------------- */
enum { NS_DESKTOP = 0, NS_COMPUTER = 1, NS_FS = 2, NS_NETWORK = 3 };

typedef struct {
    const W32_IShellFolderVtbl *vtbl;
    W32_DWORD refs;
    int kind;
    uint16_t base[300];    /* Win path for NS_FS; empty for desktop/computer */
} ns_folder;

static const W32_IShellFolderVtbl g_folder_vtbl;

static ns_folder *nx_folder_new(int kind, const uint16_t *base) {
    ns_folder *f = (ns_folder *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->vtbl = &g_folder_vtbl;
    f->refs = 1;
    f->kind = kind;
    if (base) { size_t n = nx_wlen(base); if (n > 299) n = 299; nx_wcpy(f->base, base, n); f->base[n] = 0; }
    return f;
}

static W32_LONG W32ABI fld_qi(void *self, const void *riid, void **out) {
    if (!out) return NS_E_POINTER;
    *out = NULL;
    if (nx_iid_eq(riid, W32_IID_IShellFolder) || nx_iid_eq(riid, W32_IID_IUnknown)) {
        *out = self; ((ns_folder *)self)->refs++; return NS_S_OK;
    }
    return NS_E_NOINTERFACE;
}
static W32_DWORD W32ABI fld_addref(void *self) { return ++((ns_folder *)self)->refs; }
static W32_DWORD W32ABI fld_release(void *self) {
    ns_folder *f = (ns_folder *)self;
    if (--f->refs == 0) { free(f); return 0; }
    return f->refs;
}

static int nx_push(uint16_t ***arr, size_t *n, size_t *cap, uint16_t *item) {
    if (!item) return 0;
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        uint16_t **na = (uint16_t **)realloc(*arr, nc * sizeof(uint16_t *));
        if (!na) { free(item); return 0; }
        *arr = na; *cap = nc;
    }
    (*arr)[(*n)++] = item;
    return 1;
}

static W32_LONG W32ABI fld_enum(void *self, W32_HWND hwnd, W32_DWORD flags, void **ppenum) {
    (void)hwnd;
    ns_folder *f = (ns_folder *)self;
    if (!ppenum) return NS_E_POINTER;
    *ppenum = NULL;

    uint16_t **items = NULL;
    size_t n = 0, cap = 0;

    if (f->kind == NS_DESKTOP) {
        nx_push(&items, &n, &cap, nx_pidl_new(W32_NS_PIDL_COMPUTER, NULL));
        /* Network is reachable via SHGetSpecialFolderLocation(CSIDL_NETWORK)
         * + BindToObject (the surface 7-Zip FM measures); it is deliberately
         * not enumerated under the Desktop -- that node is not measured, and
         * D-WR1 keeps the namespace to exactly the surface a consumer drives. */
    } else if (f->kind == NS_COMPUTER) {
        uint16_t drive[] = { 'C', ':', '\\', 0 };
        nx_push(&items, &n, &cap, nx_pidl_new(W32_NS_PIDL_FS, drive));
    } else if (f->kind == NS_NETWORK) {
        /* No machines on a single-user offline box: a real, empty enumerator. */
    } else { /* NS_FS: read the directory */
        int want_folders = (flags & W32_SHCONTF_FOLDERS) || flags == 0;
        int want_files   = (flags & W32_SHCONTF_NONFOLDERS) || flags == 0;
        uint16_t pattern[320];
        size_t bl = nx_wlen(f->base);
        if (bl > 317) bl = 317;
        nx_wcpy(pattern, f->base, bl);
        size_t k = bl;
        if (k == 0 || !nx_is_sep(pattern[k - 1])) pattern[k++] = '\\';
        pattern[k++] = '*';
        pattern[k] = 0;

        W32_WIN32_FIND_DATAW fd;
        W32_HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != W32_INVALID_HANDLE_VALUE) {
            do {
                const uint16_t *nm = fd.cFileName;
                if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
                int is_dir = (fd.dwFileAttributes & W32_FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (is_dir && !want_folders) continue;
                if (!is_dir && !want_files) continue;

                uint16_t full[400];
                size_t fb = nx_wlen(f->base);
                if (fb > 397) fb = 397;
                nx_wcpy(full, f->base, fb);
                size_t fk = fb;
                if (fk == 0 || !nx_is_sep(full[fk - 1])) full[fk++] = '\\';
                size_t nl = nx_wlen(nm);
                if (fk + nl >= 400) nl = 399 - fk;
                nx_wcpy(full + fk, nm, nl);
                full[fk + nl] = 0;
                nx_push(&items, &n, &cap, nx_pidl_new(W32_NS_PIDL_FS, full));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    ns_enum *e = nx_enum_new(items, n);
    if (!e) {
        for (size_t i = 0; i < n; i++) free(items[i]);
        free(items);
        return NS_E_OUTOFMEMORY;
    }
    *ppenum = e;
    return NS_S_OK;
}

static W32_LONG W32ABI fld_bind(void *self, const void *pidl, void *pbc,
                                const void *riid, void **ppv) {
    (void)self; (void)pbc;
    if (!ppv) return NS_E_POINTER;
    *ppv = NULL;
    if (!pidl) return NS_E_INVALIDARG;
    const uint16_t *p = (const uint16_t *)pidl;
    uint16_t kind = p[1];
    ns_folder *child = NULL;
    if (kind == W32_NS_PIDL_COMPUTER) {
        child = nx_folder_new(NS_COMPUTER, NULL);
    } else if (kind == W32_NS_PIDL_NETWORK) {
        child = nx_folder_new(NS_NETWORK, NULL);
    } else if (kind == W32_NS_PIDL_FS) {
        uint16_t path[320];
        if (ns_pidl_to_path(pidl, path, 320) <= 0) return NS_E_INVALIDARG;
        child = nx_folder_new(NS_FS, path);
    } else {
        return NS_E_INVALIDARG;
    }
    if (!child) return NS_E_OUTOFMEMORY;
    W32_LONG hr = fld_qi(child, riid, ppv);
    fld_release(child);
    return hr;
}

static W32_LONG W32ABI fld_bindstorage(void *self, const void *pidl, void *pbc,
                                       const void *riid, void **ppv) {
    (void)self; (void)pidl; (void)pbc; (void)riid;
    if (ppv) *ppv = NULL;
    return NS_E_NOTIMPL;   /* no IStorage on the single-user VFS (named) */
}

static W32_LONG W32ABI fld_compare(void *self, intptr_t lparam,
                                   const void *p1, const void *p2) {
    (void)self; (void)lparam;
    uint16_t a[320], b[320];
    a[0] = 0; b[0] = 0;
    ns_pidl_to_path(p1, a, 320);
    ns_pidl_to_path(p2, b, 320);
    int c = 0;
    for (size_t i = 0;; i++) {
        uint16_t ca = nx_lower(a[i]), cb = nx_lower(b[i]);
        if (ca != cb) { c = (ca < cb) ? -1 : 1; break; }
        if (!ca) break;
    }
    return (W32_LONG)(int16_t)c;   /* HRESULT: the low word carries the order */
}

static W32_LONG W32ABI fld_createview(void *self, W32_HWND hwnd,
                                      const void *riid, void **ppv) {
    (void)self; (void)hwnd; (void)riid;
    if (ppv) *ppv = NULL;
    return NS_E_NOTIMPL;   /* the shell view is a GUI object — not this layer */
}

static W32_LONG W32ABI fld_getattrs(void *self, W32_UINT cidl,
                                    const void **apidl, W32_UINT *inout) {
    (void)self;
    if (!inout) return NS_E_POINTER;
    W32_UINT req = *inout;
    W32_UINT common = 0xFFFFFFFFu;
    for (W32_UINT i = 0; i < cidl; i++) {
        const uint16_t *p = (const uint16_t *)apidl[i];
        W32_UINT a = 0;
        uint16_t kind = p ? p[1] : 0;
        if (kind == W32_NS_PIDL_COMPUTER) {
            a = W32_SFGAO_FOLDER | W32_SFGAO_HASSUBFOLDER |
                W32_SFGAO_FILESYSANCESTOR | W32_SFGAO_BROWSABLE;
        } else if (kind == W32_NS_PIDL_NETWORK) {
            /* A browsable virtual folder with no filesystem underneath it and
             * no children on this offline box (HASSUBFOLDER is not set). */
            a = W32_SFGAO_FOLDER | W32_SFGAO_BROWSABLE;
        } else {
            uint16_t path[320];
            ns_pidl_to_path(p, path, 320);
            a = W32_SFGAO_FILESYSTEM;
            if (nx_path_is_dir(path))
                a |= W32_SFGAO_FOLDER | W32_SFGAO_FILESYSANCESTOR |
                     W32_SFGAO_HASSUBFOLDER | W32_SFGAO_BROWSABLE;
            else
                a |= W32_SFGAO_STREAM | W32_SFGAO_CANRENAME | W32_SFGAO_CANDELETE;
        }
        common &= a;
    }
    if (cidl == 0) common = 0;
    *inout = req & common;
    return NS_S_OK;
}

static W32_LONG W32ABI fld_getuiobject(void *self, W32_HWND hwnd, W32_UINT cidl,
                                       const void **apidl, const void *riid,
                                       W32_UINT *reserved, void **ppv) {
    (void)self; (void)hwnd; (void)cidl; (void)apidl; (void)riid; (void)reserved;
    if (ppv) *ppv = NULL;
    return NS_E_NOTIMPL;   /* icons/context menus are GUI objects (named) */
}

static W32_LONG W32ABI fld_getname(void *self, const void *pidl,
                                   W32_UINT flags, W32_STRRET *sr) {
    (void)self;
    if (!sr) return NS_E_POINTER;
    if (!pidl) return NS_E_INVALIDARG;
    const uint16_t *p = (const uint16_t *)pidl;
    uint16_t out[320];
    out[0] = 0;

    if (p[1] == W32_NS_PIDL_COMPUTER) {
        nx_a2w("Computer", out, 320);
    } else if (p[1] == W32_NS_PIDL_NETWORK) {
        nx_a2w("Network", out, 320);
    } else {
        uint16_t path[320];
        int r = ns_pidl_to_path(pidl, path, 320);
        if (r < 0) return NS_E_INVALIDARG;
        size_t n = nx_wlen(path);
        if (flags & W32_SHGDN_FORPARSING) {
            nx_wcpy(out, path, n); out[n] = 0;
        } else if (n > 0 && nx_is_sep(path[n - 1])) {
            /* a drive root "C:\" displays as "C:" */
            size_t e = n - 1;
            nx_wcpy(out, path, e); out[e] = 0;
        } else {
            size_t s = n;
            while (s > 0 && !nx_is_sep(path[s - 1])) s--;
            nx_wcpy(out, path + s, n - s); out[n - s] = 0;
        }
    }

    size_t on = nx_wlen(out);
    uint16_t *copy = (uint16_t *)malloc((on + 1) * 2u);
    if (!copy) return NS_E_OUTOFMEMORY;
    nx_wcpy(copy, out, on); copy[on] = 0;
    sr->uType = W32_STRRET_WSTR;
    sr->pOleStr = copy;
    return NS_S_OK;
}

static W32_LONG W32ABI fld_setname(void *self, W32_HWND hwnd, const void *pidl,
                                   const uint16_t *name, W32_UINT flags,
                                   void **ppidlOut) {
    (void)self; (void)hwnd; (void)pidl; (void)name; (void)flags;
    if (ppidlOut) *ppidlOut = NULL;
    return NS_E_NOTIMPL;   /* rename lives on SHFileOperationW, not here */
}

static W32_LONG W32ABI fld_parse(void *self, W32_HWND hwnd, void *pbc,
                                 uint16_t *name, W32_UINT *eaten,
                                 void **ppidl, W32_UINT *attrs) {
    (void)hwnd; (void)pbc;
    ns_folder *f = (ns_folder *)self;
    if (!ppidl) return NS_E_POINTER;
    *ppidl = NULL;
    if (!name || !name[0]) return NS_E_INVALIDARG;

    uint16_t full[400];
    int absolute = (name[0] && name[1] == ':') || nx_is_sep(name[0]);
    if (f->kind == NS_FS && !absolute) {
        size_t bl = nx_wlen(f->base);
        if (bl > 397) bl = 397;
        nx_wcpy(full, f->base, bl);
        size_t k = bl;
        if (k == 0 || !nx_is_sep(full[k - 1])) full[k++] = '\\';
        size_t nl = nx_wlen(name);
        if (k + nl >= 400) nl = 399 - k;
        nx_wcpy(full + k, name, nl);
        full[k + nl] = 0;
    } else {
        size_t nl = nx_wlen(name);
        if (nl > 399) nl = 399;
        nx_wcpy(full, name, nl); full[nl] = 0;
    }

    if (!nx_path_exists(full)) return NS_E_FILENOTFOUND;

    uint16_t *p = nx_pidl_new(W32_NS_PIDL_FS, full);
    if (!p) return NS_E_OUTOFMEMORY;
    *ppidl = p;
    if (eaten) *eaten = (W32_UINT)nx_wlen(name);
    if (attrs) {
        const void *ap[1] = { p };
        W32_UINT a = *attrs;
        fld_getattrs(self, 1, ap, &a);
        *attrs = a;
    }
    return NS_S_OK;
}

static const W32_IShellFolderVtbl g_folder_vtbl = {
    fld_qi, fld_addref, fld_release,
    fld_parse, fld_enum, fld_bind, fld_bindstorage, fld_compare,
    fld_createview, fld_getattrs, fld_getuiobject, fld_getname, fld_setname
};

/* ---- the SHGetDesktopFolder body -------------------------------------- */
W32_LONG ns_get_desktop_folder(void **out) {
    if (!out) return NS_E_POINTER;
    ns_folder *f = nx_folder_new(NS_DESKTOP, NULL);
    if (!f) { *out = NULL; return NS_E_OUTOFMEMORY; }
    *out = f;
    return NS_S_OK;
}
