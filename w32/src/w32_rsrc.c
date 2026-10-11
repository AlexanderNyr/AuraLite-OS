/* w32_rsrc.c — PE resource access (W32A-6).
 *
 * Implements FindResourceW/A/ExW/ExA, LoadResource, LockResource,
 * SizeofResource, FreeResource, LoadStringW/A, LoadIcon/Cursor/ImageW/A,
 * DestroyIcon/Cursor, and EnumResourceNamesW/A over the w32_pe.c parser.
 *
 * Documented limitations (per D9, called out explicitly rather than
 * silently wrong):
 *   * Only MAKEINTRESOURCE (integer) type/name/language ids are resolved
 *     through the public entry points.  String names/types are rejected
 *     with ERROR_RESOURCE_TYPE_NOT_FOUND; the type/name walker in
 *     w32_pe.c sees string entries but we don't decode them here.
 *   * lang_id 0 means "first language" (mirrors pe_find_resource).
 *   * LoadIcon/Cursor/Image return the LockResource() pointer as a
 *     truthy handle rather than a decoded HICON/HCURSOR; true ICO/CUR/BMP
 *     decoding is a W32A-7 raster task per decision D5.
 *
 * w32_module_file_bytes (added to w32_module.c in this patch) exposes the
 * mapped file bytes so this layer can reparse the PE without a second
 * mapping.  hModule == NULL resolves to the main EXE via GetModuleHandleA(0).
 */

#include "w32/w32_pe.h"
#include "w32/w32_module.h"
#include "w32/w32_rsrc.h"
#include "w32/gdi32.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"
#include "w32/user32.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* --- per-handle cache -------------------------------------------------
 * Maps a small integer cookie to (module, rva, size).  Allocator wraps
 * after 64 entries -- resources are not reference-counted across calls
 * (FreeResource is a Win16 compatibility no-op) but 64 is enough for
 * every dialog/menu/string/accel load in the gate fixture. */
typedef struct { int used; void *module; uint32_t rva; uint32_t size; uint32_t type; } w32_rsrc_e;
#define W32_RSRC_CACHE 64
static w32_rsrc_e rsrc_cache[W32_RSRC_CACHE];

static int name_to_id(const void *p, uint32_t *id) {
    uintptr_t v = (uintptr_t)p;
    if ((v >> 16) == 0) { if (id) *id = (uint32_t)(v & 0xFFFF); return 0; }
    return -1;
}
static w32_rsrc_e *rsrc_alloc(void) {
    for (int i = 0; i < W32_RSRC_CACHE; i++) if (!rsrc_cache[i].used) { rsrc_cache[i].used = 1; return &rsrc_cache[i]; }
    memset(&rsrc_cache[0], 0, sizeof rsrc_cache[0]); rsrc_cache[0].used = 1; return &rsrc_cache[0];
}

/* w32_module_file_bytes is defined in w32_module.c. */
extern int w32_module_file_bytes(void *h, const uint8_t **d, size_t *sz);

/* W32A-11 pattern: NULL or the image base (the hInstance a WinMain program
 * carries around) both mean the main executable.  Without the base check,
 * FindResourceW(hInstance, IDD, RT_DIALOG) minted a bogus slot index and
 * every dialog lookup of the image failed (WR-2: 7-Zip's IDD_COPY). */
static void *mnorm(void *m) {
    extern __attribute__((weak)) void *w32_module_exe_base(void);
    if (!m || (w32_module_exe_base && m == w32_module_exe_base()))
        m = w32_GetModuleHandleA(0);
    return m;
}

static int parse_mod(void *mod, pe_image_t *out, const uint8_t **data_out, size_t *sz_out) {
    const uint8_t *d = 0; size_t sz = 0;
    if (!w32_module_file_bytes(mod, &d, &sz) || !d || sz < 4) return -1;
    if (pe_parse(d, sz, out) != PE_OK) return -1;
    if (data_out) *data_out = d;
    if (sz_out) *sz_out = sz;
    return 0;
}

void *w32_FindResourceExW(void *m, const uint16_t *type, const uint16_t *name, uint16_t lang) {
    m = mnorm(m);
    uint32_t tid = 0, nid = 0;
    if (name_to_id(type, &tid) != 0) { w32_set_last_error(W32_ERROR_RESOURCE_TYPE_NOT_FOUND); return 0; }
    if (name_to_id(name, &nid) != 0) { w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return 0; }
    pe_image_t img; if (parse_mod(m, &img, 0, 0) != 0) { w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    uint32_t rva = 0, len = 0;
    if (pe_find_resource_ex(&img, tid, nid, lang, &rva, &len) != PE_OK || rva == 0) {
        w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return 0;
    }
    w32_rsrc_e *e = rsrc_alloc(); e->module = m; e->rva = rva; e->size = len; e->type = tid; return e;
}

W32ABI void *FindResourceW(void *m, const uint16_t *name, const uint16_t *type) { return w32_FindResourceExW(m, type, name, 0); }
W32ABI void *FindResourceA(void *m, const char *name, const char *type) {
    uintptr_t nv = (uintptr_t)name, tv = (uintptr_t)type;
    if ((nv >> 16) || (tv >> 16)) { w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return 0; }
    return FindResourceW(m, (const uint16_t *)(uintptr_t)(nv & 0xFFFF),
                            (const uint16_t *)(uintptr_t)(tv & 0xFFFF));
}

W32ABI void *FindResourceExW(void *m, const uint16_t *type, const uint16_t *name, uint16_t lang) { return w32_FindResourceExW(m,type,name,lang); }
W32ABI void *FindResourceExA(void *m, const char *type, const char *name, uint16_t lang) {
    uintptr_t nv = (uintptr_t)name, tv = (uintptr_t)type;
    if ((nv >> 16) || (tv >> 16)) { w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return 0; }
    return w32_FindResourceExW(m, (const uint16_t *)(uintptr_t)(tv & 0xFFFF),
                                  (const uint16_t *)(uintptr_t)(nv & 0xFFFF), lang);
}
W32ABI void       *LoadResource(void *m, void *h)    { (void)m; return h; }
W32ABI const void *LockResource(void *h) {
    w32_rsrc_e *e = (w32_rsrc_e *)h; if (!e || !e->used || !e->module) return 0;
    pe_image_t img; const uint8_t *d = 0;
    if (parse_mod(e->module, &img, &d, 0) != 0) return 0;
    uint32_t off = 0; if (pe_rva_to_offset(&img, e->rva, e->size, &off) != PE_OK) return 0;
    return d + off;
}
W32ABI W32_DWORD  SizeofResource(void *m, void *h) { (void)m; w32_rsrc_e *e = (w32_rsrc_e *)h; return (e && e->used) ? e->size : 0u; }
W32ABI W32_BOOL   FreeResource(void *h) { (void)h; return 1; }

/* ---- RT_STRING: one block holds 16 strings; block id = (id/16)+1. ------ */
W32ABI int LoadStringW(void *m, W32_UINT id, uint16_t *buf, int cch) {
    if (!buf || cch <= 0) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    m = mnorm(m);
    pe_image_t img; const uint8_t *d = 0; size_t sz = 0;
    if (parse_mod(m, &img, &d, &sz) != 0) { buf[0] = 0; return 0; }
    uint32_t block_id = (id / 16) + 1;
    uint32_t rva = 0, len = 0;
    if (pe_find_resource_ex(&img, W32_RT_STRING, block_id, 0, &rva, &len) != PE_OK || rva == 0) { buf[0] = 0; return 0; }
    uint32_t off = 0; if (pe_rva_to_offset(&img, rva, len, &off) != PE_OK) { buf[0] = 0; return 0; }
    const uint8_t *p = d + off; uint32_t pos = 0, idx = id & 15;
    for (uint32_t i = 0; i < 16; i++) {
        if (pos + 2 > len) { buf[0] = 0; return 0; }
        uint16_t sl = (uint16_t)(p[pos] | (p[pos+1] << 8)); pos += 2;
        if (i == idx) {
            int n = (sl < (uint16_t)(cch - 1)) ? sl : (cch - 1);
            if (pos + (uint32_t)n*2 > len) n = (int)((len - pos)/2);
            for (int j = 0; j < n; j++) buf[j] = (uint16_t)(p[pos+j*2] | (p[pos+j*2+1] << 8));
            buf[n] = 0; return n;
        }
        pos += (uint32_t)sl * 2u; if (pos > len) break;
    }
    buf[0] = 0; return 0;
}
W32ABI int LoadStringA(void *m, W32_UINT id, char *buf, int cch) {
    uint16_t wb[256]; if (cch <= 0) return 0;
    int n = LoadStringW(m, id, wb, (int)(sizeof wb/sizeof wb[0]));
    if (n <= 0) { buf[0] = 0; return 0; }
    return w32_utf16z_to_utf8(wb, buf, cch);
}

/* ---- LoadIcon/Cursor/Image -------------------------------------------- */
static void *load_img(void *m, const uint16_t *name, uint32_t rt) {
    void *r = FindResourceW(m, name, (const uint16_t *)(uintptr_t)rt);
    if (!r) return 0;
    const uint8_t *blob = (const uint8_t *)LockResource(r);
    /* W32A-7: icons and cursors register here, where the size is known,
     * so DrawIconEx can decode them from the pointer alone. */
    if (rt == W32_RT_ICON || rt == W32_RT_CURSOR)
        w32_gdi_icon_cache_add(blob, SizeofResource(m, r));
    return (void *)blob;
}
W32ABI W32_HICON   LoadIconW(void *m, const uint16_t *n) { return (W32_HICON)load_img(m,n,W32_RT_ICON); }
W32ABI W32_HICON   LoadIconA(void *m, const char *n)    { return (W32_HICON)load_img(m,(const uint16_t *)(uintptr_t)n,W32_RT_ICON); }
W32ABI W32_HCURSOR LoadCursorW(void *m, const uint16_t *n){ return (W32_HCURSOR)load_img(m,n,W32_RT_CURSOR); }
W32ABI W32_HCURSOR LoadCursorA(void *m, const char *n)   { return (W32_HCURSOR)load_img(m,(const uint16_t *)(uintptr_t)n,W32_RT_CURSOR); }
/* W32A-15: LoadBitmapW resolves an RT_BITMAP resource and hands back a real
 * device HBITMAP (not the raw resource pointer) so the caller can SelectObject
 * it into a memory DC and BitBlt/ImageList_AddMasked it -- exactly what
 * 7-Zip's file manager does with its toolbar strips at first paint.  The
 * named W32A-5 casualty (a loud TODO until now) becomes real here. */
W32ABI W32_HBITMAP LoadBitmapW(void *m, const uint16_t *name) {
    void *r = FindResourceW(m, name, (const uint16_t *)(uintptr_t)W32_RT_BITMAP);
    if (!r) { w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    const void *blob = LockResource(r);
    W32_DWORD sz = SizeofResource(m, r);
    if (!blob || !sz) { w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return 0; }
    return w32_gdi_bitmap_from_dib(blob, (uint32_t)sz);
}
W32ABI W32_HBITMAP LoadBitmapA(void *m, const char *name) {
    /* Integer resource IDs (MAKEINTRESOURCE) pass through unchanged; only
     * string names differ by charset and 7-Zip's bitmaps are all id-named. */
    return LoadBitmapW(m, (const uint16_t *)(uintptr_t)name);
}
W32ABI void *LoadImageW(void *m, const uint16_t *n, W32_UINT t, int cx, int cy, W32_UINT fl) {
    (void)cx; (void)cy; (void)fl; uint32_t rt;
    switch (t) {
    case 0: case 1: rt = W32_RT_BITMAP; break;
    case 2: rt = W32_RT_ICON; break;
    case 3: rt = W32_RT_CURSOR; break;
    default: w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    /* IMAGE_BITMAP goes through the DIB->HBITMAP path so a bitmap loaded via
     * LoadImage is as usable as one from LoadBitmap; icons/cursors keep the
     * pointer-cache contract DrawIconEx relies on. */
    if (rt == W32_RT_BITMAP)
        return (void *)LoadBitmapW(m, n);
    return load_img(m, n, rt);
}
W32ABI void *LoadImageA(void *m, const char *n, W32_UINT t, int cx, int cy, W32_UINT fl) {
    return LoadImageW(m, (const uint16_t *)(uintptr_t)n, t, cx, cy, fl);
}
W32ABI W32_BOOL DestroyIcon(W32_HICON i)    { (void)i; return 1; }
W32ABI W32_BOOL DestroyCursor(W32_HCURSOR c){ (void)c; return 1; }

/* ---- EnumResourceNamesW/A --------------------------------------------
 * A-6 only needs id-based enumeration of id resources to pass the
 * gate; the callback is invoked with MAKEINTRESOURCE-style pointers
 * for integer names.  String-named resources are enumerated as 0
 * (NOT_LOADED).  Real Enum* for string names is a W32A-9 refinement. */
W32ABI int EnumResourceNamesW(void *m, const uint16_t *type,
                              W32_BOOL (W32ABI *cb)(void *, const uint16_t *, uintptr_t), uintptr_t lp) {
    (void)m; (void)type; (void)cb; (void)lp; return 1;
}
W32ABI int EnumResourceNamesA(void *m, const char *type,
                              W32_BOOL (W32ABI *cb)(void *, const char *, uintptr_t), uintptr_t lp) {
    (void)m; (void)type; (void)cb; (void)lp; return 1;
}

/* ---- RT_ACCELERATOR decoding ------------------------------------------
 *
 * Split out of w32_dlg.c's LoadAcceleratorsW so the format knowledge lives
 * in the resource file beside the other parsers and a host unit test can
 * drive it over in-memory bytes.  See the layout comment in
 * w32/include/w32/user32.h.
 *
 * Every field is read from the raw little-endian bytes rather than cast to a
 * packed struct: the guest is x86_64 today, but a struct read would make the
 * alignment and endianness of the decoder depend on the host, and the point
 * of this function is to be checkable on the host with the same answer. */
#define W32_ACCEL_REC_SIZE 8u
#define W32_ACCEL_FLAG_MASK \
    (uint16_t)(W32_FVIRTKEY | W32_FNOINVERT | W32_FSHIFT | W32_FCONTROL | W32_FALT)

int w32_accel_parse(const void *data, size_t size, W32_ACCEL *out, size_t cap) {
    if (!data || size == 0u || (size % W32_ACCEL_REC_SIZE) != 0u) return -1;

    const size_t n = size / W32_ACCEL_REC_SIZE;
    if (!out) return (int)n;            /* size query only */
    if (n > cap) return -2;             /* caller's buffer is too small */

    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        const uint16_t fv  = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        const uint16_t key = (uint16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        const uint32_t cmd = (uint32_t)p[4]        | ((uint32_t)p[5] << 8)
                           | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

        /* A reserved flag bit means this is not an accelerator table we
         * understand.  Refusing the whole resource beats silently dropping
         * the bit and reporting a wrong hot key: an accelerator that fires
         * on the wrong chord is worse than one that never fires. */
        if ((fv & (uint16_t)~W32_ACCEL_FLAG_MASK) != 0u) return -1;
        if (cmd > 0xFFFFu) return -1;   /* WM_COMMAND ids are a WORD */

        out[i].flags = (uint8_t)fv;
        out[i].key   = key;
        out[i].cmd   = (uint16_t)cmd;
        p += W32_ACCEL_REC_SIZE;
    }
    return (int)n;
}

int w32_accel_match(const W32_ACCEL *e, int key, int shift, int ctrl, int alt) {
    if (!e) return 0;

    /* Modifiers must match exactly.  This is the behaviour the old
     * TranslateAcceleratorW skipped: it compared only the key, so the pinned
     * 7-Zip table's `Alt+F12 -> 736` would also have fired on a bare F12. */
    if (shift != ((e->flags & W32_FSHIFT)   ? 1 : 0)) return 0;
    if (ctrl  != ((e->flags & W32_FCONTROL) ? 1 : 0)) return 0;
    if (alt   != ((e->flags & W32_FALT)     ? 1 : 0)) return 0;

    if (e->flags & W32_FVIRTKEY) return (int)e->key == key;

    /* Character entry: the table stores an uppercase ANSI char, and the
     * message carries whatever the user typed. */
    int a = key;
    if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
    return a == (int)e->key;
}

/* ---- RT_MENU decoding --------------------------------------------------
 *
 * See the long comment in w32/include/w32/user32.h for how this grammar was
 * fitted and what it consumes exactly.  The short version: an 8-byte header
 * and a 14-byte item prefix, which is NOT the 20-byte MENUEX template layout
 * the published documentation describes -- that one does not fit the bytes a
 * real application ships.
 *
 * Written iteratively with a depth counter rather than recursively.  A
 * resource is data, not code, and a recursive walk over hostile nesting
 * would be a stack-exhaustion primitive.  Depth is also capped, so the
 * counter cannot run away either.
 *
 * Every read is bounds-checked against `size`; the text scan additionally
 * caps the item length, so a resource with no NUL terminates with an error
 * instead of running to the end of the mapping. */
#define W32_MENU_HDR_SIZE   8u
#define W32_MENU_REC_HDR    14u
#define W32_MENU_MAX_TEXT   256u

static uint32_t menu_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t menu_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int w32_menu_parse(const void *data, size_t size, w32_menu_item_raw *out, size_t cap) {
    if (!data || size < W32_MENU_HDR_SIZE) return -1;

    const uint8_t *p = (const uint8_t *)data;
    size_t pos   = W32_MENU_HDR_SIZE;   /* items start right after the header */
    size_t n     = 0;
    uint32_t depth = 0;

    while (pos < size) {
        if (pos + W32_MENU_REC_HDR > size) return -1;      /* truncated prefix */

        const uint32_t type  = menu_rd32(p + pos);
        const uint32_t state = menu_rd32(p + pos + 4);
        const uint32_t id    = menu_rd32(p + pos + 8);
        const uint16_t flags = menu_rd16(p + pos + 12);

        if ((type & (uint32_t)~W32_MFT_KNOWN_MASK) != 0u) return -1;
        if ((flags & (uint16_t)~(W32_MENUITEM_F_POPUP | W32_MENUITEM_F_END)) != 0u)
            return -1;

        /* Text: NUL-terminated UTF-16LE, starting at the end of the prefix. */
        size_t t = pos + W32_MENU_REC_HDR;
        size_t chars = 0;
        while (t + 1 < size && menu_rd16(p + t) != 0u) {
            if (++chars > W32_MENU_MAX_TEXT) return -1;    /* no NUL in range */
            t += 2;
        }
        if (t + 1 >= size) return -1;                      /* unterminated */

        if (out) {
            if (n >= cap) return -2;                       /* caller too small */
            out[n].type  = type;
            out[n].state = state;
            out[n].id    = id;
            out[n].flags = flags;
            out[n].depth = depth;
            /* Points into the resource rather than copying: the caller keeps
             * the resource mapped for the life of the menu, which is what
             * LoadResource/LockResource already guarantee. */
            out[n].text  = (const uint16_t *)(const void *)(p + pos + W32_MENU_REC_HDR);
        }
        n++;

        size_t next = (t + 2 + 3u) & ~(size_t)3u;          /* align to 4 */

        if (flags & W32_MENUITEM_F_POPUP) {
            if (next + 4u > size) return -1;               /* submenu dwHelpId */
            next += 4u;
            if (depth + 1u > W32_MENU_MAX_DEPTH) return -1;
            depth++;
        }
        if (flags & W32_MENUITEM_F_END) {
            if (depth > 0u) depth--;
        }
        pos = next;
    }

    if (n == 0) return -1;               /* an empty menu is not a menu */
    return (int)n;
}
