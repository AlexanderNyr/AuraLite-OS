/* W32A-11 limited, real themed drawing onto caller-owned GDI DC.
 * Only v6 BUTTON/BP_PUSHBUTTON 1..5 uses the compositor palette. No success
 * for other parts/classes.  The FULL UxTheme/animation gate is pending.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/uxtheme.h"
#include "w32/comctl32.h"
#include "w32/w32_errno.h"
#include <stdint.h>
#include <stddef.h>
#define UX_HANDLE_BASE ((uintptr_t)0x74000000u)
#define UX_MAX_HANDLES 32
static struct { unsigned generation; int used; } handles[UX_MAX_HANDLES];
static volatile int ux_lock;
static void lock_ux(void) {
    while (__sync_lock_test_and_set(&ux_lock, 1))
        while (ux_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_ux(void) { __sync_lock_release(&ux_lock); }
static int valid_slot(void *theme) {
    uintptr_t v = (uintptr_t)theme;
    if (v < UX_HANDLE_BASE || v - UX_HANDLE_BASE > 0x00ffffffu) return -1;
    uintptr_t id = v - UX_HANDLE_BASE;
    unsigned slot = (unsigned)(id & 0xffu);
    if (!slot || slot > UX_MAX_HANDLES) return -1;
    --slot;
    return handles[slot].used && handles[slot].generation == (id >> 8)
        ? (int)slot : -1;
}
static int button_part(int part, int state) {
    return part == W32_THEME_BP_PUSHBUTTON &&
           state >= W32_THEME_PBS_NORMAL && state <= W32_THEME_PBS_DEFAULTED;
}
static uint32_t colorref(uint32_t rgb) {
    return ((rgb & 0xffu) << 16) | (rgb & 0x0000ff00u) |
           ((rgb >> 16) & 0xffu);
}
static int button_class(const uint16_t *name) {
    static const char button[] = "button";
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof button; ++i) {
        uint16_t c = name[i];
        if (c >= 'A' && c <= 'Z') c = (uint16_t)(c + 'a' - 'A');
        if (c != (uint16_t)button[i]) return 0;
    }
    return 1;
}
W32ABI void *OpenThemeData(W32_HWND hwnd, const uint16_t *classes) {
    (void)hwnd;
    if (!button_class(classes) || w32_comctl_version() < 6) {
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return NULL;
    }
    lock_ux();
    for (unsigned i = 0; i < UX_MAX_HANDLES; ++i) {
        if (handles[i].used) continue;
        unsigned next = handles[i].generation + 1;
        if (!next || next > 0xffffu) next = 1;
        handles[i].used = 1; handles[i].generation = next;
        unlock_ux();
        return (void *)(UX_HANDLE_BASE + ((uintptr_t)next << 8) + i + 1);
    }
    unlock_ux();
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return NULL;
}
W32ABI W32_DWORD CloseThemeData(void *theme) {
    lock_ux(); int slot = valid_slot(theme);
    if (slot >= 0) handles[slot].used = 0;
    unlock_ux();
    return slot >= 0 ? W32_THEME_S_OK : W32_THEME_E_HANDLE;
}
W32ABI W32_DWORD DrawThemeBackground(void *theme, W32_HDC hdc,
                                      int part, int state, const W32_RECT *rect,
                                      const W32_RECT *clip) {
    lock_ux(); int slot = valid_slot(theme); unlock_ux();
    if (slot < 0) return W32_THEME_E_HANDLE;
    if (!hdc || !rect || rect->right < rect->left || rect->bottom < rect->top)
        return W32_THEME_E_INVALIDARG;
    if (!button_part(part,state)) return W32_THEME_E_NOTIMPL;
    W32_RECT draw = *rect;
    if (clip) {
        if (draw.left < clip->left) draw.left = clip->left;
        if (draw.top < clip->top) draw.top = clip->top;
        if (draw.right > clip->right) draw.right = clip->right;
        if (draw.bottom > clip->bottom) draw.bottom = clip->bottom;
    }
    if (draw.left >= draw.right || draw.top >= draw.bottom) return W32_THEME_S_OK;
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    uint32_t rgb = state == W32_THEME_PBS_HOT ? pal->hot :
                   state == W32_THEME_PBS_PRESSED ? pal->sel : pal->face;
    W32_HBRUSH bg = CreateSolidBrush(colorref(rgb));
    if (!bg) return W32_THEME_E_OUTOFMEMORY;
    int painted = FillRect(hdc, &draw, bg);
    DeleteObject(bg);
    if (!painted) return W32_THEME_E_INVALIDARG;
    if (!clip || (draw.left == rect->left && draw.top == rect->top &&
                  draw.right == rect->right && draw.bottom == rect->bottom)) {
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
    lock_ux(); int slot = valid_slot(theme); unlock_ux();
    if (slot < 0) return W32_THEME_E_HANDLE;
    if (!bounds || !content || bounds->right < bounds->left ||
        bounds->bottom < bounds->top) return W32_THEME_E_INVALIDARG;
    if (!button_part(part,state)) return W32_THEME_E_NOTIMPL;
    *content = *bounds;
    if (content->right - content->left >= 4) { content->left+=2; content->right-=2; }
    if (content->bottom - content->top >= 4) { content->top+=2; content->bottom-=2; }
    return W32_THEME_S_OK;
}
