/* user32_priv.h — the seam inside the USER32 implementation.
 *
 * W32APP_PLAN.md phase W32A-5 split the personality's USER32 into two
 * translation units:
 *
 *   user32_win.c  the W core (classes, windows, queues, geometry, metrics)
 *   user32.c      the GDI half W32-5 wrote (DC state, drawing, brushes) and
 *                 the A entry points that forward into the W core
 *
 * Only these four functions cross the seam, and each exists because the
 * other side owns the state it answers for.  Anything else that wanted to
 * cross would mean the split is wrong.
 */

#ifndef AURALITE_W32_USER32_PRIV_H
#define AURALITE_W32_USER32_PRIV_H

#include "w32/user32.h"

/* user32_win.c -> user32.c: make the DC for a live window slot.  The HDC
 * encoding (and the DC's text colour and current position) belongs to the
 * GDI half; the window table belongs to the W half. */
W32_HDC w32_win_make_dc(int win_index);

/* user32_win.c -> user32.c: everything the DC half needs to draw into a
 * window without owning the window table. */
int      w32_win_ag_wid(int win_index);        /* -1 if the slot is free */
uint32_t w32_win_client_w(int win_index);
uint32_t w32_win_client_h(int win_index);
uint32_t w32_win_bg_color(int win_index);      /* the class brush colour */
int      w32_win_index_from_hwnd(W32_HWND hwnd);
W32_HWND w32_win_hwnd_from_index(int i);
int      w32_win_live_count(void);

#endif /* AURALITE_W32_USER32_PRIV_H */
