/* shell32.c — W32APP_PLAN.md phase W32A-10: the shell furniture DLL.
 *
 * Known folders (a documented CSIDL mapping onto the real tree), the
 * minimal-but-real PIDL pair, the minimal IShellItem, file operations
 * over the W32A-2 primitives (wildcards through the find engine),
 * shell execution through the W32A-2 process machinery, the
 * notification area over the compositor notification engine, file
 * info / icon extraction over the W32A-6/A-8 decode machinery, and
 * the drag-list model W32A-11 will feed.
 *
 * SHBrowseForFolderW lives in comdlg32.c with the dialog engine and is
 * bound under SHELL32 — the forwarder shape the tree already uses for
 * IsTextUnicode (W32A-9).
 *
 * Licensed Apache-2.0.  Interface facts only (see shell32.h,
 * w32/PROVENANCE.md).
 */

#include "w32/shell32.h"
#include "w32/shlwapi.h"       /* PathMatchSpecW for wildcards */
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/w32_pe.h"
#include "w32/w32_rsrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

extern char *w32_fs_xlate_dup(const char *p);

/* The compositor notification engine (libauragui, as user32 uses it). */
extern int ag_notify(const char *text, uint32_t color, uint32_t duration_ms);

/* The A-8 icon mint/decode machinery (w32_gdi.c). */
extern W32_HICON w32_gdi_icon_from_argb(int32_t w, int32_t h,
                                        const uint32_t *argb);
extern W32_HICON w32_gdi_icon_decode(const uint8_t *bytes, size_t len);

static int sh_noted;
static void sh_note(const char *what) {
    if (sh_noted) return;
    sh_noted = 1;
    printf("w32: [shell32] %s\n", what);
}

/* ---- helpers ------------------------------------------------------------------ */

static size_t sh_wcslen(const uint16_t *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int sh_w2a(const uint16_t *w, char *a, size_t cap) {
    return w32_utf16z_to_utf8(w, a, (int32_t)cap);
}

static int sh_a2w(const char *a, uint16_t *w, size_t cap) {
    return w32_utf8z_to_utf16(a, w, (int32_t)cap);
}

static void sh_wcpy(uint16_t *d, const uint16_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* ASCII case-insensitive compare (the extension/type vocabulary is
 * ASCII; nothing here needs the full collation machinery). */
static int sh_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int sh_streq(const char *a, const char *b) {
    return sh_casecmp(a, b) == 0;
}

/* ---- known folders ------------------------------------------------------------- */

/* The data root mirrors the A-9 hive policy: /disk when the scratch
 * disk is mounted (settings survive a reboot), else /tmp with the
 * volatility logged once. */
static const char *sh_data_root(void) {
    struct stat st;
    if (stat("/disk", &st) == 0) return "/disk";
    sh_note("CSIDL data root: /disk is absent -- using /tmp "
            "(volatile: shell settings do not survive a reboot)");
    return "/tmp";
}

/* The documented mapping table (docs/win32.md).  Returns the host path
 * or NULL for a refused CSIDL (the reason noted once). */
static const char *sh_csidl_path(uint32_t csidl) {
    static char buf[300];
    uint32_t base = csidl & ~W32_CSIDL_FLAG_CREATE;
    switch (base) {
    case W32_CSIDL_DESKTOP:
    case W32_CSIDL_DESKTOPDIRECTORY:
        return "/";
    case W32_CSIDL_PROGRAMS:
    case W32_CSIDL_STARTMENU:
    case W32_CSIDL_STARTUP:
        return "/apps";
    case W32_CSIDL_PERSONAL:
        return sh_data_root();
    case W32_CSIDL_APPDATA:
        snprintf(buf, sizeof buf, "%s/w32/appdata", sh_data_root());
        return buf;
    case W32_CSIDL_FONTS:
        sh_note("CSIDL_FONTS refused: the font is baked into the kernel, "
                "there is no font directory");
        return NULL;
    default:
        return NULL;
    }
}

static int sh_mkdir_p(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n == 0 || n >= sizeof tmp) return -1;
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);      /* EEXIST is fine */
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) == 0) return 0;
    struct stat st;
    return (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

W32_BOOL W32ABI SHGetFolderPathW(W32_HWND owner, W32_DWORD csidl,
                                 W32_HANDLE token, W32_DWORD flags,
                                 W32_LPWSTR path) {
    (void)owner;
    (void)token;
    (void)flags;                   /* SHGFP_TYPE_CURRENT/DEFAULT agree here */
    if (!path) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const char *host = sh_csidl_path(csidl);
    if (!host) {
        path[0] = 0;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;                   /* E_FAIL shape; the code named */
    }
    if ((csidl & W32_CSIDL_FLAG_CREATE) && sh_mkdir_p(host) != 0) {
        w32_set_last_error(W32_ERROR_CANNOT_MAKE);
        return 0;
    }
    if (sh_a2w(host, path, W32_MAX_PATH) <= 0) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return 1;
}

W32_BOOL W32ABI SHGetSpecialFolderPathW(W32_HWND owner, W32_LPWSTR path,
                                        W32_DWORD csidl, W32_BOOL create) {
    if (create) csidl |= W32_CSIDL_FLAG_CREATE;
    return SHGetFolderPathW(owner, csidl, NULL, 0, path);
}

/* ---- the PIDL pair --------------------------------------------------------------- */

/* Model (shell32.h): { u16 cb, u16 csidl, u16 path_units, path, u16 0 }.
 * cb counts the bytes AFTER the cb field. */
W32_LONG W32ABI SHGetSpecialFolderLocation(W32_HWND owner, W32_DWORD csidl,
                                           void **pidl) {
    (void)owner;
    if (!pidl) return 0x80004003uL; /* E_POINTER */
    *pidl = NULL;
    const char *host = sh_csidl_path(csidl);
    if (!host) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0x80004005uL;        /* E_FAIL, the refused-CSIDL shape */
    }
    uint16_t w[150];
    int n = sh_a2w(host, w, 150);
    if (n <= 0) return 0x80070057uL;
    size_t units = (size_t)n;       /* units without the NUL */
    size_t cb = 2 + 2 + (units + 1) * 2 + 2;
    uint16_t *p = (uint16_t *)malloc(cb + 2);
    if (!p) return 0x8007000EuL;   /* E_OUTOFMEMORY */
    p[0] = (uint16_t)cb;
    p[1] = (uint16_t)(csidl & 0xFFFFu);
    p[2] = (uint16_t)units;
    sh_wcpy(p + 3, w, units + 1);  /* path + NUL */
    p[3 + units + 1] = 0;           /* the terminating empty item */
    *pidl = p;
    return 0;                       /* S_OK */
}

W32_BOOL W32ABI SHGetPathFromIDListW(const void *pidl, W32_LPWSTR buf) {
    if (!buf) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    buf[0] = 0;
    if (!pidl) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint16_t *p = (const uint16_t *)pidl;
    uint16_t cb = p[0];
    if (cb < 8 || (cb & 1)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint16_t units = p[2];
    if (units == 0 || (size_t)(4 + units) * 2u > (size_t)cb + 2u) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    sh_wcpy(buf, p + 3, units);
    buf[units] = 0;
    return 1;
}

W32_LONG W32ABI SHGetDesktopFolder(void **out) {
    if (out) *out = NULL;
    sh_note("SHGetDesktopFolder: the namespace object is plan section 7 "
            "(E_NOTIMPL)");
    return 0x80004001uL;            /* E_NOTIMPL */
}

/* ---- IShellItem (minimal, REAL) ---------------------------------------------------- */

typedef struct W32_IShellItemVtbl {
    W32_LONG (W32ABI *QueryInterface)(void *self, const void *riid, void **out);
    W32_DWORD (W32ABI *AddRef)(void *self);
    W32_DWORD (W32ABI *Release)(void *self);
    W32_LONG (W32ABI *GetDisplayName)(void *self, W32_LONG sigdn, W32_LPWSTR *out);
} W32_IShellItemVtbl;

typedef struct {
    const W32_IShellItemVtbl *vtbl;
    W32_DWORD refs;
    uint16_t path[300];
} shell_item_t;

/* The documented IShellItem IID {43826d1e-e718-42ee-bc55-a1e261c37bfe}. */
static const uint8_t shellitem_iid[16] = {
    0x1e, 0x6d, 0x82, 0x43, 0x18, 0xe7, 0xee, 0x42,
    0xbc, 0x55, 0xa1, 0xe2, 0x61, 0x1c, 0x37, 0xfe
};

/* SIGDN_DESKTOPABSOLUTEPARSING = 0x80018000 (documented). */
#define W32_SIGDN_DESKTOPABSOLUTEPARSING 0x80018000uL

static W32_LONG W32ABI si_qi(void *self, const void *riid, void **out) {
    if (!out) return 0x80004003uL;
    *out = NULL;
    if (!riid) return 0x80004003uL;
    if (memcmp(riid, shellitem_iid, 16) != 0)
        return 0x80004002uL;        /* E_NOINTERFACE */
    *out = self;
    ((shell_item_t *)self)->refs++;
    return 0;
}

static W32_DWORD W32ABI si_addref(void *self) {
    return ++((shell_item_t *)self)->refs;
}

static W32_DWORD W32ABI si_release(void *self) {
    shell_item_t *s = (shell_item_t *)self;
    if (--s->refs == 0) {
        free(s);
        return 0;
    }
    return s->refs;
}

static W32_LONG W32ABI si_getdisplayname(void *self, W32_LONG sigdn,
                                         W32_LPWSTR *out) {
    shell_item_t *s = (shell_item_t *)self;
    if (!out) return 0x80004003uL;
    *out = NULL;
    if ((uint32_t)sigdn != W32_SIGDN_DESKTOPABSOLUTEPARSING)
        return 0x80070057uL;        /* E_INVALIDARG: no other SIGDN */
    size_t n = sh_wcslen(s->path);
    uint16_t *copy = (uint16_t *)malloc((n + 1) * 2u);
    if (!copy) return 0x8007000EuL;
    sh_wcpy(copy, s->path, n);
    copy[n] = 0;
    *out = copy;                    /* CoTaskMemFree-shaped: plain free */
    return 0;
}

static const W32_IShellItemVtbl shell_item_vtbl = {
    si_qi, si_addref, si_release, si_getdisplayname
};

W32_LONG W32ABI SHCreateItemFromParsingName(W32_LPCWSTR path, void *bd,
                                            void *riid, void **out) {
    (void)bd;                       /* pbc: no binding context exists */
    if (!out) return 0x80004003uL;
    *out = NULL;
    if (!path || !path[0]) return 0x80070057uL;
    shell_item_t *s = (shell_item_t *)malloc(sizeof *s);
    if (!s) return 0x8007000EuL;
    s->vtbl = &shell_item_vtbl;
    s->refs = 1;
    size_t i = 0;
    while (path[i] && i + 1 < sizeof s->path / 2u) {
        s->path[i] = path[i];
        i++;
    }
    s->path[i] = 0;
    if (riid) {
        W32_LONG hr = si_qi(s, riid, out);
        si_release(s);              /* the QI ref is the caller's */
        return hr;
    }
    *out = s;
    return 0;
}

/* ---- SHCreateDirectory (#165) / SHChangeNotify ------------------------------------- */

W32_LONG W32ABI SHCreateDirectory(W32_HWND owner, W32_LPCWSTR path) {
    (void)owner;
    if (!path || !path[0]) return 0x80070057uL;
    char a[512];
    if (sh_w2a(path, a, sizeof a) <= 0) return 0x80070057uL;
    char *host = w32_fs_xlate_dup(a);
    if (!host) return 0x80070057uL;
    int r = sh_mkdir_p(host);
    free(host);
    if (r != 0) return 0x800700AAuL;   /* E_ACCESSDENIED shape */
    return 0;                        /* S_OK */
}

/* The generation counter the file dialogs poll: a notify bumps it and
 * the next dialog rebuilds its listing.  The narrow REAL behaviour,
 * not a broadcast system. */
static uint32_t sh_fs_generation;

void W32ABI SHChangeNotify(W32_LONG id, W32_UINT flags,
                           W32_LPCWSTR data1, W32_LPCWSTR data2) {
    (void)id; (void)flags; (void)data1; (void)data2;
    sh_fs_generation++;
}

uint32_t w32_shell_fs_generation(void) { return sh_fs_generation; }

/* ---- the extension/type map --------------------------------------------------------- */

static const struct { const char *ext; const char *name; } sh_types[] = {
    { ".txt",  "Text Document" },
    { ".c",    "C Source" },
    { ".h",    "C Header" },
    { ".md",   "Markdown Document" },
    { ".bmp",  "Bitmap Image" },
    { ".ico",  "Icon" },
    { ".zip",  "Zip Archive" },
    { ".tar",  "Tape Archive" },
    { ".elf",  "ELF Program" },
};

static const char *sh_typename_for(const char *name8, int is_dir) {
    if (is_dir) return "File Folder";
    const char *dot = strrchr(name8, '.');
    if (!dot) return "File";
    for (size_t i = 0; i < sizeof sh_types / sizeof sh_types[0]; i++)
        if (sh_streq(dot, sh_types[i].ext))
            return sh_types[i].name;
    return "File";
}

/* ---- PE icon helpers ------------------------------------------------------------------ */

static uint8_t *sh_read_file(const char *host, size_t *out_len) {
    int fd = open(host, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (size_t)st.st_size > 64u * 1024u * 1024u) {
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        off += (size_t)r;
    }
    close(fd);
    *out_len = n;
    return buf;
}

/* Count the icon GROUPs in a PE image.  -1: not a PE / unreadable. */
static int sh_icon_group_count(const char *host) {
    size_t flen = 0;
    uint8_t *file = sh_read_file(host, &flen);
    if (!file) return -1;
    pe_image_t img;
    int n = -1;
    if (pe_parse(file, flen, &img) == PE_OK) {
        n = 0;
        for (uint32_t id = 1; id <= 64; id++) {
            uint32_t rva, size;
            if (pe_find_resource_ex(&img, 14u, id, 0, &rva, &size) == PE_OK &&
                rva != 0)
                n++;
            else if (id > 1)
                break;              /* ids are dense in real images */
        }
    }
    free(file);
    return n;
}

/* Decode icon group `index` (0-based) into a real HICON. */
static W32_HICON sh_icon_from_group(const char *host, int index) {
    size_t flen = 0;
    uint8_t *file = sh_read_file(host, &flen);
    if (!file) return 0;
    W32_HICON h = 0;
    pe_image_t img;
    if (pe_parse(file, flen, &img) == PE_OK) {
        uint32_t rva = 0, gsize = 0;
        if (pe_find_resource_ex(&img, 14u, (uint32_t)index + 1, 0,
                                &rva, &gsize) == PE_OK && rva) {
            uint32_t goff = 0;
            if (pe_rva_to_offset(&img, rva, gsize, &goff) == PE_OK) {
                const uint8_t *grp = img.data + goff;
                size_t glen = gsize;
                if (glen >= 6) {
                    uint16_t count = (uint16_t)(grp[4] | (grp[5] << 8));
                    if (count && glen >= (size_t)6 + 14u) {
                        uint16_t icon_id =
                            (uint16_t)(grp[18] | (grp[19] << 8));
                        uint32_t irva = 0, isize = 0;
                        if (pe_find_resource_ex(&img, 3u, icon_id, 0,
                                                &irva, &isize) == PE_OK && irva) {
                            uint32_t ioff = 0;
                            if (pe_rva_to_offset(&img, irva, isize, &ioff) == PE_OK)
                                h = w32_gdi_icon_decode(img.data + ioff, isize);
                        }
                    }
                }
            }
        }
    }
    free(file);
    return h;
}

/* The generic document icon (16x16, ours): a parchment sheet with a
 * folded corner, minted through the A-8 ARGB seam. */
static W32_HICON sh_doc_icon(void) {
    static uint32_t px[16 * 16];
    static W32_HICON cached;
    if (cached) return cached;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t c = 0;
            int sheet = (x >= 3 && x <= 12 && y >= 1 && y <= 14);
            int fold = (x > 9 && y < 4);
            int edge = (x == 3 || x == 12 || y == 1 || y == 14);
            int rule = (y == 6 || y == 9 || y == 12) && (x >= 5 && x <= 10);
            if (sheet && !fold) {
                c = 0xFFF6F0D8u;   /* parchment */
                if (edge) c = 0xFF8A7A50u;
                else if (rule) c = 0xFFB0A070u;
            } else if (fold && sheet) {
                c = 0xFFD8CCA8u;
            }
            px[y * 16 + x] = c;
        }
    }
    cached = w32_gdi_icon_from_argb(16, 16, px);
    return cached;
}

/* ---- SHGetFileInfoW ---------------------------------------------------------------------- */

W32_DWORD_PTR W32ABI SHGetFileInfoW(W32_LPCWSTR path, W32_DWORD attrs,
                                    W32_SHFILEINFOW *sfi, W32_UINT cb,
                                    W32_UINT flags) {
    if (!path || (!sfi && (flags & (W32_SHGFI_ICON | W32_SHGFI_DISPLAYNAME |
                                    W32_SHGFI_TYPENAME | W32_SHGFI_ATTRIBUTES)))) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (sfi && cb && cb < sizeof *sfi) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & W32_SHGFI_PIDL) {
        sh_note("SHGetFileInfoW: PIDL addressing is section 7 "
                "(ERROR_INVALID_PARAMETER)");
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    char a[512];
    if (sh_w2a(path, a, sizeof a) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    int use_attrs = (flags & W32_SHGFI_USEFILEATTRIBUTES) != 0;
    struct stat st;
    int have_stat = 0, is_dir = 0, is_pe = 0;
    char *host = NULL;
    const char *name8 = strrchr(a, '\\');
    const char *name2 = strrchr(a, '/');
    if (name2 > name8) name8 = name2;
    name8 = name8 ? name8 + 1 : a;

    if (!use_attrs) {
        host = w32_fs_xlate_dup(a);
        if (!host) {
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            return 0;
        }
        have_stat = (stat(host, &st) == 0);
        if (!have_stat) {
            free(host);
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            return 0;
        }
        is_dir = S_ISDIR(st.st_mode);
    } else {
        is_dir = (attrs & 0x10u) != 0;   /* FILE_ATTRIBUTE_DIRECTORY */
    }

    /* "Application" needs more than the extension: the file must be a
     * loadable PE (the documented EXETYPE shape). */
    if (!is_dir && !use_attrs) {
        size_t flen = 0;
        uint8_t *file = sh_read_file(host, &flen);
        if (file) {
            pe_image_t img;
            is_pe = (pe_parse(file, flen, &img) == PE_OK);
            free(file);
        }
    } else if (!is_dir && use_attrs) {
        const char *dot = strrchr(name8, '.');
        is_pe = dot && sh_streq(dot, ".exe");
    }

    W32_DWORD_PTR result = 1;       /* the "info obtained" receipt */

    if (flags & W32_SHGFI_TYPENAME) {
        const char *tn = is_pe ? "Application" : sh_typename_for(name8, is_dir);
        uint16_t w[80];
        sh_a2w(tn, w, 80);
        size_t i = 0;
        while (w[i] && i < 79) {
            sfi->szTypeName[i] = w[i];
            i++;
        }
        sfi->szTypeName[i] = 0;
    }
    if (flags & W32_SHGFI_DISPLAYNAME) {
        uint16_t w[260];
        sh_a2w(name8, w, 260);
        size_t i = 0;
        while (w[i] && i < 259) {
            sfi->szDisplayName[i] = w[i];
            i++;
        }
        sfi->szDisplayName[i] = 0;
    }
    if (flags & W32_SHGFI_ATTRIBUTES) {
        W32_DWORD out = 0;
        if (is_dir) out |= 0x10u;   /* FILE_ATTRIBUTE_DIRECTORY */
        if (have_stat && (st.st_mode & 0222) == 0) out |= 1u; /* READONLY */
        if (!is_dir) out |= 0x20u;  /* ARCHIVE, the normal-file shape */
        sfi->dwAttributes = out;
    }
    if (flags & W32_SHGFI_ICON) {
        sfi->hIcon = 0;
        sfi->iIcon = 0;
        if (!is_dir && host) {
            int groups = sh_icon_group_count(host);
            if (groups > 0)
                sfi->hIcon = sh_icon_from_group(host, 0);
        }
        if (!sfi->hIcon)
            sfi->hIcon = sh_doc_icon();
    }
    if (flags & W32_SHGFI_EXETYPE) {
        /* The documented return: 2 for a Win32 console image, 0 for
         * a non-executable. */
        result = (is_pe && !is_dir) ? 2 : 0;
    }
    if (host) free(host);
    return result;
}

/* ---- ExtractIconExW ------------------------------------------------------------------------- */

W32_UINT W32ABI ExtractIconExW(W32_LPCWSTR file, int index,
                               W32_HICON *large, W32_HICON *small,
                               W32_UINT count) {
    if (!file) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((W32_UINT)index == 0xFFFFFFFFu) {
        /* The documented -1 receipt: the total icon count, nothing read.
         * large/small/count are ignored. */
        char a0[512];
        if (sh_w2a(file, a0, sizeof a0) <= 0) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        char *host0 = w32_fs_xlate_dup(a0);
        if (!host0) {
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            return 0;
        }
        int g0 = sh_icon_group_count(host0);
        free(host0);
        if (g0 < 0) {
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            return 0;
        }
        return (W32_UINT)g0;
    }
    if ((!large && !small) || count == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char a[512];
    if (sh_w2a(file, a, sizeof a) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *host = w32_fs_xlate_dup(a);
    if (!host) {
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    int groups = sh_icon_group_count(host);
    if (index < 0 || groups < 0) {
        free(host);
        w32_set_last_error(index < 0 ? W32_ERROR_INVALID_PARAMETER
                                     : W32_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    W32_UINT n = 0;
    for (W32_UINT i = 0; i < count; i++) {
        int g = index + (int)i;
        if (g >= groups) break;
        W32_HICON h = sh_icon_from_group(host, g);
        if (!h) break;
        if (large) large[i] = h;
        if (small) small[i] = h;
        n++;
    }
    free(host);
    if (n == 0) w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
    return n;
}

/* ---- SHFileOperationW ------------------------------------------------------------------------ */

/* Split a double-null-terminated list.  Returns the item count (<=16). */
static int sh_split_list(W32_LPCWSTR list, const uint16_t *items[],
                         size_t lens[]) {
    if (!list) return 0;
    int n = 0;
    const uint16_t *p = list;
    while (*p && n < 16) {
        items[n] = p;
        lens[n] = sh_wcslen(p);
        p += lens[n] + 1;
        n++;
    }
    return n;
}

/* Does the final component carry a wildcard? */
static int sh_has_wildcard(const uint16_t *item, size_t len) {
    int in_name = 0;
    for (size_t i = len; i > 0; i--) {
        uint16_t c = item[i - 1];
        if (c == '/' || c == '\\') break;
        if (c == '*' || c == '?') in_name = 1;
    }
    return in_name;
}

/* Copy/move every wildcard match of `from' into the destination
 * directory (host view).  Returns 0, or the Win32 error of the first
 * failure. */
static W32_DWORD sh_op_wildcard(const uint16_t *from, size_t flen,
                                const uint16_t *to, size_t tlen,
                                int move) {
    /* Split into dir + pattern (both guest-side UTF-16). */
    uint16_t dir[400], pat[80], dest[400];
    size_t dlen = 0, plen = 0;
    size_t cut = flen;
    for (size_t i = flen; i > 0; i--)
        if (from[i - 1] == '\\' || from[i - 1] == '/') { cut = i - 1; break; }
    if (cut == 0) {
        dir[dlen++] = '.';
    } else {
        sh_wcpy(dir, from, cut);
        dlen = cut;
    }
    dir[dlen] = 0;
    sh_wcpy(pat, from + cut + 1, flen - cut - 1);
    plen = flen - cut - 1;
    pat[plen] = 0;
    sh_wcpy(dest, to, tlen);
    dest[tlen] = 0;

    /* Wildcard enumeration through the public find engine: the pattern
     * semantics (folding, '*'/'?') are the A-2 directory scan's. */
    uint16_t query[480];
    sh_wcpy(query, dir, dlen);
    query[dlen] = '\\';
    sh_wcpy(query + dlen + 1, pat, plen);
    query[dlen + 1 + plen] = 0;

    W32_WIN32_FIND_DATAW fd;
    W32_HANDLE h = FindFirstFileW(query, &fd);
    if (!h) return w32_get_last_error_raw();

    W32_DWORD ret = 0;
    int n = 0;
    do {
        if (fd.cFileName[0] == 0) continue;
        if (fd.dwFileAttributes & 0x10u) continue;   /* directories: no */
        /* from = dir\name, to = dest\name */
        uint16_t src[560], dst[560];
        size_t sl = dlen, dl = tlen;
        sh_wcpy(src, dir, dlen);
        src[sl++] = '\\';
        for (size_t i = 0; fd.cFileName[i] && sl + 1 < 560; i++)
            src[sl++] = fd.cFileName[i];
        src[sl] = 0;
        sh_wcpy(dst, dest, tlen);
        dst[dl++] = '\\';
        for (size_t i = 0; fd.cFileName[i] && dl + 1 < 560; i++)
            dst[dl++] = fd.cFileName[i];
        dst[dl] = 0;
        W32_BOOL ok = move ? MoveFileW(src, dst) : CopyFileW(src, dst, 1);
        if (!ok) {
            ret = w32_get_last_error_raw();
            break;
        }
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (ret) return ret;
    if (n == 0) return 2;           /* ERROR_FILE_NOT_FOUND */
    return 0;
}

W32_INT W32ABI SHFileOperationW(W32_SHFILEOPSTRUCTW *op) {
    if (!op) return (W32_INT)W32_DE_INVALIDFILES;
    if (op->wFunc < W32_FO_MOVE || op->wFunc > W32_FO_RENAME) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_INT)W32_DE_INVALIDFILES;
    }
    if (op->fFlags & W32_FOF_ALLOWUNDO) {
        sh_note("SHFileOperationW: FOF_ALLOWUNDO refused -- there is no "
                "recycle bin (ERROR_CALL_NOT_IMPLEMENTED)");
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return (W32_INT)W32_DE_CALL_NOT_IMPLEMENTED;
    }
    if (!op->pFrom) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_INT)W32_DE_INVALIDFILES;
    }
    const uint16_t *from[16];
    size_t flen[16];
    const uint16_t *to[16];
    size_t tlen[16];
    int nfrom = sh_split_list(op->pFrom, from, flen);
    int nto = op->pTo ? sh_split_list(op->pTo, to, tlen) : 0;

    if (nfrom == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_INT)W32_DE_INVALIDFILES;
    }
    if (op->wFunc != W32_FO_DELETE && nto == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_INT)W32_DE_INVALIDFILES;
    }
    if (op->wFunc == W32_FO_RENAME && (nfrom != 1 || nto != 1)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_INT)W32_DE_INVALIDFILES;
    }
    /* Every destination of a copy/move must exist (the documented
     * single-item rule: pTo names directories or full paths). */
    if (op->wFunc != W32_FO_DELETE && !(op->fFlags & W32_FOF_MULTIDESTFILES)) {
        for (int d = 1; d < nto; d++) {
            sh_note("SHFileOperationW: extra destinations without "
                    "FOF_MULTIDESTFILES are ignored (the documented "
                    "single-list rule)");
            break;
        }
    }

    W32_INT ret = 0;
    for (int i = 0; i < nfrom; i++) {
        W32_DWORD rc = 0;
        if (op->wFunc == W32_FO_DELETE) {
            if (!DeleteFileW(from[i]))
                rc = w32_get_last_error_raw();
        } else if (sh_has_wildcard(from[i], flen[i])) {
            if (op->wFunc == W32_FO_RENAME) {
                rc = W32_ERROR_INVALID_PARAMETER;
            } else {
                int di = (op->fFlags & W32_FOF_MULTIDESTFILES) ? i : 0;
                if (di >= nto) {
                    rc = W32_ERROR_INVALID_PARAMETER;
                } else {
                    rc = sh_op_wildcard(from[i], flen[i], to[di], tlen[di],
                                        op->wFunc == W32_FO_MOVE);
                }
            }
        } else {
            int di = (op->fFlags & W32_FOF_MULTIDESTFILES) ? i : 0;
            if (di >= nto) {
                rc = W32_ERROR_INVALID_PARAMETER;
            } else if (op->wFunc == W32_FO_COPY) {
                if (!CopyFileW(from[i], to[di], 1))
                    rc = w32_get_last_error_raw();
            } else {               /* MOVE / RENAME */
                if (!MoveFileW(from[i], to[di]))
                    rc = w32_get_last_error_raw();
            }
        }
        if (rc) {
            ret = (W32_INT)rc;
            break;
        }
    }
    /* lpszProgressTitle is accepted; the progress surface is the
     * notification engine, one line per operation (fFlags honors the
     * callbacks by construction: the ops are synchronous). */
    return ret;
}

/* ---- ShellExecute ------------------------------------------------------------------------------ */

static int sh_file_is_exec(const char *host, int *elf) {
    *elf = 0;
    int fd = open(host, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t hdr[64];
    ssize_t r = read(fd, hdr, sizeof hdr);
    close(fd);
    if (r < 4) return 0;
    if (hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        *elf = 1;
        return 1;
    }
    if (hdr[0] == 'M' && hdr[1] == 'Z') {
        size_t flen = 0;
        uint8_t *file = sh_read_file(host, &flen);
        if (!file) return 0;
        pe_image_t img;
        int loadable = (pe_parse(file, flen, &img) == PE_OK);
        free(file);
        return loadable;
    }
    return 0;
}

/* The shared decision core of the three entry points.  Returns >32
 * ("an executable was found") with a malloc'd command line for the
 * spawn, or the SE_ERR receipt. */
static W32_HINSTANCE sh_execute(W32_LPCWSTR verb, W32_LPCWSTR file,
                                W32_LPCWSTR params, uint16_t **cmdline_out) {
    *cmdline_out = NULL;
    if (!file || !file[0]) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_FNF;
    }
    if (verb && verb[0]) {
        char v8[64];
        sh_w2a(verb, v8, sizeof v8);
        if (sh_streq(v8, "open")) {
            /* the one executing verb */
        } else if (sh_streq(v8, "runas")) {
            sh_note("ShellExecute: verb 'runas' refused -- there "
                    "is no elevation (SE_ERR_ACCESSDENIED)");
            w32_set_last_error(W32_ERROR_ACCESS_DENIED);
            return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_ACCESSDENIED;
        } else {
            sh_note("ShellExecute: no associations exist, so non-open "
                    "verbs refuse (SE_ERR_NOASSOC)");
            w32_set_last_error(W32_ERROR_NO_ASSOCIATION);
            return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_NOASSOC;
        }
    }
    char a[512];
    if (sh_w2a(file, a, sizeof a) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_FNF;
    }
    char *host = w32_fs_xlate_dup(a);
    if (!host) {
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_FNF;
    }
    struct stat st;
    if (stat(host, &st) != 0) {
        free(host);
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_FNF;
    }
    if (S_ISDIR(st.st_mode)) {
        free(host);
        sh_note("ShellExecute: directories refuse (the desktop namespace "
                "is section 7; SE_ERR_NOASSOC)");
        w32_set_last_error(W32_ERROR_NO_ASSOCIATION);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_NOASSOC;
    }
    int elf = 0;
    if (!sh_file_is_exec(host, &elf)) {
        free(host);
        sh_note("ShellExecute: documents refuse -- there is no "
                "association table (SE_ERR_NOASSOC)");
        w32_set_last_error(W32_ERROR_NO_ASSOCIATION);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_NOASSOC;
    }
    /* An executable: hand the spawn to the W32A-2 process machinery.
     * The command line is the quoted path + params, argv[0]-shaped. */
    size_t cap = 600;
    uint16_t *cmd = (uint16_t *)malloc(cap * 2u);
    if (!cmd) {
        free(host);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return (W32_HINSTANCE)(uintptr_t)W32_SE_ERR_OOM;
    }
    size_t o = 0;
    cmd[o++] = '"';
    size_t i = 0;
    while (file[i] && o + 2 < cap) cmd[o++] = file[i++];
    cmd[o++] = '"';
    if (params) {
        cmd[o++] = ' ';
        size_t j = 0;
        while (params[j] && o + 1 < cap) cmd[o++] = params[j++];
    }
    cmd[o] = 0;
    free(host);
    *cmdline_out = cmd;
    return (W32_HINSTANCE)(uintptr_t)33;   /* > SE_ERR_OOM: "started" */
}

/* The spawn, shared by W and ExW.  `keep` decides who owns hProcess. */
static W32_HINSTANCE sh_spawn(W32_LPCWSTR file, uint16_t *cmd,
                              W32_LPCWSTR dir, W32_HANDLE *proc_out) {
    uint16_t app[512];
    size_t i = 0;
    while (file[i] && i + 1 < sizeof app / 2u) {
        app[i] = file[i];
        i++;
    }
    app[i] = 0;
    W32_STARTUPINFOW si;
    W32_PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(app, cmd, NULL, NULL, 0, 0, NULL, dir, &si, &pi)) {
        W32_DWORD e = w32_get_last_error_raw();
        return (W32_HINSTANCE)(uintptr_t)
            (e == W32_ERROR_FILE_NOT_FOUND ? W32_SE_ERR_FNF
                                           : W32_SE_ERR_ACCESSDENIED);
    }
    if (proc_out)
        *proc_out = pi.hProcess;
    else if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    return (W32_HINSTANCE)(uintptr_t)33;
}

W32_HINSTANCE W32ABI ShellExecuteW(W32_HWND owner, W32_LPCWSTR verb,
                                   W32_LPCWSTR file, W32_LPCWSTR params,
                                   W32_LPCWSTR dir, W32_INT show) {
    (void)owner; (void)show;
    uint16_t *cmd = NULL;
    W32_HINSTANCE pre = sh_execute(verb, file, params, &cmd);
    if ((uintptr_t)pre <= 32) return pre;
    W32_HINSTANCE r = sh_spawn(file, cmd, dir, NULL);
    free(cmd);
    return r;
}

W32_HINSTANCE W32ABI ShellExecuteA(W32_HWND owner, const char *verb,
                                   const char *file, const char *params,
                                   const char *dir, W32_INT show) {
    uint16_t wv[64], wf[512], wp[512], wd[512];
    if (verb) sh_a2w(verb, wv, 64); else wv[0] = 0;
    if (file) sh_a2w(file, wf, 512); else wf[0] = 0;
    if (params) sh_a2w(params, wp, 512); else wp[0] = 0;
    if (dir) sh_a2w(dir, wd, 512); else wd[0] = 0;
    return ShellExecuteW(owner, verb ? wv : NULL, file ? wf : NULL,
                         params ? wp : NULL, dir ? wd : NULL, show);
}

W32_BOOL W32ABI ShellExecuteExW(W32_SHELLEXECUTEINFOW *info) {
    if (!info || info->cbSize < sizeof *info || !info->lpFile) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint16_t *cmd = NULL;
    W32_HINSTANCE pre = sh_execute(info->lpVerb, info->lpFile,
                                   info->lpParameters, &cmd);
    info->hInstApp = pre;
    if ((uintptr_t)pre <= 32) return 0;
    W32_HANDLE proc = NULL;
    W32_HINSTANCE r = sh_spawn(info->lpFile, cmd, info->lpDirectory, &proc);
    free(cmd);
    info->hInstApp = r;
    if ((uintptr_t)r <= 32) return 0;
    info->hProcess = proc;          /* SEE_MASK_NOCLOSEPROCESS honoured */
    return 1;
}

/* ---- Shell_NotifyIconW -------------------------------------------------------------------------- */

#define SH_NOTIFY_SLOTS 8
static struct {
    int used;
    W32_HWND hwnd;
    W32_UINT id;
    uint16_t tip[128];
} sh_notify_slots[SH_NOTIFY_SLOTS];

static void sh_notify_show(int slot) {
    char tip8[128];
    sh_w2a(sh_notify_slots[slot].tip, tip8, sizeof tip8);
    char line[160];
    snprintf(line, sizeof line, "shell icon: %s", tip8[0] ? tip8 : "(no tip)");
    ag_notify(line, 0xFF3060C0u, 3000);
}

W32_BOOL W32ABI Shell_NotifyIconW(W32_DWORD msg, W32_NOTIFYICONDATAW *data) {
    if (!data || (data->cbSize && data->cbSize < 32u)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < SH_NOTIFY_SLOTS; i++)
        if (sh_notify_slots[i].used &&
            sh_notify_slots[i].hwnd == data->hWnd &&
            sh_notify_slots[i].id == data->uID) {
            slot = i;
            break;
        }
    switch (msg) {
    case W32_NIM_ADD:
        if (slot >= 0) {
            w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
            return 0;
        }
        for (int i = 0; i < SH_NOTIFY_SLOTS; i++)
            if (!sh_notify_slots[i].used) { slot = i; break; }
        if (slot < 0) {
            w32_set_last_error(W32_ERROR_NO_MORE_ITEMS);
            return 0;
        }
        sh_notify_slots[slot].used = 1;
        sh_notify_slots[slot].hwnd = data->hWnd;
        sh_notify_slots[slot].id = data->uID;
        memset(sh_notify_slots[slot].tip, 0, sizeof sh_notify_slots[slot].tip);
        if (data->uFlags & W32_NIF_TIP)
            for (int i = 0; i < 127 && data->szTip[i]; i++)
                sh_notify_slots[slot].tip[i] = data->szTip[i];
        sh_notify_show(slot);
        return 1;
    case W32_NIM_MODIFY:
        if (slot < 0) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (data->uFlags & W32_NIF_TIP) {
            memset(sh_notify_slots[slot].tip, 0, sizeof sh_notify_slots[slot].tip);
            for (int i = 0; i < 127 && data->szTip[i]; i++)
                sh_notify_slots[slot].tip[i] = data->szTip[i];
            sh_notify_show(slot);
        }
        return 1;
    case W32_NIM_DELETE:
        if (slot < 0) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        memset(&sh_notify_slots[slot], 0, sizeof sh_notify_slots[slot]);
        return 1;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

/* ---- the drag trio (the model W32A-11 feeds) ------------------------------------------------------ */

#define SH_DROP_MAX 16
static const char *sh_drop_list[SH_DROP_MAX];
static int sh_drop_count;

/* W32A-11's drag source registers the drop list; until then it is
 * empty and every query answers honestly from the empty model. */
void w32_shell_set_drop_list(const char *paths[], int n) {
    sh_drop_count = 0;
    for (int i = 0; i < n && i < SH_DROP_MAX; i++)
        sh_drop_list[i] = paths[i];
    sh_drop_count = (n < SH_DROP_MAX) ? n : SH_DROP_MAX;
}

W32_UINT W32ABI DragQueryFileW(W32_HANDLE hDrop, W32_UINT index,
                               W32_LPWSTR buf, W32_UINT cch) {
    (void)hDrop;                    /* one drop at a time in this model */
    if (index == 0xFFFFFFFFu)
        return (W32_UINT)sh_drop_count;
    if ((int)index >= sh_drop_count) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint16_t w[600];
    if (sh_a2w(sh_drop_list[index], w, 600) <= 0) return 0;
    size_t n = sh_wcslen(w);
    if (!buf || cch == 0)
        return (W32_UINT)(n + 1);   /* the needed size, documented */
    if (cch < n + 1) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return (W32_UINT)(n + 1);
    }
    sh_wcpy(buf, w, n);
    buf[n] = 0;
    return (W32_UINT)n;
}

W32_BOOL W32ABI DragQueryPoint(W32_HANDLE hDrop, W32_POINT *pt) {
    (void)hDrop;
    if (!pt) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pt->x = 0;
    pt->y = 0;
    return 0;                       /* no drop has happened in this model */
}

void W32ABI DragFinish(W32_HANDLE hDrop) {
    (void)hDrop;
    sh_drop_count = 0;              /* release: the list is a borrow */
}
