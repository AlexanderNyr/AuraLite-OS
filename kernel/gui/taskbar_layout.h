/* kernel/gui/taskbar_layout.h
 *
 * OF0 (docs/plans/OFFICE_PLAN.md): pure geometry for the taskbar's window
 * strip and its overflow list.
 *
 * Why a header of pure functions: the 2026-11-08 live pass found the taskbar
 * losing windows.  draw_taskbar() hardcoded a 130 px button and `break`ed when
 * the strip filled; taskbar_hit() hardcoded the same 130 px in its own copy of
 * the walk and stopped somewhere else, so once the strip was full a window was
 * drawn nowhere and could not be clicked.  Ten open windows were reproduced as
 * "Start + six buttons".  Two copies of one piece of arithmetic is the bug;
 * one pure function called by both is the fix, and being pure it can be tested
 * on the host (`tests/unit/test_taskbar_layout.c`) without booting a kernel.
 *
 * The layout, left to right:
 *
 *   [4 .. 94)  Start button                  (GUI_TB_START_W wide)
 *   [102 ..)   window buttons, gap 4         (shrink-to-fit)
 *   [.. fw-98) still buttons                 (GUI_TB_TRAY_W reserved right)
 *   [fw-98..]  clock / tray
 *
 * Button width: 130 px while they fit, then shrunk, never below 44 px.  When
 * even 44 px does not fit, the last slot becomes a "»" button and the windows
 * that do not fit are listed in a dropdown above it.  The dropdown itself
 * pages, so the count of open windows can never exceed the count of reachable
 * ones -- which is the actual requirement.
 *
 * All widths in device pixels; all counts in windows.
 */

#ifndef AURALITE_GUI_TASKBAR_LAYOUT_H
#define AURALITE_GUI_TASKBAR_LAYOUT_H

#define GUI_TB_START_X      4u
#define GUI_TB_START_W     90u
#define GUI_TB_GAP          4u
#define GUI_TB_BTN_MAX    130u
#define GUI_TB_BTN_MIN     44u
#define GUI_TB_TRAY_W      98u   /* clock text + right margin */
#define GUI_TB_STRIP_X     (GUI_TB_START_X + GUI_TB_START_W + 8u)

#define GUI_TB_OV_ROW_H    18u
#define GUI_TB_OV_PAD       6u
#define GUI_TB_OV_W       170u
#define GUI_TB_OV_ROWS_MAX 12u
#define GUI_TB_OV_MIN_ROWS  3u

/* Right edge the button strip may reach. */
static unsigned gui_tb_strip_end(unsigned fw) {
    unsigned right = GUI_TB_TRAY_W + 8u;
    return (fw > right) ? fw - right : GUI_TB_STRIP_X;
}

/* Button width at this width and window count, before the overflow decision.
 * Clamped to [GUI_TB_BTN_MIN, GUI_TB_BTN_MAX]. */
static unsigned gui_tb_button_width(unsigned fw, unsigned n) {
    unsigned end = gui_tb_strip_end(fw);
    if (n == 0u) return GUI_TB_BTN_MAX;
    if (end <= GUI_TB_STRIP_X) return GUI_TB_BTN_MIN;
    unsigned span = end - GUI_TB_STRIP_X;
    unsigned per = span / n;
    unsigned w = (per > GUI_TB_GAP) ? per - GUI_TB_GAP : 0u;
    if (w > GUI_TB_BTN_MAX) w = GUI_TB_BTN_MAX;
    if (w < GUI_TB_BTN_MIN) w = GUI_TB_BTN_MIN;
    return w;
}

/* How many minimum-width buttons fit. */
static unsigned gui_tb_capacity(unsigned fw) {
    unsigned end = gui_tb_strip_end(fw);
    if (end <= GUI_TB_STRIP_X) return 0u;
    return (end - GUI_TB_STRIP_X + GUI_TB_GAP) / (GUI_TB_BTN_MIN + GUI_TB_GAP);
}

typedef struct gui_tb_layout {
    unsigned x0;        /* x of the first window button */
    unsigned btn_w;     /* width of every normal button */
    unsigned visible;   /* how many normal buttons are drawn */
    unsigned more_w;    /* width of the "»" button, 0 when there is no overflow */
    unsigned more_x;    /* x of the "»" button (valid when more_w != 0) */
    unsigned end;       /* right edge the strip may reach */
} gui_tb_layout_t;

/* The single decision the renderer and the hit test both consume. */
static void gui_tb_layout(unsigned fw, unsigned n, gui_tb_layout_t *out) {
    unsigned end = gui_tb_strip_end(fw);
    unsigned w = gui_tb_button_width(fw, n);
    unsigned span = (end > GUI_TB_STRIP_X) ? end - GUI_TB_STRIP_X : 0u;

    out->x0 = GUI_TB_STRIP_X;
    out->end = end;
    out->btn_w = w;
    out->more_w = 0u;
    out->more_x = 0u;

    if (n == 0u) { out->visible = 0u; return; }

    /* Do all n buttons fit at the chosen width?  The last button carries no
     * trailing gap, hence the +GAP on the right-hand side. */
    if (n * (w + GUI_TB_GAP) <= span + GUI_TB_GAP) {
        out->visible = n;
        return;
    }

    /* No: the last minimum-width slot becomes "»" and lists the rest. */
    unsigned cap = gui_tb_capacity(fw);
    if (cap == 0u) {
        /* Absurdly narrow screen: draw one button and overflow the rest. */
        out->visible = 0u;
        out->more_w = w;
        out->more_x = GUI_TB_STRIP_X;
        return;
    }
    out->visible = (cap > 0u) ? cap - 1u : 0u;
    out->more_w = w;
    out->more_x = GUI_TB_STRIP_X + out->visible * (w + GUI_TB_GAP);
}

/* Rect of normal button idx, or 0 when that button is not drawn (it is in the
 * overflow list instead). */
static int gui_tb_button_rect(unsigned fw, unsigned n, unsigned idx,
                              unsigned *x, unsigned *w) {
    gui_tb_layout_t l;
    gui_tb_layout(fw, n, &l);
    if (idx >= l.visible) return 0;
    unsigned bx = l.x0 + idx * (l.btn_w + GUI_TB_GAP);
    if (bx + l.btn_w > l.end) return 0;
    if (x) *x = bx;
    if (w) *w = l.btn_w;
    return 1;
}

/* ---- Overflow dropdown geometry ---------------------------------------- */

typedef struct gui_tb_overflow {
    int x, y;
    unsigned w, h;
    unsigned rows;      /* rows drawn */
    unsigned row_h;
    unsigned hidden;    /* windows not shown as buttons */
    unsigned per_page;  /* real entries per page (one row is the "more" row) */
    int paged;          /* nonzero when not every hidden entry fits at once */
} gui_tb_overflow_t;

/* Geometry of the dropdown above the "»" button. */
static void gui_tb_overflow_geometry(unsigned fw, unsigned fh, unsigned tb_h,
                                     unsigned n, gui_tb_overflow_t *out) {
    gui_tb_layout_t l;
    gui_tb_layout(fw, n, &l);

    out->hidden = (n > l.visible) ? n - l.visible : 0u;
    out->row_h = GUI_TB_OV_ROW_H;
    out->w = GUI_TB_OV_W;
    out->per_page = out->hidden;

    unsigned room = (fh > tb_h + GUI_TB_OV_PAD + 8u) ? fh - tb_h - GUI_TB_OV_PAD - 8u : 0u;
    unsigned room_rows = room / GUI_TB_OV_ROW_H;
    if (room_rows > GUI_TB_OV_ROWS_MAX) room_rows = GUI_TB_OV_ROWS_MAX;

    unsigned rows = (out->hidden < room_rows) ? out->hidden : room_rows;
    out->paged = (out->hidden > rows) ? 1 : 0;
    out->rows = rows;

    out->x = (int)l.more_x;
    if (out->x + (int)out->w > (int)fw) out->x = (int)fw - (int)out->w;
    if (out->x < (int)GUI_TB_START_X) out->x = (int)GUI_TB_START_X;

    unsigned h = out->rows * GUI_TB_OV_ROW_H + 2u * GUI_TB_OV_PAD;
    out->h = h;
    out->y = (int)(fh - tb_h) - (int)h;

    if (out->rows == 0u) { out->paged = 0; out->h = 0; }
}

/* Index within the hidden list for row r, or -1 when r is the "more" row or
 * out of range.  `page` is the current page, in pages. */
static int gui_tb_overflow_row_entry(const gui_tb_overflow_t *ov, unsigned page,
                                     unsigned r) {
    if (r >= ov->rows) return -1;
    if (ov->paged) {
        if (r + 1u >= ov->rows) return -1;          /* last row is "» more" */
        unsigned per = ov->rows - 1u;
        if (per == 0u) return -1;
        unsigned start = page * per;
        if (start >= ov->hidden) start %= ov->hidden;
        unsigned idx = start + r;
        if (idx >= ov->hidden) idx -= ov->hidden;
        return (int)idx;
    }
    return (int)r;
}

/* Which page a click on the "more" row advances to. */
static unsigned gui_tb_overflow_next_page(const gui_tb_overflow_t *ov, unsigned page) {
    unsigned per = (ov->rows > 0u) ? ov->rows - 1u : 1u;
    if (per == 0u) per = 1u;
    unsigned pages = (ov->hidden + per - 1u) / per;
    if (pages == 0u) return 0u;
    return (page + 1u) % pages;
}

#endif /* AURALITE_GUI_TASKBAR_LAYOUT_H */
