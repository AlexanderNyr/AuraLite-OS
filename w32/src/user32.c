/* user32.c — the colour + DC-token half of the USER32 personality.
 *
 * WIN32_PLAN.md phase W32-5 wrote this file as the whole of USER32 over the
 * compositor.  W32APP_PLAN.md phase W32A-5 split it: the window and message
 * core moved to w32/src/user32_win.c (W-first, per D6).  W32A-7 moved the
 * DC's drawing half onward again: pens, brushes, fonts, bitmaps, regions and
 * the memory-DC raster engine live in w32/src/w32_gdi.c, and a DC is no
 * longer only a window -- but window DCs keep this file's HDC encoding
 * (HDC_BIAS + slot), so GetDC/BeginPaint survive untouched.
 *
 * What remains here: the COLORREF<->AuraLite conversion (both directions --
 * GDI reads pixels back now), the window-DC token factory and its reset
 * into the GDI engine, and FillRect, which the ledger pins to user32.dll
 * but whose body is the engine's.
 */

#include "w32/user32.h"
#include "w32/user32_priv.h"
#include "w32/gdi32.h"
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

/* The W half's DC factory: it hands a slot number, this half hands back
 * the token; the state behind it now lives in w32_gdi.c (W32A-7), which
 * resets the slot's DC state here. */
W32_HDC w32_win_make_dc(int win_index) {
    if (w32_win_ag_wid(win_index) < 0) return 0;
    w32_gdi_reset_win_dc(win_index);
    return idx_to_hdc(win_index);
}

/* The GDI half's window-slot lookup: same validity test the A-5 DC
 * functions used, exported for w32_gdi.c. */
int w32_hdc_win_index(W32_HDC d) {
    return hdc_to_idx(d);
}

/* The inverse: the GDI engine reads pixels back as COLORREF. */
W32_DWORD w32_ag_to_colorref(W32_DWORD ag) {
    uint32_t r = (ag >> 16) & 0xFFu;
    uint32_t g = (ag >>  8) & 0xFFu;
    uint32_t b = (ag >>  0) & 0xFFu;
    return (b << 16) | (g << 8) | r;
}

/* FillRect stays a user32 export (the ledger pins it there) but the
 * raster body is the GDI engine's, with real brush semantics: a table
 * brush draws itself (hatches anchored, clip honoured), a raw colour
 * value draws the colour -- the A-5 convention, now documented as
 * legacy-compatible rather than as "the brush is the colour". */
W32ABI int32_t FillRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush) {
    return w32_gdi_fill_rect(hdc, r, brush);
}
