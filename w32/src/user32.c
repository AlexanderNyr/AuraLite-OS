/* user32.c — the GDI/DC half of the USER32 personality.
 *
 * WIN32_PLAN.md phase W32-5 wrote this file as the whole of USER32 over the
 * compositor.  W32APP_PLAN.md phase W32A-5 split it: the window and message
 * core moved to w32/src/user32_win.c (W-first, per D6), and what remains
 * here is the device-context half -- the DC's text colour and current
 * position, the drawing calls that need only a window's compositor id, and
 * the brush-as-colour convention W32-5 established.
 *
 * A DC in this phase still names a window: there is no off-screen surface,
 * no compatible DC and no bitmap selection.  W32A-7 owns real DCs, bitmaps
 * and regions, and this file is where they will land.
 *
 * Deliberately absent: any drawing libauragui cannot already do.  A function
 * with no AG equivalent fails with a documented error rather than pretending,
 * so a program finds out at the call site.
 */

#include "w32/user32.h"
#include "w32/user32_priv.h"
#include "w32/w32_errno.h"

#ifndef AURALITE_W32_HOST_TEST
#include "auragui.h"
#include <string.h>
#include <stdio.h>
#endif

/* HWND/HDC bias: same reasoning as the HANDLE table in W32-4.  A window token
 * must not be forgeable by inventing a small integer, and must never be NULL,
 * because NULL is how every Win32 producer reports failure. */
#define HDC_BIAS  0x3000

static W32_HDC idx_to_hdc(int i) { return (W32_HDC)(intptr_t)(HDC_BIAS + i); }

static int hdc_to_idx(W32_HDC d) {
    intptr_t v = (intptr_t)d;
    if (v < HDC_BIAS) return -1;
    intptr_t i = v - HDC_BIAS;
    if (i < 0 || i >= 32) return -1;
    /* A DC is only valid while its window is: the W half owns that test. */
    if (w32_win_ag_wid((int)i) < 0) return -1;
    return (int)i;
}

/* Win32 COLORREF is 0x00BBGGRR, AuraLite is 0x00RRGGBB.  Getting this
 * backwards produces a picture that looks plausible but has red and blue
 * swapped, which is exactly the kind of bug that survives a smoke test. */
W32_DWORD w32_colorref_to_ag(W32_DWORD cr) {
    uint32_t r = (cr >>  0) & 0xFFu;
    uint32_t g = (cr >>  8) & 0xFFu;
    uint32_t b = (cr >> 16) & 0xFFu;
    return (r << 16) | (g << 8) | b;
}

/* One text colour and one current position per window slot: enough for
 * TextOut and MoveTo/LineTo, which is what these phases' gates use.  A full
 * DC state (pen, font, ROP, clip) is not modelled, and no function pretends
 * it is -- W32A-7. */
#define DC_MAX_SLOTS 32
static W32_DWORD dc_text_color[DC_MAX_SLOTS];
static int32_t   dc_cur_x[DC_MAX_SLOTS], dc_cur_y[DC_MAX_SLOTS];

static void dc_state_reset(void) {
    for (int i = 0; i < DC_MAX_SLOTS; i++) {
        dc_text_color[i] = 0;              /* COLORREF black */
        dc_cur_x[i] = 0;
        dc_cur_y[i] = 0;
    }
}

/* The W half's DC factory: it hands a slot number, this half hands back the
 * token and owns the state behind it. */
W32_HDC w32_win_make_dc(int win_index) {
    if (w32_win_ag_wid(win_index) < 0) return 0;
    if (dc_text_color[win_index] == 0xFFFFFFFFu) dc_state_reset();
    return idx_to_hdc(win_index);
}

W32ABI W32_DWORD SetTextColor(W32_HDC hdc, W32_DWORD color) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    W32_DWORD old = dc_text_color[i];
    dc_text_color[i] = color;
    return old;
}

W32ABI int32_t FillRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush) {
    int i = hdc_to_idx(hdc);
    if (i < 0 || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (r->right <= r->left || r->bottom <= r->top) return 0;
    ag_fill_rect(w32_win_ag_wid(i), r->left, r->top,
                 (uint32_t)(r->right - r->left),
                 (uint32_t)(r->bottom - r->top),
                 w32_colorref_to_ag((W32_DWORD)(uintptr_t)brush));
    return 1;
}

W32ABI W32_BOOL TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                         const char *s, int32_t len) {
    int i = hdc_to_idx(hdc);
    if (i < 0 || !s) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }

    /* ag_draw_text takes a NUL-terminated string; Win32 passes a length. */
    char buf[256];
    int n = 0;
    while (n < (int)sizeof buf - 1 && (len < 0 || n < len) && s[n]) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = '\0';
    ag_draw_text(w32_win_ag_wid(i), x, y, buf,
                 w32_colorref_to_ag(dc_text_color[i]));
    return W32_TRUE;
}

W32ABI W32_BOOL MoveToEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (old) { old->x = dc_cur_x[i]; old->y = dc_cur_y[i]; }
    dc_cur_x[i] = x; dc_cur_y[i] = y;
    return W32_TRUE;
}

W32ABI W32_BOOL LineTo(W32_HDC hdc, int32_t x, int32_t y) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ag_draw_line(w32_win_ag_wid(i), dc_cur_x[i], dc_cur_y[i], x, y,
                 w32_colorref_to_ag(dc_text_color[i]));
    dc_cur_x[i] = x; dc_cur_y[i] = y;
    return W32_TRUE;
}

W32ABI W32_DWORD SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    ag_draw_pixel(w32_win_ag_wid(i), x, y, w32_colorref_to_ag(color));
    return color;
}

/* The brush "handle" is the colour.  No allocation, no object table, and
 * DeleteObject on it is a no-op that cannot leak. */
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD color) {
    return (W32_HBRUSH)(uintptr_t)color;
}

W32ABI W32_BOOL DeleteObject(void *obj) { (void)obj; return W32_TRUE; }
