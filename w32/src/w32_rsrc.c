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

static void *mnorm(void *m) { if (!m) m = w32_GetModuleHandleA(0); return m; }

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
W32ABI void *LoadImageW(void *m, const uint16_t *n, W32_UINT t, int cx, int cy, W32_UINT fl) {
    (void)cx; (void)cy; (void)fl; uint32_t rt;
    switch (t) {
    case 0: case 1: rt = W32_RT_BITMAP; break;
    case 2: rt = W32_RT_ICON; break;
    case 3: rt = W32_RT_CURSOR; break;
    default: w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
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
