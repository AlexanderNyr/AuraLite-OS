/* W32A-11: flat, pixel-backed theme onto the same compositor palette as
 * COMCTL32 v6. Every accepted class/part/state paints; unknown parts and
 * nonzero-time animation are refused, not called 'themed'. Public Win64
 * UxTheme signatures/layouts; SPDX-License-Identifier: Apache-2.0. */
#include "w32/uxtheme.h"
#include "w32/comctl32.h"
#include "w32/w32_errno.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(W32_DTTOPTS) == 72 &&
               offsetof(W32_DTTOPTS, pfnDrawTextCallback) == 56,
               "Win64 DTTOPTS layout");
_Static_assert(sizeof(W32_BP_PAINTPARAMS) == 24 &&
               sizeof(W32_BP_ANIMATIONPARAMS) == 16,
               "Win64 buffered animation layout");
#define UX_HANDLE_BASE ((uintptr_t)0x74000000u)
#define UX_ANIM_BASE ((uintptr_t)0x75000000u)
#define UX_MAX_HANDLES 32
#define UX_MAX_OVERRIDES 32
#define UX_MAX_ANIMS 8
/* Accepted part matrix, published VS part IDs; unsupported class/part/state
 * returns E_NOTIMPL and does not mutate the destination DC:
 * BUTTON       BP_PUSHBUTTON(1): 1..5
 * EDIT         EP_EDITTEXT(1): 1..7
 * TAB          TABP_TABITEM(1): 1..4
 * PROGRESS     PP_BAR/PP_BARVERT(1,2), PP_CHUNK/PP_CHUNKVERT(3,4): 0
 * COMBOBOX     CP_DROPDOWNBUTTON(1): 1..4 */
enum ux_class { UX_NONE, UX_BUTTON, UX_EDIT, UX_TAB, UX_PROGRESS, UX_COMBO };
static struct theme_handle {
    unsigned generation;
    int used;
    enum ux_class cls;
    W32_HWND hwnd;
} handles[UX_MAX_HANDLES];
static struct theme_override {
    W32_HWND hwnd;
    enum ux_class cls;
    int disabled;
    W32_DWORD dialog_flags;
} overrides[UX_MAX_OVERRIDES];
static struct animation_buffer {
    unsigned generation;
    int used;
    W32_HWND hwnd;
    W32_HDC target, from, to;
    W32_HBITMAP from_bitmap, to_bitmap;
    W32_HGDIOBJ old_from, old_to;
    W32_RECT rect;
} animations[UX_MAX_ANIMS];
static unsigned paint_depth;
static volatile int ux_lock;
static void lock_ux(void) {
    while (__sync_lock_test_and_set(&ux_lock, 1))
        while (ux_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_ux(void) { __sync_lock_release(&ux_lock); }
static int valid_slot(void *theme) { /* lock held */
    uintptr_t v = (uintptr_t)theme;
    if (v < UX_HANDLE_BASE || v - UX_HANDLE_BASE > 0x00ffffffu) return -1;
    uintptr_t id = v - UX_HANDLE_BASE;
    unsigned slot = (unsigned)(id & 0xffu);
    if (!slot || slot > UX_MAX_HANDLES) return -1;
    --slot;
    return handles[slot].used && handles[slot].generation == (id >> 8)
        ? (int)slot : -1;
}
static int valid_anim(void *buffer) { /* lock held */
    uintptr_t v = (uintptr_t)buffer;
    if (v < UX_ANIM_BASE || v - UX_ANIM_BASE > 0x00ffffffu) return -1;
    uintptr_t id = v - UX_ANIM_BASE;
    unsigned slot = (unsigned)(id & 0xffu);
    if (!slot || slot > UX_MAX_ANIMS) return -1;
    --slot;
    return animations[slot].used && animations[slot].generation == (id >> 8)
        ? (int)slot : -1;
}
static unsigned next_gen(unsigned old) { return old >= 0xffffu ? 1u : old + 1u; }
static uint32_t colorref(uint32_t rgb) {
    return ((rgb & 0xffu) << 16) | (rgb & 0x0000ff00u) |
           ((rgb >> 16) & 0xffu);
}
static enum ux_class class_token(const uint16_t *s, size_t n) {
    static const struct { const char *name; enum ux_class cls; } names[] = {
        { "button", UX_BUTTON }, { "edit", UX_EDIT }, { "tab", UX_TAB },
        { "progress", UX_PROGRESS }, { "combobox", UX_COMBO }
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        const char *p = names[i].name;
        size_t j = 0;
        while (j < n && p[j] &&
               (s[j] == (uint16_t)p[j] ||
                (s[j] >= 'A' && s[j] <= 'Z' &&
                 s[j] - 'A' + 'a' == (uint16_t)p[j]))) ++j;
        if (j == n && p[j] == '\0') return names[i].cls;
    }
    return UX_NONE;
}
static enum ux_class class_list(const uint16_t *s) {
    if (!s) return UX_NONE;
    /* Semicolon-separated VS class lists are commonplace. Bound parsing: a
     * missing terminator must not scan indefinitely into unrelated memory. */
    for (size_t p = 0; p < 96 && s[p];) {
        size_t start = p;
        while (p < 96 && s[p] && s[p] != ';') ++p;
        enum ux_class cls = class_token(s + start, p - start);
        if (cls != UX_NONE) return cls;
        if (p >= 96 || !s[p]) break;
        ++p;
    }
    return UX_NONE;
}
static int supported(enum ux_class cls, int part, int state) {
    switch (cls) {
    case UX_BUTTON: return part == 1 && state >= 1 && state <= 5;
    case UX_EDIT: return part == 1 && state >= 1 && state <= 7;
    case UX_TAB: return part == 1 && state >= 1 && state <= 4;
    case UX_PROGRESS: return part >= 1 && part <= 4 && state == 0;
    case UX_COMBO: return part == 1 && state >= 1 && state <= 4;
    default: return 0;
    }
}
static enum ux_class get_class(void *theme) {
    enum ux_class cls = UX_NONE;
    lock_ux();
    int slot = valid_slot(theme);
    if (slot >= 0) cls = handles[slot].cls;
    unlock_ux();
    return cls;
}
static int override_slot(W32_HWND hwnd, int create) { /* lock held */
    int empty = -1;
    for (int i = 0; i < UX_MAX_OVERRIDES; ++i) {
        if (overrides[i].hwnd == hwnd) return i;
        if (!overrides[i].hwnd && empty < 0) empty = i;
    }
    return create ? empty : -1;
}
W32ABI void *OpenThemeData(W32_HWND hwnd, const uint16_t *classes) {
    enum ux_class cls = class_list(classes);
    if (hwnd && !IsWindow(hwnd)) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE); return NULL;
    }
    if (!cls || w32_comctl_version() < 6) {
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return NULL;
    }
    lock_ux();
    if (hwnd) {
        int o = override_slot(hwnd, 0);
        if (o >= 0) {
            if (overrides[o].disabled) cls = UX_NONE;
            else if (overrides[o].cls) cls = overrides[o].cls;
        }
    }
    if (cls) for (unsigned i = 0; i < UX_MAX_HANDLES; ++i) {
        if (handles[i].used) continue;
        unsigned gen = next_gen(handles[i].generation);
        handles[i] = (struct theme_handle){gen, 1, cls, hwnd};
        unlock_ux();
        return (void *)(UX_HANDLE_BASE + ((uintptr_t)gen << 8) + i + 1u);
    }
    unlock_ux();
    w32_set_last_error(cls ? W32_ERROR_NOT_ENOUGH_MEMORY : W32_ERROR_NOT_SUPPORTED);
    return NULL;
}
W32ABI W32_DWORD CloseThemeData(void *theme) {
    lock_ux();
    int slot = valid_slot(theme);
    if (slot >= 0) handles[slot].used = 0;
    unlock_ux();
    return slot >= 0 ? W32_THEME_S_OK : W32_THEME_E_HANDLE;
}
W32ABI W32_DWORD DrawThemeBackground(void *theme, W32_HDC hdc,
                                      int part, int state, const W32_RECT *rect,
                                      const W32_RECT *clip) {
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!hdc || !rect || rect->right < rect->left || rect->bottom < rect->top)
        return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, state)) return W32_THEME_E_NOTIMPL;
    W32_RECT draw = *rect;
    if (clip) {
        if (draw.left < clip->left) draw.left = clip->left;
        if (draw.top < clip->top) draw.top = clip->top;
        if (draw.right > clip->right) draw.right = clip->right;
        if (draw.bottom > clip->bottom) draw.bottom = clip->bottom;
    }
    if (draw.left >= draw.right || draw.top >= draw.bottom) return W32_THEME_S_OK;
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    uint32_t rgb = pal->face;
    if (cls == UX_PROGRESS && part >= 3) rgb = pal->sel;
    else if (state == 2) rgb = pal->hot;
    else if (state == 3 || (cls == UX_TAB && state == 4)) rgb = pal->sel;
    W32_HBRUSH bg = CreateSolidBrush(colorref(rgb));
    if (!bg) return W32_THEME_E_OUTOFMEMORY;
    int painted = FillRect(hdc, &draw, bg);
    DeleteObject(bg);
    if (!painted) return W32_THEME_E_INVALIDARG;
    /* The only non-rectangular effect here is an inset frame. A clipped
     * rectangle cannot draw its OUTSIDE border without modifying the clip. */
    if ((cls == UX_BUTTON || cls == UX_EDIT || cls == UX_TAB ||
         cls == UX_COMBO || (cls == UX_PROGRESS && part <= 2)) &&
        draw.left == rect->left && draw.top == rect->top &&
        draw.right == rect->right && draw.bottom == rect->bottom) {
        W32_HBRUSH edge = CreateSolidBrush(colorref(pal->frame));
        if (!edge) return W32_THEME_E_OUTOFMEMORY;
        painted = FrameRect(hdc, rect, edge);
        DeleteObject(edge);
        if (!painted) return W32_THEME_E_INVALIDARG;
    }
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD GetThemeBackgroundContentRect(void *theme, W32_HDC hdc,
                                                 int part, int state,
                                                 const W32_RECT *bounds,
                                                 W32_RECT *content) {
    (void)hdc;
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!bounds || !content || bounds->right < bounds->left ||
        bounds->bottom < bounds->top) return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, state)) return W32_THEME_E_NOTIMPL;
    *content = *bounds;
    int inset = cls == UX_PROGRESS && part >= 3 ? 0 : 2;
    if (content->right - content->left >= 2 * inset) {
        content->left += inset; content->right -= inset;
    }
    if (content->bottom - content->top >= 2 * inset) {
        content->top += inset; content->bottom -= inset;
    }
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD GetThemePartSize(void *theme, W32_HDC dc, int part, int state,
                                   W32_RECT *bounds, int which, W32_SIZE *out) {
    (void)dc;
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!out || which < W32_THEME_TS_MIN || which > W32_THEME_TS_DRAW)
        return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, state)) return W32_THEME_E_NOTIMPL;
    if (which == W32_THEME_TS_DRAW && bounds) {
        if (bounds->right < bounds->left || bounds->bottom < bounds->top)
            return W32_THEME_E_INVALIDARG;
        out->cx = bounds->right - bounds->left;
        out->cy = bounds->bottom - bounds->top;
    } else {
        /* Stable minimal sizes for the flat raster parts; TS_DRAW without
         * a rect answers TS_TRUE, per the documented optional rectangle. */
        switch (cls) {
        case UX_BUTTON: out->cx = 18; out->cy = 16; break;
        case UX_EDIT: out->cx = 18; out->cy = 18; break;
        case UX_TAB: out->cx = 24; out->cy = 20; break;
        case UX_PROGRESS:
            out->cx = part >= 3 ? 6 : 14;
            out->cy = part >= 3 ? 10 : 14; break;
        case UX_COMBO: out->cx = 17; out->cy = 17; break;
        default: return W32_THEME_E_NOTIMPL;
        }
    }
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD GetThemeFont(void *theme, W32_HDC hdc, int part, int state,
                               int prop, W32_LOGFONTW *out) {
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!out) return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, state) || prop != W32_THEME_TMT_FONT)
        return W32_THEME_E_NOTIMPL;
    W32_HGDIOBJ font = hdc ? GetCurrentObject(hdc, W32_OBJ_FONT) : NULL;
    if (!font) font = GetStockObject(W32_DEFAULT_GUI_FONT);
    return font && GetObjectW(font, sizeof *out, out) == (int32_t)sizeof *out
        ? W32_THEME_S_OK : W32_THEME_E_HANDLE;
}
W32ABI W32_DWORD GetThemeTransitionDuration(void *theme, int part, int from,
                                             int to, int prop,
                                             W32_DWORD *milliseconds) {
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!milliseconds) return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, from) || !supported(cls, part, to) ||
        prop != W32_THEME_TMT_TRANSITIONDURATIONS) return W32_THEME_E_NOTIMPL;
    *milliseconds = 0;       /* flat theme has NO timed transition */
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD DrawThemeTextEx(void *theme, W32_HDC dc, int part, int state,
                                   const uint16_t *text, int32_t count,
                                   W32_DWORD flags, W32_RECT *rect,
                                   const W32_DTTOPTS *opts) {
    enum ux_class cls = get_class(theme);
    if (!cls) return W32_THEME_E_HANDLE;
    if (!dc || !text || !rect || count < -1 ||
        rect->right < rect->left || rect->bottom < rect->top)
        return W32_THEME_E_INVALIDARG;
    if (!supported(cls, part, state)) return W32_THEME_E_NOTIMPL;
    if (opts && (opts->dwSize < sizeof *opts ||
                 opts->dwFlags & ~(W32_THEME_DTT_TEXTCOLOR | W32_THEME_DTT_CALCRECT)))
        return opts->dwSize < sizeof *opts ? W32_THEME_E_INVALIDARG : W32_THEME_E_NOTIMPL;
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    W32_DWORD color = (opts && (opts->dwFlags & W32_THEME_DTT_TEXTCOLOR))
        ? opts->crText : colorref(state == 4 ? pal->frame : pal->text);
    W32_DWORD old_color = SetTextColor(dc, color);
    if (old_color == 0xffffffffu) return W32_THEME_E_HANDLE;
    int32_t old_mode = SetBkMode(dc, W32_TRANSPARENT);
    if (!old_mode) { SetTextColor(dc, old_color); return W32_THEME_E_HANDLE; }
    int height = DrawTextW(dc, text, count, rect,
                           flags | ((opts && (opts->dwFlags & W32_THEME_DTT_CALCRECT))
                                    ? W32_DT_CALCRECT : 0));
    SetBkMode(dc, old_mode);
    SetTextColor(dc, old_color);
    return height > 0 ? W32_THEME_S_OK : W32_THEME_E_INVALIDARG;
}
W32ABI W32_DWORD DrawThemeParentBackground(W32_HWND hwnd, W32_HDC dc,
                                             W32_RECT *rect) {
    if (!hwnd || !IsWindow(hwnd)) return W32_THEME_E_HANDLE;
    if (!dc) return W32_THEME_E_INVALIDARG;
    W32_RECT bounds;
    if (!rect) {
        if (!GetClientRect(hwnd, &bounds)) return W32_THEME_E_HANDLE;
        rect = &bounds;
    }
    if (rect->right < rect->left || rect->bottom < rect->top)
        return W32_THEME_E_INVALIDARG;
    /* Native theme has flat panel backgrounds, no image or alpha layer. */
    W32_HBRUSH b = CreateSolidBrush(colorref(w32_comctl_palette()->face));
    if (!b) return W32_THEME_E_OUTOFMEMORY;
    int ok = FillRect(dc, rect, b);
    DeleteObject(b);
    return ok ? W32_THEME_S_OK : W32_THEME_E_HANDLE;
}
W32ABI W32_DWORD SetWindowTheme(W32_HWND hwnd, const uint16_t *subapp,
                                 const uint16_t *class_name) {
    if (!hwnd || !IsWindow(hwnd)) return W32_THEME_E_HANDLE;
    if (subapp && subapp[0] && !(subapp[0] == 'A' && subapp[1] == 'u' &&
                                  subapp[2] == 'r' && subapp[3] == 'a' &&
                                  subapp[4] == 0)) return W32_THEME_E_NOTIMPL;
    enum ux_class cls = class_name && class_name[0] ? class_list(class_name) : UX_NONE;
    if (class_name && class_name[0] && !cls) return W32_THEME_E_NOTIMPL;
    lock_ux();
    int i = override_slot(hwnd, 1);
    if (i >= 0) {
        overrides[i].hwnd = hwnd;
        overrides[i].cls = cls;
        overrides[i].disabled = (subapp && !subapp[0]) ||
                                (class_name && !class_name[0]);
        if (!subapp && !class_name) memset(&overrides[i], 0, sizeof overrides[i]);
        for (int j = 0; j < UX_MAX_HANDLES; ++j)
            if (handles[j].used && handles[j].hwnd == hwnd) handles[j].used = 0;
    }
    unlock_ux();
    if (i < 0) return W32_THEME_E_OUTOFMEMORY;
    /* Let the application reopen its invalidated handle. */
    SendMessageW(hwnd, 0x031Au /* WM_THEMECHANGED */, 0, 0);
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD EnableThemeDialogTexture(W32_HWND hwnd, W32_DWORD flags) {
    if (!hwnd || !IsWindow(hwnd)) return W32_THEME_E_HANDLE;
    if (flags & ~(W32_THEME_ETDT_DISABLE | W32_THEME_ETDT_ENABLE |
                  W32_THEME_ETDT_USETABTEXTURE)) return W32_THEME_E_INVALIDARG;
    if (flags != W32_THEME_ETDT_DISABLE && flags != W32_THEME_ETDT_ENABLE &&
        flags != (W32_THEME_ETDT_ENABLE | W32_THEME_ETDT_USETABTEXTURE))
        return W32_THEME_E_INVALIDARG;
    lock_ux();
    int i = override_slot(hwnd, 1);
    if (i >= 0) {
        overrides[i].hwnd = hwnd;
        overrides[i].dialog_flags = flags;
    }
    unlock_ux();
    return i < 0 ? W32_THEME_E_OUTOFMEMORY : W32_THEME_S_OK;
}
static void destroy_anim(struct animation_buffer *a, int update) {
    if (update) (void)BitBlt(a->target, a->rect.left, a->rect.top,
                             a->rect.right - a->rect.left,
                             a->rect.bottom - a->rect.top,
                             a->to, 0, 0, W32_SRCCOPY);
    if (a->from) {
        if (a->old_from) SelectObject(a->from, a->old_from);
        DeleteDC(a->from);
    }
    if (a->to) {
        if (a->old_to) SelectObject(a->to, a->old_to);
        DeleteDC(a->to);
    }
    if (a->from_bitmap) DeleteObject(a->from_bitmap);
    if (a->to_bitmap) DeleteObject(a->to_bitmap);
}
static void discard_animations(W32_HWND hwnd) {
    /* No GDI callback under ux_lock. Consume each handle once, generation
     * protects a stale EndBufferedAnimation after HWND destruction. */
    for (int i = 0; i < UX_MAX_ANIMS; ++i) {
        lock_ux();
        int match = animations[i].used && (!hwnd || animations[i].hwnd == hwnd);
        struct animation_buffer a = {0};
        if (match) { a = animations[i]; animations[i].used = 0; }
        unlock_ux();
        if (match) destroy_anim(&a, 0);
    }
}
void w32_theme_window_destroyed(W32_HWND hwnd) {
    discard_animations(hwnd);
    lock_ux();
    for (int i = 0; i < UX_MAX_HANDLES; ++i)
        if (handles[i].used && handles[i].hwnd == hwnd) handles[i].used = 0;
    int o = override_slot(hwnd, 0);
    if (o >= 0) memset(&overrides[o], 0, sizeof overrides[o]);
    unlock_ux();
}
W32ABI W32_DWORD BufferedPaintInit(void) {
    lock_ux();
    if (paint_depth == UINT32_MAX) { unlock_ux(); return W32_THEME_E_OUTOFMEMORY; }
    ++paint_depth;
    unlock_ux();
    return W32_THEME_S_OK;
}
W32ABI W32_DWORD BufferedPaintUnInit(void) {
    lock_ux();
    int last = paint_depth == 1;
    if (paint_depth) --paint_depth;
    unlock_ux();
    if (last) discard_animations(NULL);
    return W32_THEME_S_OK;
}
W32ABI void *BeginBufferedAnimation(W32_HWND hwnd, W32_HDC target,
                                     const W32_RECT *rect, int format,
                                     const W32_BP_PAINTPARAMS *paint,
                                     const W32_BP_ANIMATIONPARAMS *animation,
                                     W32_HDC *from, W32_HDC *to) {
    if (from) *from = NULL;
    if (to) *to = NULL;
    if (!hwnd || !IsWindow(hwnd) || !target || !rect || !from || !to ||
        rect->right <= rect->left || rect->bottom <= rect->top ||
        rect->right - rect->left > 4096 || rect->bottom - rect->top > 4096 ||
        (paint && (paint->cbSize < sizeof *paint || paint->dwFlags ||
                   paint->prcExclude || paint->pBlendFunction)) ||
        (animation && animation->cbSize < sizeof *animation)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return NULL;
    }
    if (format != W32_THEME_BPBF_COMPATIBLEBITMAP ||
        (animation && animation->dwDuration)) {
        /* No frame scheduler in this personality. Do not claim nonzero-time
         * animations or DIB/alpha formats as functional. */
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return NULL;
    }
    lock_ux();
    int ready = paint_depth > 0;
    unlock_ux();
    if (!ready) { w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return NULL; }
    struct animation_buffer a = {0};
    a.hwnd = hwnd; a.target = target; a.rect = *rect;
    int w = rect->right - rect->left, h = rect->bottom - rect->top;
    a.from = CreateCompatibleDC(target);
    a.to = CreateCompatibleDC(target);
    if (a.from && a.to) {
        a.from_bitmap = CreateCompatibleBitmap(target, w, h);
        a.to_bitmap = CreateCompatibleBitmap(target, w, h);
    }
    if (a.from_bitmap && a.to_bitmap) {
        a.old_from = SelectObject(a.from, a.from_bitmap);
        a.old_to = SelectObject(a.to, a.to_bitmap);
    }
    if (!a.old_from || !a.old_to ||
        !BitBlt(a.from, 0, 0, w, h, target, rect->left, rect->top, W32_SRCCOPY) ||
        !BitBlt(a.to, 0, 0, w, h, a.from, 0, 0, W32_SRCCOPY)) {
        destroy_anim(&a, 0);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return NULL;
    }
    lock_ux();
    int slot = -1;
    if (paint_depth) for (int i = 0; i < UX_MAX_ANIMS; ++i)
        if (!animations[i].used) { slot = i; break; }
    if (slot >= 0) {
        a.generation = next_gen(animations[slot].generation);
        a.used = 1;
        animations[slot] = a;
    }
    unlock_ux();
    if (slot < 0) {
        destroy_anim(&a, 0);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return NULL;
    }
    *from = a.from; *to = a.to;
    return (void *)(UX_ANIM_BASE + ((uintptr_t)a.generation << 8) + slot + 1u);
}
W32ABI W32_DWORD EndBufferedAnimation(void *buffer, W32_BOOL update) {
    lock_ux();
    int slot = valid_anim(buffer);
    struct animation_buffer a = {0};
    if (slot >= 0) { a = animations[slot]; animations[slot].used = 0; }
    unlock_ux();
    if (slot < 0) return W32_THEME_E_HANDLE;
    int ok = !update || BitBlt(a.target, a.rect.left, a.rect.top,
                                a.rect.right - a.rect.left,
                                a.rect.bottom - a.rect.top,
                                a.to, 0, 0, W32_SRCCOPY);
    destroy_anim(&a, 0);
    return ok ? W32_THEME_S_OK : W32_THEME_E_HANDLE;
}
W32ABI W32_DWORD BufferedPaintStopAllAnimations(W32_HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return W32_THEME_E_HANDLE;
    discard_animations(hwnd);
    return W32_THEME_S_OK;
}
