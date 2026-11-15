/* gui.c — kernel-side GUI controller and compositor.
 *
 * The AuraLite GUI is undergoing a gradual migration from C to Rust.
 * This file remains the C-facing compatibility layer and continues to own
 * window-manager state, window lifecycle, focus and z-order, input routing,
 * event delivery, shell integration, and compositor orchestration.
 *
 * Rust-side responsibilities:
 *   - gui_drawing.rs: window-buffer drawing, pixel blending, rectangles,
 *     lines, blitting, bulk pixel operations, and x86_64 SSE2 paths.
 *   - gui_dirty.rs: bounded dirty-rectangle tracking and snapshots.
 *   - sdf/gui_sdf.rs: client text rendering through the MSDF/SDF atlas.
 *
 * C-side responsibilities retained during this migration:
 *   - Window state, parenting, hit-testing, focus, and event queues.
 *   - Keyboard/mouse routing, taskbar and desktop-shell integration.
 *   - Window decorations, desktop composition, clipping, and presentation.
 *
 * The migration is intentionally incremental. Existing public interfaces in
 * gui.h remain C APIs; selected implementation details cross a narrow C ABI
 * so current kernel and userspace call sites do not need to change at once.
 */

#include <stdint.h>
#include <stddef.h>
/* SELFHOST SH5c: tcc's __atomic_*_n macros + __ATOMIC_* orders come from
 * its <stdatomic.h>; see kernel/arch/x86_64/tlb_shootdown.c. */
#include <stdatomic.h>
#include "kernel/gui/gui.h"
#include "kernel/gui/gui_shell.h"
#include "kernel/gui/taskbar_layout.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/perfstat.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/process.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/psf.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/timer/pit.h"

#define GUI_CONTENT_PAD 4u

/* Pixel SIMD implementations are provided by gui_drawing.rs. */

/* ===================================================================
 * Theme
 * =================================================================== */

static gui_theme_t active_theme;

static const gui_theme_t default_theme = {
    /* Desktop */
    .desktop_top     = 0x000F172A,
    .desktop_bot     = 0x00020617,
    /* Window */
    .win_bg          = 0x00202020,      /* frame + titlebar + padding */
    .win_content     = 0x00272727,      /* content "card" */
    .title_active    = 0x00202020,      /* unused by decor, kept for ABI */
    .title_inactive  = 0x001C1C1C,      /* unused by decor, kept for ABI */
    .title_text      = 0x00F3F3F3,
    .border          = 0x00333333,      /* 1-px outline, inactive */
    .border_active   = 0x00454545,      /* 1-px outline, active */
    /* Taskbar */
    .taskbar_bg      = 0x001C1C1C,
    .taskbar_border  = 0x002E2E2E,
    .taskbar_text    = 0x00E4E4E7,
    .start_btn_bg    = 0x000078D4,
    .start_btn_text  = 0x00FFFFFF,
    /* Buttons */
    .close_bg        = 0x00202020,
    .close_bg_hover  = 0x00C42B1C,
    .max_bg          = 0x002D2D2D,
    .min_bg          = 0x002D2D2D,
    /* Icons */
    .icon_text       = 0x00F4F4F5,
    .icon_selected   = 0x002563EB,
    /* Notifications */
    .notif_bg        = 0x00242424,
    .notif_border    = 0x00383838,
    .notif_text      = 0x00F3F3F3,
    /* Shadows */
    .shadow_color    = 0x00000000,
    .shadow_offset   = 4,

    /* Dimensions */
    .taskbar_h       = 32,
    .titlebar_h      = 28,
    .border_w        = 0,
    .resize_grip     = 12,
    .icon_size       = 32,
    .icon_pad        = 8,
    .win_round       = 12,
    /* Screen DPI (W32A-7): 96 is the system default; gtheme --dpi owns it. */
    .dpi             = 96,
};

const gui_theme_t *gui_default_theme(void) { return &default_theme; }
const gui_theme_t *gui_get_theme(void)     { return &active_theme; }

void gui_set_theme(const gui_theme_t *t) {
    if (t) {
        active_theme = *t;
    } else {
        active_theme = default_theme;
    }
    gui_request_redraw();
}

/* ===================================================================
 * Window state
 * =================================================================== */

typedef struct gui_win {
    int       in_use;
    int       visible;
    int       minimized;
    int       maximized;
    int       focused;
    int32_t   x, y;               /* outer top-left (titlebar) */
    uint32_t  w, h;               /* outer size */
    int32_t   restore_x, restore_y;
    uint32_t  restore_w, restore_h;
    uint32_t  flags;
    int       z;
    /* CW-1: parent window index, or -1 for a top-level window.  A child
     * stores its x/y RELATIVE to the parent's content origin (Win32 WS_CHILD
     * semantics); the compositor resolves absolute coords via win_abs_x/y and
     * clips the child to its parent's content rectangle. */
    int       parent;
    gui_snap_t snap;
    char      title[GUI_TITLE_MAX];
    uint32_t *back;               /* content buffer */
    uint32_t  back_w, back_h;     /* content pixels */
    /* per-window event ring */
    gui_event_t events[GUI_EVT_RING_SIZE];
    volatile uint32_t evt_head, evt_tail;
    /* W32A-11: tokenised path slots, never a cross-process user pointer. */
#define GUI_DROP_SLOTS 8
    char drop_paths[GUI_DROP_SLOTS][GUI_DROP_PATH_MAX];
    uint16_t drop_generation[GUI_DROP_SLOTS];
    uint8_t drop_busy[GUI_DROP_SLOTS];
    /* owner pid */
    int       owner_pid;
    /* per-window dirty flag for back-buffer changes */
    volatile int content_dirty;

    /* Front buffer: the last COMPLETE frame, which is what the compositor
     * draws.  NULL until a client uses the frame brackets, in which case
     * the compositor reads back[] directly as it always did.
     *
     * The compositor runs as its own thread at ~100 Hz
     * (gui_compositor_thread) and reads the window while the client writes
     * it, with no synchronisation.  For a program that repaints a little at
     * a time that is unnoticeable; for one that replaces the whole frame it
     * is textbook tearing -- DOOM showed horizontal bands from two or three
     * different frames at once.
     *
     * Skipping the window while a frame is in flight was tried first and is
     * WRONG: under TCG a 640x400 blit takes far longer than the 10 ms tick,
     * so the window was busy at essentially every tick and never drew at
     * all.  A frame the compositor is allowed to miss must therefore be a
     * COMPLETE OLD frame, not no frame -- which means keeping two buffers
     * and swapping them, not gating on a flag. */
    uint32_t *front;
    volatile int updating;
    /* WR-3: see-through hole in window-buffer pixels (GUI_OP_SET_HOLE).
     * Empty (r<=l or b<=t) means none; slots start zeroed, so windows are
     * born opaque.  The compositor skips these pixels and the hit test
     * falls through them, which is how a full-size tab control leaves its
     * display area to the editor living in it -- Win32 paints that region
     * by never painting it, an option a buffered compositor does not
     * have without an explicit hole. */
    int32_t hole_l, hole_t, hole_r, hole_b;
} gui_win_t;

/* WR-3: heap-allocated, not a static array.  At 256 slots this table is
 * ~1.2 MB; as BSS it pushed the kernel image past the region the EFI
 * bootloader maps and the first write faulted at 0xFFFFFFFF80600000 during
 * gui_init().  kmalloc serves it in 2 MiB pages after boot, so the table
 * costs nothing until the GUI starts and the kernel image stays small. */
static gui_win_t *windows;
static spinlock_t gui_lock;
/* Event rings and pathname slots share this lock. Always acquire gui_lock
 * before gui_evt_lock if both are needed (destroy/send). */
static spinlock_t gui_evt_lock;
/* Survive HWND slot recycling: a stale 16-bit token from a destroyed window
 * must not match the next occupant's first drop in the same path slot. */
static uint16_t drop_epoch[GUI_MAX_WINDOWS][GUI_DROP_SLOTS];
/* Event waiters must not follow an integer HWND into its next lifetime. */
static uint32_t window_epoch[GUI_MAX_WINDOWS];
static int focused = -1;
/* WR-2 gap 2: the KEYBOARD target, tracked separately from `focused`.
 *
 * `focused` is the window-manager focus: it picks the highlighted title bar
 * and the Alt+F4 victim, and recompute_focus() deliberately skips NO_DECOR
 * windows so a WS_CHILD pane can never own it.  That is correct for chrome
 * and fatal for input: Win32 delivers WM_KEYDOWN to the focused window, and
 * in a real app that window is usually a CHILD (7-Zip's file panel is a
 * SysListView32 WS_CHILD that owns Backspace = "up one level").
 *
 * Mouse input already had no such problem -- hit_window() descends into the
 * child subtree, so a click on the panel reaches the panel.  Keys went to
 * the top level instead, which is exactly why "double-click navigates"
 * worked while "Backspace goes up" did not.
 *
 * key_focus may therefore name a child.  -1 means "follow the top level".
 * It is reset whenever the top-level focus changes, and cleared when its
 * owner hides, minimises or dies. */
static int key_focus = -1;
/* W32A-5: SetCapture.  -1 = nobody; otherwise the wid that receives every
 * mouse event until it releases.  Cleared when the owner dies (see
 * gui_cleanup_process). */
static int captured = -1;
static gui_cursor_t cursor = GUI_CURSOR_ARROW;

/* Software cursor is a normal final compositor layer.  It remains in the
 * back buffer; repainting the old cursor rectangle removes the previous
 * image before the new position is drawn.
 * The bounds cover every cursor shape: x=[mx-8, mx+11], y=[my-8, my+15]. */
#define CURSOR_DIRTY_LEFT   8
#define CURSOR_DIRTY_TOP    8
#define CURSOR_DIRTY_W      20
#define CURSOR_DIRTY_H      24
static int last_drawn_mx = -1, last_drawn_my = -1;
static int last_drawn_valid = 0;

/* Rust dirty-rect tracker (kernel/gui/gui_dirty.rs).  Rects are kept disjoint
 * and rendered individually, so a diagonal cursor move no longer damages
 * the whole bounding box. */
typedef struct {
    int32_t x, y;
    uint32_t w, h;
} gui_dirty_rect_t;

extern void   gui_dirty_reset(void);
extern int    gui_dirty_add(int32_t x, int32_t y, uint32_t w, uint32_t h);
extern size_t gui_dirty_snapshot(gui_dirty_rect_t *out, size_t max);
extern int gui_dirty_is_empty(void);

/* Dirty-rect tracking. */
static volatile int full_dirty = 1;  /* start with a full redraw */

/* OPT_PLAN.md O4: the compositor sleeps between events instead of
 * yield-spinning at 100 Hz.  Anything that creates work pokes it:
 * keyboard/mouse IRQs, the GUI syscall layer, and the PIT's 1 Hz clock
 * line (which also drives notification expiry).  gui_pending is the
 * lost-wakeup guard: pokes can land between the "any work?" check and
 * the sleep, so the flag is checked under gui_wake_lock. */
static struct wait_queue gui_wq;
/* O7: user threads blocked in gui_wait_event(); woken by event push
 * and window destruction (with a 5-tick net for the races). */
static struct wait_queue gui_evt_wq;
static spinlock_t gui_wake_lock;
static volatile int gui_pending;
static volatile int gui_wq_ready;

void gui_poke(void) {
    if (!gui_wq_ready) return;
    __atomic_store_n(&gui_pending, 1, __ATOMIC_RELEASE);
    wq_wake_all(&gui_wq);        /* IRQ-safe since the O4 wait_queue fix */
}

/* GUI clock. */
static uint64_t gui_clock_base_ticks = 0;
static uint64_t gui_clock_last_second = (uint64_t)-1;

/* Drag/resize state. */
static int  drag_wid = -1;
static int  drag_mode = 0;          /* 0=none, 1=move, 2=resize */
static int  drag_edge = 0;          /* resize edge bitmask: 1=left 2=right 4=top 8=bottom */
static int32_t drag_dx, drag_dy;
static int32_t drag_orig_x, drag_orig_y;
static uint32_t drag_orig_w, drag_orig_h;

/* Snap preview. */
static int snap_preview_active = 0;
static gui_snap_t snap_preview_type = GUI_SNAP_NONE;

/* Hover state. */
static int last_hover_wid = -1;
/* Hover state for the title-bar buttons: which window is under the
 * pointer and which button (0=none, 1=close, 2=max, 3=min).  Used only
 * to recolour the glyph; buttons have no background any more. */
static int hover_btn_wid = -1;
static int hover_btn_kind = 0;
static uint32_t last_click_tick = 0;
static int32_t  last_click_x = -999, last_click_y = -999;
static int      last_click_wid = -999;
static int      last_click_part = -1;
static uint8_t  last_click_button = 0;

#define GUI_DBLCLICK_SLOP_PX 5

/* ===================================================================
 * Helpers
 * =================================================================== */

/* ===================================================================
 * SIMD helpers
 *
 * SSE2 is isolated in target-specific functions. The rest of the
 * kernel continues to compile with -mno-sse -mno-sse2.
 *
 * Pixel format: 0x00RRGGBB in memory as B,G,R,0.
 * =================================================================== */

/* Rust GUI drawing core ABI. Buffer validation and window ownership remain
 * in C; Rust receives only the selected window's pixel buffer and geometry. */
extern void gui_rust_fill32(uint32_t *dst, uint32_t value, size_t count);
extern void gui_rust_copy32(uint32_t *dst, const uint32_t *src, size_t count);

extern int gui_rust_draw_pixel(uint32_t *buffer, uint32_t width, uint32_t height,
                               uint32_t stride, int32_t x, int32_t y,
                               uint32_t color);
extern int gui_rust_get_pixel(const uint32_t *buffer, uint32_t width, uint32_t height,
                              uint32_t stride, int32_t x, int32_t y,
                              uint32_t *out);
extern int gui_rust_blend_pixel(uint32_t *buffer, uint32_t width, uint32_t height,
                                uint32_t stride, int32_t x, int32_t y,
                                uint32_t color, uint8_t alpha);
extern int gui_rust_blend_pixels4(uint32_t *buffer, uint32_t width, uint32_t height,
                                  uint32_t stride, int32_t x, int32_t y,
                                  uint32_t color, const uint8_t *alpha);
extern int gui_rust_fill_rect(uint32_t *buffer, uint32_t width, uint32_t height,
                              uint32_t stride, int32_t x, int32_t y,
                              uint32_t w, uint32_t h, uint32_t color);
extern int gui_rust_draw_rect(uint32_t *buffer, uint32_t width, uint32_t height,
                              uint32_t stride, int32_t x, int32_t y,
                              uint32_t w, uint32_t h, uint32_t color);
extern int gui_rust_draw_line(uint32_t *buffer, uint32_t width, uint32_t height,
                              uint32_t stride, int32_t x0, int32_t y0,
                              int32_t x1, int32_t y1, uint32_t color);
extern int gui_rust_clear(uint32_t *buffer, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t color);
extern int gui_rust_blit(uint32_t *buffer, uint32_t width, uint32_t height,
                         uint32_t stride, int32_t x, int32_t y,
                         uint32_t w, uint32_t h, const uint32_t *src,
                         uint32_t src_stride);
extern int gui_rust_blit_alpha(uint32_t *buffer, uint32_t width, uint32_t height,
                               uint32_t stride, int32_t x, int32_t y,
                               uint32_t w, uint32_t h, const uint32_t *src,
                               uint32_t src_stride);

/* Keep the existing C call sites stable for allocation, resize, and frames. */
static void gui_fill32(uint32_t *dst, uint32_t value, size_t count) {
    gui_rust_fill32(dst, value, count);
}

static void gui_copy32(uint32_t *dst, const uint32_t *src, size_t count) {
    gui_rust_copy32(dst, src, count);
}

/* Blend two 24-bit colours with an 8-bit alpha (0 = dst, 255 = src).
 * Used for the soft shadow: the source is the theme's shadow colour,
 * the destination is the desktop gradient under the window. */
static uint32_t blend_u32(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    uint32_t g = (((src >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * ia) / 255;
    uint32_t b = (((src      ) & 0xFF) * a + ((dst      ) & 0xFF) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ===================================================================
 * Rounded-corner helpers
 *
 * A rounded rectangle is drawn as r tapered rows at the top, a straight
 * slab in the middle, and r mirrored tapered rows at the bottom.  For
 * each tapered row we solve x² + y² <= r² for the largest integer x
 * and inset the fill by (r - x) pixels on both sides.  This keeps the
 * flat edges pixel-exact and only touches the corners, so the cost is
 * O(r) per corner regardless of window size.
 * =================================================================== */

static void gfx_fill_round_rect(int32_t x, int32_t y,
                                uint32_t w, uint32_t h,
                                uint32_t r, uint32_t color) {
    if (w == 0 || h == 0) return;
    uint32_t rmax = (w < h ? w : h) / 2;
    if (r > rmax) r = rmax;
    if (r == 0) {
        gfx_fill_rect((uint32_t)x, (uint32_t)y, w, h, color);
        return;
    }
    for (uint32_t row = 0; row < r; row++) {
        int32_t dy = (int32_t)r - 1 - (int32_t)row;
        int32_t rr = (int32_t)r * (int32_t)r - dy * dy;
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) <= rr) dx++;
        uint32_t inset = (uint32_t)((int32_t)r - dx);
        if (w <= 2 * inset) continue;
        uint32_t bw = w - 2 * inset;
        gfx_fill_rect((uint32_t)(x + (int32_t)inset),
                      (uint32_t)(y + (int32_t)row), bw, 1, color);
        gfx_fill_rect((uint32_t)(x + (int32_t)inset),
                      (uint32_t)(y + (int32_t)h - 1 - (int32_t)row),
                      bw, 1, color);
    }
    if (h > 2 * r) {
        gfx_fill_rect((uint32_t)x, (uint32_t)(y + (int32_t)r),
                      w, h - 2 * r, color);
    }
}

static int point_in_round_rect(int32_t mx, int32_t my,
                               int32_t x, int32_t y,
                               uint32_t w, uint32_t h,
                               uint32_t r) {
    if (mx < x || mx >= x + (int32_t)w ||
        my < y || my >= y + (int32_t)h) return 0;
    if (r == 0) return 1;
    uint32_t rmax = (w < h ? w : h) / 2;
    if (r > rmax) r = rmax;
    int32_t lx = mx - x, ly = my - y;
    int32_t cx, cy;
    if (lx < (int32_t)r) cx = (int32_t)r;
    else if (lx > (int32_t)w - 1 - (int32_t)r) cx = (int32_t)w - 1 - (int32_t)r;
    else return 1;
    if (ly < (int32_t)r) cy = (int32_t)r;
    else if (ly > (int32_t)h - 1 - (int32_t)r) cy = (int32_t)h - 1 - (int32_t)r;
    else return 1;
    int32_t dx = lx - cx, dy = ly - cy;
    return dx * dx + dy * dy <= (int32_t)r * (int32_t)r;
}

static int win_alive(int wid) {
    return windows && wid >= 0 && wid < GUI_MAX_WINDOWS && windows[wid].in_use;
}

/* ---- Helpers for gui_shell.c -------------------------------------- */

int gui_taskbar_win_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & (GUI_WIN_NO_DECOR | GUI_WIN_TOOL_WINDOW))
            continue;
        n++;
    }
    return n;
}

int gui_taskbar_win_at(int slot) {
    int n = 0;
    if (slot < 0) return -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & (GUI_WIN_NO_DECOR | GUI_WIN_TOOL_WINDOW))
            continue;
        if (n == slot) return i;
        n++;
    }
    return -1;
}

void gui_get_window_title(int wid, char *buf, uint32_t size) {
    if (!buf || size == 0) return;
    buf[0] = 0;
    if (!win_alive(wid)) return;
    uint32_t n = 0;
    while (n < size - 1 && windows[wid].title[n]) {
        buf[n] = windows[wid].title[n];
        n++;
    }
    buf[n] = 0;
}

int gui_window_is_minimized(int wid) {
    if (!win_alive(wid)) return 0;
    return windows[wid].minimized;
}

/* A visible presentation window owns the whole display.  Unlike NO_DECOR,
 * which only removes a titlebar, this mode also suppresses the taskbar and
 * notification/snap overlays so the app can show exactly one slide per
 * screen.  The window itself remains an ordinary owner-checked GUI surface. */
int gui_presentation_active(void) {
    if (!windows) return 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        const gui_win_t *w = &windows[i];
        if (w->in_use && w->visible && !w->minimized &&
            (w->flags & GUI_WIN_PRESENTATION)) return 1;
    }
    return 0;
}

static int32_t abs_diff(int32_t a, int32_t b) { return a > b ? a - b : b - a; }

int gui_is_double_click(uint32_t now, int32_t x, int32_t y,
                               int wid, int part, uint8_t button) {
    return last_click_tick != 0 &&
           last_click_wid == wid &&
           last_click_part == part &&
           last_click_button == button &&
           (uint32_t)(now - last_click_tick) <= MOUSE_DBLCLICK_TICKS &&
           abs_diff(x, last_click_x) <= GUI_DBLCLICK_SLOP_PX &&
           abs_diff(y, last_click_y) <= GUI_DBLCLICK_SLOP_PX;
}

void gui_record_click(uint32_t now, int32_t x, int32_t y,
                             int wid, int part, uint8_t button) {
    last_click_tick = now;
    last_click_x = x;
    last_click_y = y;
    last_click_wid = wid;
    last_click_part = part;
    last_click_button = button;
}

/* Content area helpers — read from active theme dimensions. */
static uint32_t c_border(void)    { return active_theme.border_w; }
static uint32_t c_titlebar(void)  { return active_theme.titlebar_h; }

/* Height of the active console/GUI font, used to vertically centre
 * single-line text inside a fixed-height bar (titlebar, taskbar, start
 * button, notifications, ...). Falls back to 8 if no font is loaded yet
 * (shouldn't happen once fb_init()/psf_init() have run, but keeps this
 * arithmetic safe either way). */
uint32_t gui_font_h(void) {
    const struct psf_font *f = psf_get_font();
    return f ? f->height : 8;
}

static uint32_t content_w(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->w;
    uint32_t inner = w->w - 2 * c_border();
    uint32_t p = 2 * GUI_CONTENT_PAD;
    return inner > p ? inner - p : 0;
}
static uint32_t content_h(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->h;
    uint32_t inner = w->h - c_titlebar() - 2 * c_border();
    uint32_t p = 2 * GUI_CONTENT_PAD;
    return inner > p ? inner - p : 0;
}

/* CW-1: is this window a live child of a valid parent slot? */
static int win_has_parent(const gui_win_t *w) {
    return w->parent >= 0 && w->parent < GUI_MAX_WINDOWS &&
           windows[w->parent].in_use && &windows[w->parent] != w;
}

/* Forward decls: win_abs_* and content_* are mutually recursive up the
 * parent chain (a child's absolute origin is its parent's content origin
 * plus the child's parent-relative x/y). */
static int32_t content_x(const gui_win_t *w);
static int32_t content_y(const gui_win_t *w);

/* Absolute (screen) outer top-left of a window, resolving the parent chain.
 * Top-level windows store absolute coords (w->parent < 0 ⇒ raw); a child
 * stores coords relative to its parent's content origin. */
static int32_t win_abs_x(const gui_win_t *w) {
    if (!win_has_parent(w)) return w->x;
    return content_x(&windows[w->parent]) + w->x;
}
static int32_t win_abs_y(const gui_win_t *w) {
    if (!win_has_parent(w)) return w->y;
    return content_y(&windows[w->parent]) + w->y;
}

static int32_t content_x(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return win_abs_x(w);
    return win_abs_x(w) + (int32_t)c_border() + (int32_t)GUI_CONTENT_PAD;
}
static int32_t content_y(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return win_abs_y(w);
    return win_abs_y(w) + (int32_t)c_titlebar()
         + (int32_t)c_border() + (int32_t)GUI_CONTENT_PAD;
}

/* Mark a screen-space rectangle as dirty. */
void gui_mark_dirty(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    if (full_dirty) return;
    if (gui_dirty_add(x, y, w, h) < 0) {
        /* Overflow: Rust dropped the list.  Promote to a full redraw. */
        full_dirty = 1;
        gui_dirty_reset();
    }
}

/* Damage the complete cursor footprint.  The same rectangle is used for
 * old and new positions, so shape changes and edge clipping are safe. */
static void gui_mark_cursor_dirty(int32_t x, int32_t y) {
    gui_mark_dirty(x - CURSOR_DIRTY_LEFT,
                   y - CURSOR_DIRTY_TOP,
                   CURSOR_DIRTY_W,
                   CURSOR_DIRTY_H);
}

/* Mark the bounding rect of a window (including shadow) as dirty. */
static void mark_window_dirty(const gui_win_t *w) {
    int off = active_theme.shadow_offset;
    /* CW-1: use the absolute origin so a child marks the right screen region. */
    gui_mark_dirty(win_abs_x(w) - 1, win_abs_y(w) - 1, w->w + off + 2, w->h + off + 2);
}

/* ===================================================================
 * Init
 * =================================================================== */

void gui_init(void) {
    if (!windows) {
        windows = (gui_win_t *)kmalloc((size_t)GUI_MAX_WINDOWS *
                                       sizeof(gui_win_t));
        if (!windows) {
            kprintf("[gui] window table: kmalloc of %u slots failed\n",
                    (unsigned)GUI_MAX_WINDOWS);
            for (;;) __asm__ volatile("hlt");   /* no table: no GUI at all */
        }
    }
    memset(windows, 0, (size_t)GUI_MAX_WINDOWS * sizeof(gui_win_t));
    last_drawn_mx = -1;
    last_drawn_my = -1;
    last_drawn_valid = 0;
    memset(drop_epoch, 0, sizeof(drop_epoch));
    memset(window_epoch, 0, sizeof(window_epoch));
    spinlock_init(&gui_lock);
    spinlock_init(&gui_evt_lock);
    wq_init(&gui_wq);
    spinlock_init(&gui_wake_lock);
    gui_pending = 1;                /* first frame draws without a poke */
    gui_wq_ready = 1;
    focused = -1;
    key_focus = -1;
    cursor  = GUI_CURSOR_ARROW;
    active_theme = default_theme;
    gui_clock_base_ticks = timer_get_ticks();
    gui_clock_last_second = (uint64_t)-1;
    full_dirty = 1;
    gui_dirty_reset();

    gui_shell_init();
}

/* ===================================================================
 * Z-order and focus
 * =================================================================== */

/* WR-2 gap 2: the window that key events go to.  A child key_focus wins
 * only while it is still live and visible; otherwise fall back to the
 * top-level WM focus.  Returning `focused` keeps every pre-existing caller's
 * behaviour bit-identical when no child has taken the keyboard. */
static int gui_key_target(void) {
    if (key_focus >= 0 && key_focus < GUI_MAX_WINDOWS &&
        windows[key_focus].in_use && windows[key_focus].visible &&
        !windows[key_focus].minimized)
        return key_focus;
    return focused;
}

static void recompute_focus(void) {
    int top = -1, topz = -1;
    /* Always-on-top windows have priority. */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible || windows[i].minimized)
            continue;
        if (windows[i].flags & GUI_WIN_NO_DECOR) continue;
        int prio = (windows[i].flags & GUI_WIN_ALWAYS_TOP) ? 0x10000 : 0;
        if (windows[i].z + prio > topz) {
            topz = windows[i].z + prio;
            top = i;
        }
    }
    if (top != focused) {
        if (focused >= 0 && windows[focused].in_use) {
            windows[focused].focused = 0;
            gui_event_t e = { GUI_EVT_BLUR, 0,0,0,0,0,0 };
            gui_post_event(focused, &e);
        }
        focused = top;
        /* A new top-level window takes the keyboard with it: a child of the
         * window that just lost focus must not keep receiving keys. */
        key_focus = top;
        if (focused >= 0) {
            windows[focused].focused = 1;
            gui_event_t e = { GUI_EVT_FOCUS, 0,0,0,0,0,0 };
            gui_post_event(focused, &e);
        }
    }
}

int gui_raise_window(int wid) {
    if (!win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    int maxz = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].in_use && windows[i].z > maxz) maxz = windows[i].z;
    windows[wid].z = maxz + 1;
    recompute_focus();
    mark_window_dirty(&windows[wid]);
    spinlock_release(&gui_lock);
    return 0;
}

int gui_focus_window(int wid) {
    if (!win_alive(wid)) return -1;
    /* WR-2 gap 2: SetFocus on a CHILD must move the keyboard, not the
     * z-order.  A child carries GUI_WIN_NO_DECOR, so recompute_focus() would
     * never elect it; raising it instead would leave key_focus on the top
     * level and the child's control would still not see WM_KEYDOWN. */
    if (win_has_parent(&windows[wid]) || (windows[wid].flags & GUI_WIN_NO_DECOR)) {
        gui_event_t fe = { GUI_EVT_FOCUS, 0,0,0,0,0,0 };
        /* WR-4 F24 (4450da7 port): a top-level popup (the PuTTY config
         * box is parent-less and NO_DECOR) must not steal keyboard
         * focus from a live visible descendant that already holds it.
         * Without this, focusing the popup orphaned the focused edit
         * and typed keys went nowhere (4450da7's win_has_parent
         * takes a window pointer, not an id). */
        if (!win_has_parent(&windows[wid])) {
            int kf = key_focus;
            if (kf >= 0 && kf < GUI_MAX_WINDOWS &&
                windows[kf].in_use && windows[kf].parent == wid &&
                windows[kf].visible && !windows[kf].minimized) {
                gui_post_event(wid, &fe);
                return 0;
            }
        }
        key_focus = wid;
        gui_post_event(wid, &fe);
        return 0;
    }
    return gui_raise_window(wid);
}

/* ---- W32A-5: Z-order, flags, capture and metrics ------------------------ */

/* To the bottom of the stack.  Z values are ordinary ints that only ever
 * grow (raise takes max+1), so lowering means min-1; the renormalisation
 * below keeps them from drifting negative forever, which matters because
 * the compositor sorts on them. */
int gui_lower_window(int wid) {
    if (!win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    int minz = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].in_use && windows[i].z < minz) minz = windows[i].z;
    windows[wid].z = minz - 1;
    if (minz <= -0x40000000) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++)
            if (windows[i].in_use) windows[i].z -= minz;
    }
    recompute_focus();
    mark_window_dirty(&windows[wid]);
    spinlock_release(&gui_lock);
    return 0;
}

int gui_set_window_flags(int wid, uint32_t flags) {
    if (!win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    windows[wid].flags = flags;
    mark_window_dirty(&windows[wid]);
    full_dirty = 1;
    spinlock_release(&gui_lock);
    return 0;
}

int gui_get_window_z(int wid) {
    if (!win_alive(wid)) return -1;
    return windows[wid].z;
}

/* CW-1: establish (or clear, with parent_wid < 0) a parent/child link.
 * The child's stored x/y are henceforth interpreted as parent-relative
 * (relative to the parent's content origin).  Ownership is enforced by the
 * syscall layer; here we only reject cycles and cross-window nonsense. */
int gui_set_parent(int wid, int parent_wid) {
    if (!win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    if (parent_wid < 0) {
        windows[wid].parent = -1;
        mark_window_dirty(&windows[wid]);
        full_dirty = 1;
        spinlock_release(&gui_lock);
        return 0;
    }
    if (parent_wid == wid || !win_alive(parent_wid)) {
        spinlock_release(&gui_lock);
        return -1;
    }
    /* Reject a cycle: parent_wid must not be a descendant of wid. */
    for (int p = parent_wid, guard = 0;
         p >= 0 && guard < GUI_MAX_WINDOWS; guard++) {
        if (p == wid) { spinlock_release(&gui_lock); return -1; }
        p = windows[p].in_use ? windows[p].parent : -1;
    }
    windows[wid].parent = parent_wid;
    /* A child keeps its parent directly above it: give it the parent's z so
     * the hierarchical compositor draws it right after the parent. */
    windows[wid].z = windows[parent_wid].z;
    mark_window_dirty(&windows[wid]);
    full_dirty = 1;
    spinlock_release(&gui_lock);
    return 0;
}

/* The topmost visible window.  Used for GetForegroundWindow's honesty note:
 * the personality reports the focused window, and this is how a caller can
 * see the compositor's own answer without guessing. */
int gui_top_window(void) {
    int best = -1, bestz = 0x80000000;
    spinlock_acquire(&gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible || windows[i].minimized) continue;
        if (best < 0 || windows[i].z > bestz) { best = i; bestz = windows[i].z; }
    }
    spinlock_release(&gui_lock);
    return best;
}

int gui_set_capture(int wid) {
    if (wid >= 0 && !win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    int prev = captured;
    captured = wid;
    spinlock_release(&gui_lock);
    return prev;
}

int gui_get_capture(void) { return captured; }

uint32_t gui_screen_width(void)  { return gfx_get_width(); }
uint32_t gui_screen_height(void) { return gfx_get_height(); }

int gui_focused_window(void) { return focused; }

/* Pointer position, in the driver's own convention: 1 when the mouse driver
 * is up, 0 when there is no pointer on this lane, -1 for bad arguments.
 * GUI_OP_GET_MOUSE forwards it, so the syscall layer's success test is
 * `== 1` rather than `!= 0`. */
int gui_mouse_position(int32_t *x, int32_t *y) {
    if (!x || !y) return -1;
    return mouse_get_position((int *)x, (int *)y);
}

/* A decoration close action should make the window disappear immediately.
 * User-owned applications still receive GUI_EVT_CLOSE_REQ first, so cooperative
 * apps can run their normal shutdown path and destroy the already-hidden
 * window.  Kernel/demo windows have no userspace event loop; hiding them here
 * is what makes the [X] button visibly work for every window. */
static void request_window_close(int wid) {
    if (!win_alive(wid)) return;

    gui_event_t e = { GUI_EVT_CLOSE_REQ, 0,0,0,0,0,0 };
    gui_post_event(wid, &e);

    gui_win_t *w = &windows[wid];
    mark_window_dirty(w);
    w->visible = 0;
    w->minimized = 0;
    if (focused == wid) focused = -1;
    if (key_focus == wid) key_focus = -1;   /* WR-2 gap 2 */
    if (drag_wid == wid) { drag_wid = -1; drag_mode = 0; }
    if (last_hover_wid == wid) last_hover_wid = -1;
    recompute_focus();
    full_dirty = 1;
}

/* ===================================================================
 * Window lifecycle
 * =================================================================== */

int gui_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                      const char *title, uint32_t flags) {
    if (w == 0 || h == 0) return -1;

    /* Pre-compute content size to reject before allocating a slot. */
    uint32_t bw, bh;
    if (flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) {
        bw = w; bh = h;
    } else {
        uint32_t decor_w = 2 * active_theme.border_w + 2 * GUI_CONTENT_PAD;
        uint32_t decor_h = active_theme.titlebar_h + 2 * active_theme.border_w
                        + 2 * GUI_CONTENT_PAD;
        if (w <= decor_w || h <= decor_h) return -1;
        bw = w - decor_w;
        bh = h - decor_h;
    }
    if (bw == 0 || bh == 0) return -1;

    spinlock_acquire(&gui_lock);
    int id = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) { id = i; break; }
    }
    if (id < 0) {
        kprintf("[gui] create_window: no free slots (%d windows)\n", GUI_MAX_WINDOWS);
        spinlock_release(&gui_lock);
        return -1;
    }

    gui_win_t *win = &windows[id];
    tcb_t *owner = sched_current();
    spinlock_acquire(&gui_evt_lock);
    if (++window_epoch[id] == 0) window_epoch[id] = 1;
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->flags = flags;
    win->snap = GUI_SNAP_NONE;
    win->parent = -1;               /* CW-1: top-level until reparented */
    win->owner_pid = owner ? (int)owner->id : 0;
    win->in_use = 1;
    spinlock_release(&gui_evt_lock);
    win->visible = 0;
    win->minimized = 0;
    win->maximized = 0;
    win->z = id;
    win->content_dirty = 1;
    if (title) {
        strncpy(win->title, title, GUI_TITLE_MAX - 1);
        win->title[GUI_TITLE_MAX - 1] = 0;
    }
    win->back_w = bw;
    win->back_h = bh;
    size_t buf_size = (size_t)bw * bh * 4;
    if (buf_size / 4 != (size_t)bw * bh) {
        /* Integer overflow — reject impossibly large window. */
        spinlock_acquire(&gui_evt_lock);
        win->in_use = 0;
        spinlock_release(&gui_evt_lock);
        kprintf("[gui] create_window: buffer size overflow %ux%u\n", bw, bh);
        spinlock_release(&gui_lock);
        return -1;
    }
    win->back = (uint32_t *)kmalloc(buf_size);
    if (!win->back) {
        spinlock_acquire(&gui_evt_lock);
        win->in_use = 0;
        spinlock_release(&gui_evt_lock);
        kprintf("[gui] create_window: kmalloc failed for %ux%u back buffer\n", bw, bh);
        spinlock_release(&gui_lock);
        return -1;
    }
    size_t pixels = (size_t)bw * (size_t)bh;
    gui_fill32(win->back, active_theme.win_content, pixels);
    spinlock_release(&gui_lock);
    return id;
}

/* CW-1: free one window slot and clear any WM state that referenced it.
 * Caller holds gui_lock. */
static void gui_free_slot_locked(int wid) {
    gui_win_t *w = &windows[wid];
    mark_window_dirty(w);
    if (w->back) kfree(w->back);
    if (w->front) { kfree(w->front); w->front = NULL; }
    spinlock_acquire(&gui_evt_lock);
    memset(w, 0, sizeof(*w));
    spinlock_release(&gui_evt_lock);
    w->parent = -1;                         /* memset left it 0 (a valid slot) */
    if (focused == wid) focused = -1;
    if (key_focus == wid) key_focus = -1;   /* WR-2 gap 2 */
    if (captured == wid) captured = -1;     /* W32A-5 */
    if (drag_wid == wid) { drag_wid = -1; drag_mode = 0; }
    if (last_hover_wid == wid) last_hover_wid = -1;
}

/* CW-1: destroy a window and every descendant (Win32 destroys children with
 * their parent).  Caller holds gui_lock. */
static void gui_destroy_subtree_locked(int wid) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].parent == wid)
            gui_destroy_subtree_locked(i);
    }
    gui_free_slot_locked(wid);
}

int gui_destroy_window(int wid, uint64_t owner_pid) {
    spinlock_acquire(&gui_lock);
    /* The syscall's earlier require_owner is only a snapshot: another
     * process could recycle this slot before we acquire gui_lock. */
    if (!owner_pid || !win_alive(wid) ||
        (uint32_t)windows[wid].owner_pid != owner_pid) {
        spinlock_release(&gui_lock);
        return -1;
    }
    gui_destroy_subtree_locked(wid);
    recompute_focus();
    full_dirty = 1;
    spinlock_release(&gui_lock);
    /* Anyone parked in gui_wait_event must re-check win_alive; a pending
     * token also dies with the slot rather than waking a recycled HWND. */
    wq_wake_all(&gui_evt_wq);
    return 0;
}

int gui_window_owned_by(int wid, uint64_t owner_pid) {
    if (!win_alive(wid)) return 0;
    return (uint64_t)(uint32_t)windows[wid].owner_pid == owner_pid;
}

uint64_t gui_window_owner(int wid) {
    if (!win_alive(wid)) return (uint64_t)-1;
    return (uint64_t)(uint32_t)windows[wid].owner_pid;
}

/* RESIDUE2 T7 (clipboard ACL): does this pid own at least one live window?
 *
 * The GUI permission model draws one line: per-window ops require owning
 * THAT window (require_owner in gui_syscalls.c), and mutations of
 * GUI-GLOBAL state -- the kernel clipboard, the desktop theme, taskbar
 * notifications, desktop icons -- require being a GUI participant at all,
 * i.e. owning at least one window.  A windowless background process has
 * no business scraping the clipboard or restyling the desktop underneath
 * the user's windows.
 */
int gui_pid_has_windows(uint64_t owner_pid) {
    if (owner_pid == 0) return 0;
    if (!windows) return 0;   /* before gui_init(): nobody owns windows */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (gui_window_owned_by(i, owner_pid))
            return 1;
    }
    return 0;
}

void gui_cleanup_process(uint64_t owner_pid) {
    if (owner_pid == 0) return;
    /* The window table is heap-allocated in gui_init(); thread teardown
     * runs this for EVERY exiting thread -- including early self-test
     * threads that die before the GUI exists.  Walking the NULL table
     * from those exits faulted inside the frame of whatever the boot
     * happened to be doing and hung the SMP bringup dead. */
    if (!windows) return;
    int cleaned = 0;
    spinlock_acquire(&gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        if ((uint64_t)(uint32_t)windows[i].owner_pid != owner_pid) continue;
        mark_window_dirty(&windows[i]);
        if (windows[i].back) kfree(windows[i].back);
        spinlock_acquire(&gui_evt_lock);
        memset(&windows[i], 0, sizeof(windows[i]));
        spinlock_release(&gui_evt_lock);
        if (focused == i) focused = -1;
        if (key_focus == i) key_focus = -1;   /* WR-2 gap 2 */
        if (captured == i) captured = -1;
        if (drag_wid == i) { drag_wid = -1; drag_mode = 0; }
        if (last_hover_wid == i) last_hover_wid = -1;
        cleaned++;
    }

    shell_cleanup_icons(owner_pid);

    if (cleaned) {
        recompute_focus();
        full_dirty = 1;
    }
    spinlock_release(&gui_lock);
    if (cleaned) {
        wq_wake_all(&gui_evt_wq);
        kprintf("[gui] cleaned %d window(s) for pid %llu\n",
                cleaned, (unsigned long long)owner_pid);
    }
}

/* ---- Show / hide / move / resize ---- */

int gui_show_window(int wid) {
    if (!win_alive(wid)) return -1;
    windows[wid].visible = 1;
    windows[wid].minimized = 0;
    gui_raise_window(wid);
    return 0;
}

int gui_hide_window(int wid) {
    if (!win_alive(wid)) return -1;
    windows[wid].visible = 0;
    mark_window_dirty(&windows[wid]);
    if (focused == wid) { focused = -1; recompute_focus(); }
    if (key_focus == wid) key_focus = -1;   /* WR-2 gap 2 */
    full_dirty = 1;
    return 0;
}

int gui_move_window(int wid, int32_t x, int32_t y) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    mark_window_dirty(w);
    w->x = x; w->y = y;
    mark_window_dirty(w);
    return 0;
}

int gui_resize_window(int wid, uint32_t w, uint32_t h) {
    if (!win_alive(wid)) return -1;
    /* The 60x40 floor keeps a user-draggable, decorated top-level window from
     * collapsing under the mouse.  Borderless child controls (a toolbar or
     * rebar band) legitimately auto-size to a single ~24px row, so they must
     * not be held to the top-level floor -- otherwise the control stays at its
     * oversized creation default and the app lays panes out beneath a giant
     * phantom toolbar. */
    int borderless = (windows[wid].flags &
                      (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) != 0;
    uint32_t min_w = borderless ? 1u : 60u;
    uint32_t min_h = borderless ? 1u : 40u;
    if (w < min_w || h < min_h) return -1;
    spinlock_acquire(&gui_lock);
    gui_win_t *win = &windows[wid];
    /* Compute new content size BEFORE modifying the window. */
    uint32_t new_bw, new_bh;
    if (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) {
        new_bw = w; new_bh = h;
    } else {
        uint32_t decor_w = 2 * active_theme.border_w + 2 * GUI_CONTENT_PAD;
        uint32_t decor_h = active_theme.titlebar_h + 2 * active_theme.border_w
                        + 2 * GUI_CONTENT_PAD;
        if (w <= decor_w || h <= decor_h) {
            spinlock_release(&gui_lock);
            return -1;
        }
        new_bw = w - decor_w;
        new_bh = h - decor_h;
    }
    if (new_bw == 0 || new_bh == 0) {
        spinlock_release(&gui_lock);
        return -1;
    }
    mark_window_dirty(win);

    /* FIX (resize-allocation): keep the existing back buffer whenever it
     * already covers the requested content size.  Shrinking a window must
     * not allocate at all (previously every drag-resize mouse event did
     * kmalloc + kfree + a full multi-MiB memset, and the kernel heap never
     * returns pages to the PMM, so the peak stuck around forever).  Growing
     * inside the current allocation is also free.  Only growth beyond the
     * current allocation reallocates, rounded up to a 64 px grid so a
     * drag-resize pays for one allocation per 64 px of travel instead of
     * one per mouse event.  back_w/back_h now mean the ALLOCATED buffer
     * size; the logical content size is content_w()/content_h(), which is
     * what gui_get_window_size() and the compositor clip to. */
    if (new_bw > win->back_w || new_bh > win->back_h) {
        uint32_t alloc_w = (new_bw + 63u) & ~63u;
        uint32_t alloc_h = (new_bh + 63u) & ~63u;
        size_t buf_size = (size_t)alloc_w * alloc_h * 4;
        if (buf_size / 4 != (size_t)alloc_w * alloc_h) {
        kprintf("[gui] resize_window: buffer size overflow %ux%u\n", alloc_w, alloc_h);
        spinlock_release(&gui_lock);
        return -1;
    }
        uint32_t *nb = (uint32_t *)kmalloc(buf_size);
        if (!nb) {
        kprintf("[gui] resize_window: kmalloc failed for %ux%u buffer\n", alloc_w, alloc_h);
        spinlock_release(&gui_lock);
        return -1;
    }

        size_t pixels = (size_t)alloc_w * (size_t)alloc_h;

        gui_fill32(nb, active_theme.win_content, pixels);
        /* Copy overlapping region with the OLD row stride. */
        uint32_t cw = new_bw < win->back_w ? new_bw : win->back_w;
        uint32_t ch = new_bh < win->back_h ? new_bh : win->back_h;
        for (uint32_t row = 0; row < ch; row++) {
            gui_copy32(
                nb + (size_t)row * alloc_w,
                win->back + (size_t)row * win->back_w,
                cw
            );
        }
        if (win->back) kfree(win->back);
        win->back   = nb;
        win->back_w = alloc_w;
        win->back_h = alloc_h;
        /* The front buffer was sized for the OLD dimensions; keeping it
         * would let the compositor read past its end on the very next
         * tick.  Drop it and let gui_frame_begin() reallocate at the new
         * size -- one frame falls back to the live buffer, which is
         * exactly the pre-existing behaviour. */
        if (win->front) { kfree(win->front); win->front = NULL; }
    }
    win->w = w; win->h = h;
    win->content_dirty = 1;
    mark_window_dirty(win);
    /* Post resize event. */
    gui_event_t e = { GUI_EVT_RESIZE, 0,0,0,0,0,0 };
    e.x = (int32_t)new_bw; e.y = (int32_t)new_bh;
    spinlock_release(&gui_lock);
    gui_post_event(wid, &e);
    return 0;
}

int gui_set_title(int wid, const char *title) {
    if (!win_alive(wid)) return -1;
    if (title) {
        strncpy(windows[wid].title, title, GUI_TITLE_MAX - 1);
        windows[wid].title[GUI_TITLE_MAX - 1] = 0;
    } else {
        windows[wid].title[0] = 0;
    }
    mark_window_dirty(&windows[wid]);
    return 0;
}

int gui_minimize_window(int wid) {
    if (!win_alive(wid)) return -1;
    windows[wid].minimized = 1;
    mark_window_dirty(&windows[wid]);
    if (focused == wid) { focused = -1; recompute_focus(); }
    if (key_focus == wid) key_focus = -1;   /* WR-2 gap 2 */
    full_dirty = 1;
    return 0;
}

int gui_maximize_window(int wid) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    if (w->maximized) return 0;
    mark_window_dirty(w);
    w->restore_x = w->x; w->restore_y = w->y;
    w->restore_w = w->w; w->restore_h = w->h;
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height() - active_theme.taskbar_h;
    w->x = 0; w->y = 0;
    gui_resize_window(wid, fw, fh);
    w->maximized = 1;
    w->snap = GUI_SNAP_MAXIMIZED;
    mark_window_dirty(w);
    full_dirty = 1;
    return 0;
}

int gui_restore_window(int wid) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    if (!w->maximized && w->snap == GUI_SNAP_NONE) {
        w->minimized = 0;
        full_dirty = 1;
        return 0;
    }
    mark_window_dirty(w);
    w->x = w->restore_x; w->y = w->restore_y;
    gui_resize_window(wid, w->restore_w, w->restore_h);
    w->maximized = 0;
    w->minimized = 0;
    w->snap = GUI_SNAP_NONE;
    mark_window_dirty(w);
    full_dirty = 1;
    return 0;
}

int gui_snap_window(int wid, gui_snap_t snap) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    mark_window_dirty(w);
    /* Save restore position if not already in a snap. */
    if (w->snap == GUI_SNAP_NONE && !w->maximized) {
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
    }
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height() - active_theme.taskbar_h;
    switch (snap) {
        case GUI_SNAP_NONE:
            w->x = w->restore_x; w->y = w->restore_y;
            gui_resize_window(wid, w->restore_w, w->restore_h);
            break;
        case GUI_SNAP_LEFT:
            w->x = 0; w->y = 0;
            gui_resize_window(wid, fw / 2, fh);
            break;
        case GUI_SNAP_RIGHT:
            w->x = (int32_t)(fw / 2); w->y = 0;
            gui_resize_window(wid, fw / 2, fh);
            break;
        case GUI_SNAP_TOP:
            w->x = 0; w->y = 0;
            gui_resize_window(wid, fw, fh / 2);
            break;
        case GUI_SNAP_BOTTOM:
            w->x = 0; w->y = (int32_t)(fh / 2);
            gui_resize_window(wid, fw, fh / 2);
            break;
        case GUI_SNAP_MAXIMIZED:
            w->x = 0; w->y = 0;
            gui_resize_window(wid, fw, fh);
            w->maximized = 1;
            break;
    }
    w->snap = snap;
    w->content_dirty = 1;
    mark_window_dirty(w);
    full_dirty = 1;
    /* Notify the client. */
    gui_event_t e = { GUI_EVT_SNAP_CHANGED, 0,0,0,0,0, (uint16_t)snap };
    gui_post_event(wid, &e);
    return 0;
}

/* ---- Geometry queries ---- */

int gui_get_window_size(int wid, uint32_t *w, uint32_t *h) {
    if (!win_alive(wid)) return -1;
    /* FIX: report the LOGICAL content size (content_w/content_h), not the
     * allocated buffer size.  After the resize fix the buffer can be larger
     * than the window (it is kept on shrink), so back_w/back_h no longer
     * equal what applications should draw into. */
    if (w) *w = content_w(&windows[wid]);
    if (h) *h = content_h(&windows[wid]);
    return 0;
}

int gui_get_window_pos(int wid, int32_t *x, int32_t *y) {
    if (!win_alive(wid)) return -1;
    if (x) *x = windows[wid].x;
    if (y) *y = windows[wid].y;
    return 0;
}

int gui_get_window_rect(int wid, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (!win_alive(wid)) return -1;
    if (x) *x = windows[wid].x;
    if (y) *y = windows[wid].y;
    if (w) *w = windows[wid].w;
    if (h) *h = windows[wid].h;
    return 0;
}

/* ---- Window flags ---- */

uint32_t gui_get_window_flags(int wid) {
    if (!win_alive(wid)) return 0;
    return windows[wid].flags;
}

/* ---- Back buffer access ---- */

uint32_t *gui_window_buffer(int wid, uint32_t *out_pitch) {
    if (!win_alive(wid)) return NULL;
    if (out_pitch) *out_pitch = windows[wid].back_w;
    return windows[wid].back;
}

/* ---- Invalidation ---- */

int gui_invalidate_window(int wid) {
    if (!win_alive(wid)) return -1;
    windows[wid].content_dirty = 1;
    mark_window_dirty(&windows[wid]);
    /* The compositor sleeps until something pokes it (O4).  A syscall that
     * dirties a window is exactly such a something: without this the frame
     * the client just drew stays invisible until an unrelated IRQ (a mouse
     * move, a key press) or the 1 Hz clock line happens to wake the
     * compositor.  Reproduced live: the glrunner demo renders in a tight
     * loop but the desktop only refreshed one frame per input event, so the
     * game looked frozen and its score never advanced. */
    gui_poke();
    return 0;
}

int gui_invalidate_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!win_alive(wid)) return -1;
    windows[wid].content_dirty = 1;
    /* Translate content-local rect to screen coords. */
    gui_win_t *win = &windows[wid];
    int32_t sx = content_x(win) + x;
    int32_t sy = content_y(win) + y;
    gui_mark_dirty(sx, sy, w, h);
    gui_poke();   /* same wake-up contract as gui_invalidate_window() */
    return 0;
}

void gui_request_redraw(void) {
    full_dirty = 1;
    gui_dirty_reset();
}

void gui_set_cursor(gui_cursor_t c) {
    if (cursor == c) return;

    cursor = c;

    /* A cursor shape can change without pointer movement.  Invalidate its
     * current footprint so the new shape is composed immediately. */
    int mx = -1, my = -1;
    if (mouse_get_position(&mx, &my) == 1) {
        gui_mark_cursor_dirty(mx, my);
    } else if (last_drawn_valid) {
        gui_mark_cursor_dirty(last_drawn_mx, last_drawn_my);
    }

    gui_poke();
}

/* ===================================================================
 * C API compatibility wrappers for the Rust drawing core
 *
 * Window validation and content-dirty bookkeeping stay in C. Pixel
 * operations, clipping, blending, blitting, and bulk fills/copies are
 * implemented by kernel/gui/gui_drawing.rs through the C ABI.
 * =================================================================== */

int gui_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_draw_pixel(w->back, w->back_w, w->back_h,
                                     w->back_w, x, y, color);
    if (result == 0 && x >= 0 && y >= 0 &&
        (uint32_t)x < w->back_w && (uint32_t)y < w->back_h)
        w->content_dirty = 1;
    return result;
}


int gui_get_pixel(int wid, int32_t x, int32_t y, uint32_t *out) {
    if (!out || !win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    return gui_rust_get_pixel(w->back, w->back_w, w->back_h,
                              w->back_w, x, y, out);
}


int gui_font_metrics(uint32_t *width, uint32_t *height,
                     uint32_t *ascent, uint32_t *descent,
                     uint32_t *num_glyphs) {
    const struct psf_font *f = psf_get_font();
    if (!f || f->magic != PSF2_MAGIC || f->width == 0 || f->height == 0) return -1;
    if (width)       *width       = f->width;
    if (height)      *height      = f->height;
    /* The shipped VGA face keeps two descender rows (g/j/p/q/y); there is
     * no baseline record in PSF2, so the split is measured, not guessed. */
    if (ascent)      *ascent      = f->height - 2;
    if (descent)     *descent     = 2;
    if (num_glyphs)  *num_glyphs  = f->num_glyphs;
    return 0;
}

/* Blend a single pixel with per-pixel alpha (0=transparent, 255=opaque).
 * This is the primitive every soft-edge renderer builds on: SDF text,
 * rounded corners, shadows.  Same formula as gui_blit_alpha, but a single
 * pixel and no source pointer, so renderers can call it directly.
 * Returns 0 on success (including out-of-bounds clip), -1 if wid is dead. */
int gui_blend_pixel(int wid, int32_t x, int32_t y,
                    uint32_t color, uint8_t a) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_blend_pixel(w->back, w->back_w, w->back_h,
                                      w->back_w, x, y, color, a);
    if (result == 0 && a != 0 && x >= 0 && y >= 0 &&
        (uint32_t)x < w->back_w && (uint32_t)y < w->back_h)
        w->content_dirty = 1;
    return result;
}


/*
 * Blend four consecutive pixels.
 *
 * Intended for MSDF/SDF rendering, where four adjacent pixels
 * have independently calculated alpha values but share one color.
 *
 * x is the first pixel coordinate.
 * alpha points to exactly four alpha values.
 */
int gui_blend_pixels4(int wid, int32_t x, int32_t y,
                      uint32_t color, const uint8_t alpha[4]) {
    if (!win_alive(wid) || !alpha) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_blend_pixels4(w->back, w->back_w, w->back_h,
                                        w->back_w, x, y, color, alpha);
    int touched = 0;
    if (result == 0 && y >= 0 && (uint32_t)y < w->back_h) {
        for (int i = 0; i < 4; ++i) {
            int64_t px = (int64_t)x + i;
            if (alpha[i] != 0 && px >= 0 && px < (int64_t)w->back_w) {
                touched = 1;
                break;
            }
        }
    }
    if (touched) w->content_dirty = 1;
    return result;
}



int gui_fill_rect(int wid, int32_t x, int32_t y,
                  uint32_t W, uint32_t H, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_fill_rect(w->back, w->back_w, w->back_h,
                                    w->back_w, x, y, W, H, color);
    if (result == 0 && W != 0 && H != 0 &&
        (int64_t)x < (int64_t)w->back_w &&
        (int64_t)y < (int64_t)w->back_h &&
        (int64_t)x + (int64_t)W > 0 &&
        (int64_t)y + (int64_t)H > 0)
        w->content_dirty = 1;
    return result;
}


int gui_draw_rect(int wid, int32_t x, int32_t y,
                  uint32_t W, uint32_t H, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_draw_rect(w->back, w->back_w, w->back_h,
                                    w->back_w, x, y, W, H, color);
    if (result == 0 && W != 0 && H != 0 &&
        (int64_t)x < (int64_t)w->back_w &&
        (int64_t)y < (int64_t)w->back_h &&
        (int64_t)x + (int64_t)W > 0 &&
        (int64_t)y + (int64_t)H > 0)
        w->content_dirty = 1;
    return result;
}


int gui_draw_line(int wid, int32_t x0, int32_t y0,
                  int32_t x1, int32_t y1, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_draw_line(w->back, w->back_w, w->back_h,
                                    w->back_w, x0, y0, x1, y1, color);
    if (result == 0) w->content_dirty = 1;
    return result;
}


/* Client text goes through the kernel-space Rust SDF renderer.  The
 * compositor's own chrome (taskbar, titlebar, notifications, start menu,
 * clock) still uses gfx_draw_text -- PSF 8x16 -- because it draws into the
 * front framebuffer, not a window back-buffer, and the SDF module only
 * knows how to composite into a window.  The 16 px default is twice the
 * PSF cell height and matches the Inter atlas's design size. */
int gui_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color) {
    if (!win_alive(wid)) return -1;
    return sdf_draw_text(wid, x, y, s, color, 16);
}

int gui_clear(int wid, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_clear(w->back, w->back_w, w->back_h,
                                w->back_w, color);
    if (result == 0) w->content_dirty = 1;
    return result;
}


/* Frame brackets.
 *
 * The blit syscall copies user pixels through a one-row bounce buffer and
 * therefore calls gui_blit() once PER ROW -- 400 times for a 640x400 frame.
 * Marking the window busy inside gui_blit() would raise and clear the flag
 * 400 times, and the compositor would simply land in one of the 399 gaps.
 * The bracket has to span the whole frame, so the syscall wrapper owns it. */
void gui_frame_begin(int wid) {
    spinlock_acquire(&gui_lock);

    if (!win_alive(wid)) {
        spinlock_release(&gui_lock);
        return;
    }

    gui_win_t *w = &windows[wid];

    if (!w->front && w->back) {
        size_t pixels = (size_t)w->back_w * w->back_h;

        uint32_t *front = (uint32_t *)kmalloc(
            (uint64_t)pixels * sizeof(uint32_t)
        );

        if (front) {
            gui_copy32(front, w->back, pixels);

            /* Publish only after the initial contents are ready. */
            w->front = front;
        }
    }

    w->updating = 1;

    spinlock_release(&gui_lock);
}

void gui_frame_end(int wid) {
    spinlock_acquire(&gui_lock);

    if (!win_alive(wid)) {
        spinlock_release(&gui_lock);
        return;
    }

    gui_win_t *w = &windows[wid];

    if (w->front && w->back) {
        gui_copy32(
            w->front,
            w->back,
            (size_t)w->back_w * w->back_h
        );
    }

    w->updating = 0;
    w->content_dirty = 1;

    spinlock_release(&gui_lock);
}

int gui_blit(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H,
             const uint32_t *src, uint32_t src_stride) {
    if (!win_alive(wid) || !src) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_blit(w->back, w->back_w, w->back_h, w->back_w,
                               x, y, W, H, src, src_stride);
    if (result == 0 && W != 0 && H != 0) w->content_dirty = 1;
    return result;
}


int gui_blit_alpha(int wid, int32_t x, int32_t y,
                   uint32_t W, uint32_t H, const uint32_t *src,
                   uint32_t src_stride) {
    if (!win_alive(wid) || !src) return -1;
    gui_win_t *w = &windows[wid];
    int result = gui_rust_blit_alpha(w->back, w->back_w, w->back_h,
                                     w->back_w, x, y, W, H,
                                     src, src_stride);
    if (result == 0 && W != 0 && H != 0) w->content_dirty = 1;
    return result;
}


/* WR-3: store (or clear) the see-through hole.  The rect is clamped into
 * the live buffer so a stale oversized hole cannot outlive a shrink, and
 * the window is dirtied: pixels that used to show must be recomposited
 * away (or back) on the next tick. */
void gui_window_set_hole(int wid, int32_t l, int32_t t, int32_t r, int32_t b) {
    if (!win_alive(wid)) return;
    gui_win_t *w = &windows[wid];
    if (r > l && b > t) {
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (int32_t)w->back_w) r = (int32_t)w->back_w;
        if (b > (int32_t)w->back_h) b = (int32_t)w->back_h;
    }
    if (w->hole_l == l && w->hole_t == t && w->hole_r == r && w->hole_b == b)
        return;
    w->hole_l = l; w->hole_t = t; w->hole_r = r; w->hole_b = b;
    w->content_dirty = 1;
    mark_window_dirty(w);
}

/* ===================================================================
 * Event ring
 * =================================================================== */

static void drop_release_token(gui_win_t *w, uint16_t token) {
    unsigned slot = token & 15u;
    if (!slot || slot > GUI_DROP_SLOTS) return;
    --slot;
    if (w->drop_busy[slot] && w->drop_generation[slot] == (token >> 4))
        w->drop_busy[slot] = 0;
}
/* gui_evt_lock held by the caller. Eviction also frees a pending path. */
static void gui_post_event_locked(gui_win_t *w, const gui_event_t *evt) {
    uint32_t next = (w->evt_head + 1) % GUI_EVT_RING_SIZE;
    if (next == w->evt_tail) {
        const gui_event_t *old = &w->events[w->evt_tail];
        if (old->type == GUI_EVT_DROP) drop_release_token(w, old->data);
        w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    }
    w->events[w->evt_head] = *evt;
    w->evt_head = next;
}
int gui_post_event(int wid, const gui_event_t *evt) {
    if (!evt) return -1;
    spinlock_acquire(&gui_evt_lock);
    if (!win_alive(wid)) { spinlock_release(&gui_evt_lock); return -1; }
    gui_post_event_locked(&windows[wid], evt);
    spinlock_release(&gui_evt_lock);
    wq_wake_all(&gui_evt_wq);
    return 0;
}

/* gui_evt_lock held: the syscall's initial ownership check is NOT enough
 * when a window ID is destroyed/reused before an event is read. A blocking
 * waiter also pins this *lifetime*, not just the owner and integer slot. */
static int gui_poll_owned_event_locked(int wid, uint64_t owner_pid,
                                       uint32_t epoch, gui_event_t *out) {
    if (!out || !owner_pid || !win_alive(wid) ||
        (uint32_t)windows[wid].owner_pid != owner_pid ||
        (epoch && window_epoch[wid] != epoch)) return -1;
    gui_win_t *w = &windows[wid];
    if (w->evt_head == w->evt_tail) return 0;
    *out = w->events[w->evt_tail];
    w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    return 1;
}
int gui_poll_event(int wid, uint64_t owner_pid, gui_event_t *out) {
    spinlock_acquire(&gui_evt_lock);
    int result = gui_poll_owned_event_locked(wid, owner_pid, 0, out);
    spinlock_release(&gui_evt_lock);
    return result;
}

int gui_wait_event(int wid, uint64_t owner_pid, gui_event_t *out) {
    /* O7: block instead of yield-spinning; the 5-tick net covers a push
     * between our empty check and sleep. Destruction also wakes waiters. */
    spinlock_acquire(&gui_evt_lock);
    if (!out || !owner_pid || !win_alive(wid) ||
        (uint32_t)windows[wid].owner_pid != owner_pid) {
        spinlock_release(&gui_evt_lock);
        return -1;
    }
    uint32_t epoch = window_epoch[wid];
    spinlock_release(&gui_evt_lock);
    for (;;) {
        spinlock_acquire(&gui_evt_lock);
        int result = gui_poll_owned_event_locked(wid, owner_pid, epoch, out);
        spinlock_release(&gui_evt_lock);
        if (result) return result; /* event or destroyed/reused HWND */
        wq_wait_deadline(&gui_evt_wq, NULL, timer_get_ticks() + 5);
    }
}

/* ===================================================================
 * Compositor rendering
 * =================================================================== */

/* Implemented as concentric rounded rectangles drawn OUTSIDE the
 * window, one per layer.  Each layer is a full rectangle fill, but we
 * draw them largest-to-smallest, so the inner layers overwrite the
 * outer ones — the final visual is a smooth gradient from the desktop
 * (outermost) to the shadow's densest colour (innermost).*/
static void blit_window_shadow(const gui_win_t *win) {
    const gui_theme_t *t = &active_theme;
    uint32_t round = (win->flags & GUI_WIN_NO_DECOR) ? 0 : t->win_round;

    /* Desktop colour at the window's vertical centre. */
    uint32_t screen_h = gfx_get_height();
    int32_t  cy = win->y + (int32_t)win->h / 2;
    if (cy < 0) cy = 0;
    if (cy >= (int32_t)screen_h) cy = (int32_t)screen_h - 1;
    uint32_t fy = (screen_h > 1) ? (uint32_t)cy * 255u / (screen_h - 1) : 0;
    uint32_t top = t->desktop_top, bot = t->desktop_bot;
    uint32_t rt = (top >> 16) & 0xFF, gt = (top >>  8) & 0xFF, bt = top & 0xFF;
    uint32_t rb = (bot >> 16) & 0xFF, gb = (bot >>  8) & 0xFF, bb = bot & 0xFF;
    uint32_t r = (rt * (255 - fy) + rb * fy) / 255;
    uint32_t g = (gt * (255 - fy) + gb * fy) / 255;
    uint32_t b = (bt * (255 - fy) + bb * fy) / 255;
    uint32_t desktop = (r << 16) | (g << 8) | b;

    const int layers = 2;
    const int spread = 4;

    for (int i = layers - 1; i >= 0; i--) {
        /* i = 7 → outermost (largest, faintest)
         * i = 0 → innermost (smallest, densest) */
        int grow = (spread * (i + 1)) / layers;  /* 1..14 px outward */
        /* Alpha rises sharply toward the window edge, like a Gaussian. */
        uint32_t alpha = (uint32_t)(255 - (i * 220 / layers));
        if (alpha > 255) alpha = 255;

        uint32_t col = blend_u32(desktop, t->shadow_color, alpha);

        int32_t  sx = win->x - grow;
        int32_t  sy = win->y - grow;
        uint32_t sw = win->w + (uint32_t)(2 * grow);
        uint32_t sh = win->h + (uint32_t)(2 * grow);
        uint32_t sr = round + (uint32_t)grow;
        uint32_t lim = (sw < sh ? sw : sh) / 2;
        if (sr > lim) sr = lim;

        gfx_fill_round_rect((uint32_t)sx, (uint32_t)sy, sw, sh, sr, col);
    }
}

/* ---- Window decoration rendering ---- */

static void blit_window_decor(const gui_win_t *win) {
    const gui_theme_t *t = &active_theme;
    int active = win->focused;
    uint32_t bg    = t->win_bg;
    uint32_t textc = t->title_text;
    uint32_t brd   = c_border();
    uint32_t tbh   = c_titlebar();
    uint32_t pad   = GUI_CONTENT_PAD;
    uint32_t round = (win->flags & GUI_WIN_NO_DECOR) ? 0 : t->win_round;

    if (win->flags & GUI_WIN_NO_DECOR) return;

    /* Whole window background: frame + titlebar + padding around the
     * content card are the same colour.  No outline; the shadow (drawn
     * by blit_window_shadow before this function) separates the window
     * from the desktop. */
    if (round)
        gfx_fill_round_rect((uint32_t)win->x, (uint32_t)win->y,
                            win->w, win->h, round, bg);
    else
        gfx_fill_rect((uint32_t)win->x, (uint32_t)win->y,
                      win->w, win->h, bg);

    /* Title text. */
    {
        uint32_t fh = gui_font_h();
        uint32_t ty = (uint32_t)win->y + brd
                    + (tbh > fh ? (tbh - fh) / 2 : 0);
        gfx_draw_text((uint32_t)win->x + brd + 6, ty, win->title, textc);
    }

    /* Title-bar buttons: glyph only, no fill.  Hover recolours the glyph. */
    uint32_t btn_w = 18, btn_h = 16, btn_pad = 2;
    int mywid = (int)(win - windows);

    if (win->flags & GUI_WIN_HAS_CLOSE) {
        uint32_t bx = (uint32_t)win->x + win->w - brd - btn_w - btn_pad;
        uint32_t by = (uint32_t)win->y + brd + (tbh - btn_h) / 2;
        uint32_t glyph = (hover_btn_wid == mywid && hover_btn_kind == 1)
                       ? t->close_bg_hover : textc;
        gfx_draw_line(bx + 4, by + 3, bx + btn_w - 5, by + btn_h - 4, glyph);
        gfx_draw_line(bx + btn_w - 5, by + 3, bx + 4, by + btn_h - 4, glyph);
    }
    if (win->flags & GUI_WIN_HAS_MINMAX) {
        uint32_t bx = (uint32_t)win->x + win->w - brd - 2 * (btn_w + btn_pad);
        uint32_t by = (uint32_t)win->y + brd + (tbh - btn_h) / 2;
        uint32_t glyph = textc;
        if (hover_btn_wid == mywid && hover_btn_kind == 2) glyph = t->max_bg;
        if (win->maximized) {
            gfx_draw_rect(bx + 4, by + 3, 9, 9, glyph);
            gfx_draw_rect(bx + 7, by + 6, 9, 9, glyph);
        } else {
            gfx_draw_rect(bx + 4, by + 3, 10, 9, glyph);
        }
        uint32_t mx2 = bx - btn_w - btn_pad;
        uint32_t mglyph = (hover_btn_wid == mywid && hover_btn_kind == 3)
                        ? t->min_bg : textc;
        gfx_draw_line(mx2 + 4, by + btn_h / 2,
                      mx2 + btn_w - 5, by + btn_h / 2, mglyph);
    }

    /* Content "card": rounded background behind the client's pixels.
     * The radius is the outer radius shrunk by (border + pad), so the
     * card nests inside the window's rounded corners with `pad` pixels
     * of win_bg visible in every corner. */
    int32_t card_x = win->x + (int32_t)brd + (int32_t)pad;
    int32_t card_y = win->y + (int32_t)brd + (int32_t)tbh + (int32_t)pad;
    uint32_t card_w = win->w - 2 * brd - 2 * pad;
    uint32_t card_h = win->h - tbh - 2 * brd - 2 * pad;
    if (card_w > 0 && card_h > 0) {
        int32_t card_round = (int32_t)round - (int32_t)brd;
        if (card_round < 0) card_round = 0;
        if (card_round)
            gfx_fill_round_rect((uint32_t)card_x, (uint32_t)card_y,
                                card_w, card_h,
                                (uint32_t)card_round, t->win_content);
        else
            gfx_fill_rect((uint32_t)card_x, (uint32_t)card_y,
                          card_w, card_h, t->win_content);
    }

    /* Resize grip. */
    if ((win->flags & GUI_WIN_RESIZABLE) && !win->maximized) {
        uint32_t grip = active_theme.resize_grip;
        uint32_t gx = (uint32_t)win->x + win->w - grip - 1;
        uint32_t gy = (uint32_t)win->y + win->h - grip - 1;
        for (int i = 0; i < (int)grip; i += 3) {
            gfx_draw_line(gx + i, gy + grip - 1,
                          gx + grip - 1, gy + i, t->border);
        }
    }
}

/* ---- Window content blitting (optimised with memcpy rows) ---- */

static void blit_window_pixels(
    int32_t dst_x,
    int32_t dst_y,
    uint32_t width,
    uint32_t height,
    const uint32_t *src,
    uint32_t src_stride
) {
    if (width == 0 || height == 0 || !src) return;

    /* gfx_blit_rgb888 applies the active compositor clip and uses the
     * graphics driver's SSE2 bulk-copy path on x86_64. */
    (void)gfx_blit_rgb888(
        dst_x, dst_y, width, height,
        src, src_stride, NULL, 0
    );
}

static void blit_window_content_locked(const gui_win_t *win) {
    const uint32_t *pixels = win->front ? win->front : win->back;
    if (!pixels || win->back_w == 0 || win->back_h == 0) return;

    int32_t cx = content_x(win);
    int32_t cy = content_y(win);
    int32_t fw = (int32_t)gfx_get_width();
    int32_t fh = (int32_t)gfx_get_height();

    int32_t src_y = 0, src_x = 0;
    int32_t dst_y = cy, dst_x = cx;
    uint32_t copy_w = win->back_w, copy_h = win->back_h;

    {
        uint32_t lw = content_w(win), lh = content_h(win);
        if (copy_w > lw) copy_w = lw;
        if (copy_h > lh) copy_h = lh;
    }

    if (dst_y < 0) {
        src_y = -dst_y;
        if ((uint32_t)src_y >= copy_h) return;
        copy_h -= (uint32_t)src_y;
        dst_y = 0;
    }
    if (dst_x < 0) {
        src_x = -dst_x;
        if ((uint32_t)src_x >= copy_w) return;
        copy_w -= (uint32_t)src_x;
        dst_x = 0;
    }
    if (dst_y >= fh || dst_x >= fw) return;
    if (copy_h > (uint32_t)(fh - dst_y)) copy_h = (uint32_t)(fh - dst_y);
    if (copy_w > (uint32_t)(fw - dst_x)) copy_w = (uint32_t)(fw - dst_x);
    if (copy_w == 0 || copy_h == 0) return;

    const uint32_t stride = win->back_w;
    const uint32_t *src = pixels + (size_t)src_y * stride + (uint32_t)src_x;

    /* Content card radius: outer radius shrunk by (border + pad) so the
     * card nests inside the rounded window frame. */
    uint32_t outer_round = (win->flags & GUI_WIN_NO_DECOR) ? 0 : active_theme.win_round;
    int32_t  inner_round = (int32_t)outer_round - (int32_t)c_border();
    if (inner_round < 0) inner_round = 0;
    uint32_t round = (uint32_t)inner_round;

    /* Holes already split the content into multiple rectangles; keep the
     * existing path in that case (a hole is orthogonal to rounded outer
     * corners). */
    if (win->hole_r > win->hole_l && win->hole_b > win->hole_t) {
        int32_t sx1 = src_x + (int32_t)copy_w;
        int32_t sy1 = src_y + (int32_t)copy_h;
        int32_t hx0 = win->hole_l > src_x ? win->hole_l : src_x;
        int32_t hy0 = win->hole_t > src_y ? win->hole_t : src_y;
        int32_t hx1 = win->hole_r < sx1 ? win->hole_r : sx1;
        int32_t hy1 = win->hole_b < sy1 ? win->hole_b : sy1;

        if (hx1 > hx0 && hy1 > hy0) {
            uint32_t top_h = (uint32_t)(hy0 - src_y);
            uint32_t hole_h = (uint32_t)(hy1 - hy0);
            uint32_t left_w = (uint32_t)(hx0 - src_x);
            uint32_t right_x = (uint32_t)(hx1 - src_x);
            uint32_t right_w = copy_w - right_x;

            if (top_h != 0)
                blit_window_pixels(dst_x, dst_y, copy_w, top_h, src, stride);
            if (left_w != 0) {
                const uint32_t *left_src =
                    pixels + (size_t)hy0 * stride + (uint32_t)src_x;
                blit_window_pixels(dst_x, dst_y + (int32_t)top_h,
                                   left_w, hole_h, left_src, stride);
            }
            if (right_w != 0) {
                const uint32_t *right_src =
                    pixels + (size_t)hy0 * stride + (uint32_t)hx1;
                blit_window_pixels(dst_x + (int32_t)right_x,
                                   dst_y + (int32_t)top_h,
                                   right_w, hole_h, right_src, stride);
            }
            uint32_t bottom_h = (uint32_t)(sy1 - hy1);
            if (bottom_h != 0) {
                const uint32_t *bottom_src =
                    pixels + (size_t)hy1 * stride + (uint32_t)src_x;
                blit_window_pixels(dst_x, dst_y + (int32_t)(hy1 - src_y),
                                   copy_w, bottom_h, bottom_src, stride);
            }
            return;
        }
        /* Hole does not intersect the visible rect: fall through to the
         * normal path below. */
    }

    if (round == 0) {
        blit_window_pixels(dst_x, dst_y, copy_w, copy_h, src, stride);
        return;
    }

    /* Rounded content: taper the top and bottom r rows. */
    uint32_t r = round;
    uint32_t rmax = (copy_w < copy_h ? copy_w : copy_h) / 2;
    if (r > rmax) r = rmax;

    for (uint32_t row = 0; row < r; row++) {
        int32_t dy = (int32_t)r - 1 - (int32_t)row;
        int32_t rr = (int32_t)r * (int32_t)r - dy * dy;
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) <= rr) dx++;
        uint32_t inset = (uint32_t)((int32_t)r - dx);
        if (copy_w <= 2 * inset) continue;
        uint32_t bw = copy_w - 2 * inset;

        const uint32_t *stop =
            pixels + (size_t)(src_y + (int32_t)row) * stride + (uint32_t)src_x + inset;
        blit_window_pixels(dst_x + (int32_t)inset, dst_y + (int32_t)row,
                           bw, 1, stop, stride);

        uint32_t sbot_y = (uint32_t)(copy_h - 1 - row);
        const uint32_t *sbot =
            pixels + (size_t)(src_y + (int32_t)sbot_y) * stride + (uint32_t)src_x + inset;
        blit_window_pixels(dst_x + (int32_t)inset, dst_y + (int32_t)sbot_y,
                           bw, 1, sbot, stride);
    }
    if (copy_h > 2 * r) {
        const uint32_t *smid =
            pixels + (size_t)(src_y + (int32_t)r) * stride + (uint32_t)src_x;
        blit_window_pixels(dst_x, dst_y + (int32_t)r,
                           copy_w, copy_h - 2 * r, smid, stride);
    }
}

static void blit_window_content(const gui_win_t *win) {
    spinlock_acquire(&gui_lock);

    blit_window_content_locked(win);

    spinlock_release(&gui_lock);
}

/* ---- Desktop rendering ---- */

static void draw_desktop(void) {
    const gui_theme_t *t = &active_theme;
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    /* Vertical gradient. */
    gfx_gradient_v(0, 0, w, h, t->desktop_top, t->desktop_bot);
    /* Subtle horizontal stripes for texture. */
    for (uint32_t y = 0; y < h; y += 48) {
        gfx_draw_line(0, y, w, y, 0x00000018);
    }
}

/* ---- Snap preview overlay ---- */

static void draw_snap_preview(void) {
    if (!snap_preview_active) return;
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height() - active_theme.taskbar_h;
    uint32_t preview_color = 0x604080C0; /* semi-transparent blue */

    switch (snap_preview_type) {
        case GUI_SNAP_LEFT:
            gfx_fill_rect(0, 0, fw / 2, fh, preview_color);
            break;
        case GUI_SNAP_RIGHT:
            gfx_fill_rect(fw / 2, 0, fw / 2, fh, preview_color);
            break;
        case GUI_SNAP_TOP:
            gfx_fill_rect(0, 0, fw, fh / 2, preview_color);
            break;
        case GUI_SNAP_BOTTOM:
            gfx_fill_rect(0, fh / 2, fw, fh / 2, preview_color);
            break;
        case GUI_SNAP_MAXIMIZED:
            gfx_fill_rect(0, 0, fw, fh, preview_color);
            break;
        default: break;
    }
}

/* ---- Cursor rendering ---- */

static void draw_cursor(int mx, int my) {
    switch (cursor) {
        case GUI_CURSOR_ARROW:
        default: {
            /* Classic 11x16 arrow.  Each row is [black | white... | black]; the
            * tail narrows at rows 11..15 to leave the notch.  White cannot leak
            * outside the outline because the fill is computed from the row
            * width, not from a fixed triangle. */
            static const uint8_t arrow_w[16] = {
                1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 6, 5, 4, 3, 2
            };
            for (int i = 0; i < 16; i++) {
                int w = arrow_w[i];
                if (w > 2) {
                    gfx_fill_rect((uint32_t)(mx + 1), (uint32_t)(my + i),
                                (uint32_t)(w - 2), 1, 0x00FFFFFF);
                }
                gfx_fill_rect((uint32_t)mx, (uint32_t)(my + i), 1, 1, 0x00000000);
                if (w > 1) {
                    gfx_fill_rect((uint32_t)(mx + w - 1), (uint32_t)(my + i),
                                1, 1, 0x00000000);
                }
            }
            break;
        }
        case GUI_CURSOR_IBEAM:
            for (int i = -8; i <= 8; i++)
                gfx_putpixel((uint32_t)mx, (uint32_t)(my + i), 0x00000000);
            gfx_draw_line((uint32_t)(mx - 3), (uint32_t)(my - 8),
                          (uint32_t)(mx + 3), (uint32_t)(my - 8), 0x00000000);
            gfx_draw_line((uint32_t)(mx - 3), (uint32_t)(my + 8),
                          (uint32_t)(mx + 3), (uint32_t)(my + 8), 0x00000000);
            break;
        case GUI_CURSOR_HAND:
            gfx_fill_rect((uint32_t)(mx - 4), (uint32_t)(my - 1), 8, 12, 0x00FFFFFF);
            gfx_draw_rect((uint32_t)(mx - 4), (uint32_t)(my - 1), 8, 12, 0x00000000);
            break;
        case GUI_CURSOR_HRESIZE:
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)my,
                          (uint32_t)(mx + 8), (uint32_t)my, 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)(my - 1),
                          (uint32_t)(mx + 8), (uint32_t)(my - 1), 0x00000000);
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)(my + 1),
                          (uint32_t)(mx + 8), (uint32_t)(my + 1), 0x00000000);
            /* Arrowheads. */
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)my,
                          (uint32_t)(mx - 5), (uint32_t)(my - 3), 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)my,
                          (uint32_t)(mx - 5), (uint32_t)(my + 3), 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx + 8), (uint32_t)my,
                          (uint32_t)(mx + 5), (uint32_t)(my - 3), 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx + 8), (uint32_t)my,
                          (uint32_t)(mx + 5), (uint32_t)(my + 3), 0x00FFFFFF);
            break;
        case GUI_CURSOR_VRESIZE:
            gfx_draw_line((uint32_t)mx, (uint32_t)(my - 8),
                          (uint32_t)mx, (uint32_t)(my + 8), 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx - 1), (uint32_t)(my - 8),
                          (uint32_t)(mx - 1), (uint32_t)(my + 8), 0x00000000);
            gfx_draw_line((uint32_t)(mx + 1), (uint32_t)(my - 8),
                          (uint32_t)(mx + 1), (uint32_t)(my + 8), 0x00000000);
            break;
        case GUI_CURSOR_DRESIZE:
            for (int i = -7; i <= 7; i++)
                gfx_putpixel((uint32_t)(mx + i), (uint32_t)(my + i), 0x00FFFFFF);
            gfx_draw_line((uint32_t)(mx - 7), (uint32_t)(my - 7),
                          (uint32_t)(mx + 7), (uint32_t)(my + 7), 0x00000000);
            break;
        case GUI_CURSOR_WAIT:
            gfx_fill_circle((uint32_t)mx, (uint32_t)my, 6, 0x00FFFFFF);
            gfx_draw_circle((uint32_t)mx, (uint32_t)my, 7, 0x00000000);
            break;
        case GUI_CURSOR_MOVE: {
            /* Four-arrow cross. */
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)my,
                          (uint32_t)(mx + 8), (uint32_t)my, 0x00FFFFFF);
            gfx_draw_line((uint32_t)mx, (uint32_t)(my - 8),
                          (uint32_t)mx, (uint32_t)(my + 8), 0x00FFFFFF);
            break;
        }
        case GUI_CURSOR_CROSSHAIR: {
            gfx_draw_line((uint32_t)(mx - 8), (uint32_t)my,
                          (uint32_t)(mx - 2), (uint32_t)my, 0x00000000);
            gfx_draw_line((uint32_t)(mx + 2), (uint32_t)my,
                          (uint32_t)(mx + 8), (uint32_t)my, 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)(my - 8),
                          (uint32_t)mx, (uint32_t)(my - 2), 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)(my + 2),
                          (uint32_t)mx, (uint32_t)(my + 8), 0x00000000);
            break;
        }
        case GUI_CURSOR_NOT_ALLOWED: {
            gfx_draw_circle((uint32_t)mx, (uint32_t)my, 7, 0x00C04040);
            gfx_draw_line((uint32_t)(mx - 5), (uint32_t)(my + 5),
                          (uint32_t)(mx + 5), (uint32_t)(my - 5), 0x00C04040);
            break;
        }
    }
}

static void cursor_commit_presented(int have_cursor, int mx, int my) {
    last_drawn_valid = have_cursor;
    last_drawn_mx = have_cursor ? mx : -1;
    last_drawn_my = have_cursor ? my : -1;
}

/* Cursor coordinates are sampled once by the compositor tick and passed
 * through the complete render.  Never re-read mouse state between damage
 * calculation and drawing: doing so can stamp outside the flipped rectangle. */
static void cursor_draw_and_flip_full(int have_cursor, int mx, int my) {
    if (have_cursor) draw_cursor(mx, my);
    gfx_flip();
    cursor_commit_presented(have_cursor, mx, my);
}

static int cursor_intersects_rect(
    int mx,
    int my,
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h
) {
    int32_t cx0 = mx - CURSOR_DIRTY_LEFT;
    int32_t cy0 = my - CURSOR_DIRTY_TOP;
    int32_t cx1 = cx0 + CURSOR_DIRTY_W;
    int32_t cy1 = cy0 + CURSOR_DIRTY_H;
    int32_t rx1 = x + (int32_t)w;
    int32_t ry1 = y + (int32_t)h;

    return cx0 < rx1 && cy0 < ry1 && cx1 > x && cy1 > y;
}

static void cursor_draw_and_flip_rect(
    int32_t ux,
    int32_t uy,
    uint32_t uw,
    uint32_t uh,
    int have_cursor,
    int mx,
    int my
) {
    /* Avoid issuing cursor primitives for dirty regions nowhere near it. */
    if (have_cursor && cursor_intersects_rect(mx, my, ux, uy, uw, uh))
        draw_cursor(mx, my);

    gfx_flip_rect(ux, uy, uw, uh);
    cursor_commit_presented(have_cursor, mx, my);
}

/* ---- Full compositor render ---- */

/* Z-order insertion sort for visible-window arrays.
 * Uses local variables: windows[], _i, _j, _v.
 * Must be #undef'd after use in each function to avoid duplicate definitions. */
#define SORT_ARR(arr, cnt) do { \
    for (int _i = 1; _i < (cnt); _i++) { \
        int _v = (arr)[_i], _j = _i - 1; \
        while (_j >= 0 && windows[(arr)[_j]].z > windows[_v].z) { \
            (arr)[_j + 1] = (arr)[_j]; _j--; \
        } \
        (arr)[_j + 1] = _v; \
    } \
} while(0)

/* ---- CW-1: hierarchical child compositing ----
 *
 * A child window is drawn only as part of its parent's subtree, always above
 * the parent, and clipped to the intersection of every ancestor's content
 * rectangle (and, on the partial path, the dirty union — passed in as the
 * base clip rect).  Children carry no decoration or shadow (WS_CHILD ⇒
 * NO_DECOR|BORDERLESS), so only their content is blitted. */
static void blit_children(int pidx,
                          int32_t cl, int32_t ct, int32_t cr, int32_t cb) {
    if (cr <= cl || cb <= ct) return;      /* parent clip is empty */

    int kids[GUI_MAX_WINDOWS], nk = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_win_t *c = &windows[i];
        if (c->in_use && c->visible && !c->minimized && c->parent == pidx)
            kids[nk++] = i;
    }
    if (nk == 0) return;
    SORT_ARR(kids, nk);

    for (int i = 0; i < nk; i++) {
        int ci = kids[i];
        gui_win_t *c = &windows[ci];
        int32_t ax = win_abs_x(c), ay = win_abs_y(c);
        /* This child's on-screen clip: its own rect ∩ the parent clip. */
        int32_t nl = ax > cl ? ax : cl;
        int32_t nt = ay > ct ? ay : ct;
        int32_t cx1 = ax + (int32_t)c->w, cy1 = ay + (int32_t)c->h;
        int32_t nr = cx1 < cr ? cx1 : cr;
        int32_t nb = cy1 < cb ? cy1 : cb;
        if (nr <= nl || nb <= nt) continue; /* fully clipped away */

        gfx_clip_set(nl, nt, (uint32_t)(nr - nl), (uint32_t)(nb - nt));
        blit_window_content(c);

        /* Grandchildren clip to this child's content rect ∩ the current clip. */
        int32_t gl = content_x(c), gt = content_y(c);
        int32_t gr = gl + (int32_t)content_w(c);
        int32_t gb = gt + (int32_t)content_h(c);
        if (gl < nl) gl = nl;
        if (gt < nt) gt = nt;
        if (gr > nr) gr = nr;
        if (gb > nb) gb = nb;
        blit_children(ci, gl, gt, gr, gb);
    }
}

/* Draw a top-level window's decoration + content (under the currently-armed
 * base clip), then its whole child subtree clipped to its content rect ∩ the
 * base clip.  base_* is the base clip rect (full screen, or the dirty union). */
static void blit_window_tree(int idx,
                             int32_t base_l, int32_t base_t,
                             int32_t base_r, int32_t base_b) {
    gui_win_t *w = &windows[idx];
    blit_window_shadow(w);
    blit_window_decor(w);
    blit_window_content(w);

    int32_t cl = content_x(w), ct = content_y(w);
    int32_t cr = cl + (int32_t)content_w(w);
    int32_t cb = ct + (int32_t)content_h(w);
    if (cl < base_l) cl = base_l;
    if (ct < base_t) ct = base_t;
    if (cr > base_r) cr = base_r;
    if (cb > base_b) cb = base_b;
    blit_children(idx, cl, ct, cr, cb);
}

/* Per-rect partial redraw.  Each rect is clipped, redrawn, and flipped on
 * its own — a small cursor-adjacent update and a far-away window update
 * cost the sum of their areas, not the bounding box of both. */
static void compositor_render_dirty(const gui_dirty_rect_t *rects, size_t n,
                                    int have_cursor, int cursor_x, int cursor_y) {
    int32_t sw = (int32_t)gfx_get_width();
    int32_t sh = (int32_t)gfx_get_height();
    uint64_t total_px = 0;

    for (size_t ri = 0; ri < n; ri++) {
        int32_t x0 = rects[ri].x;
        int32_t y0 = rects[ri].y;
        int32_t x1 = rects[ri].x + (int32_t)rects[ri].w;
        int32_t y1 = rects[ri].y + (int32_t)rects[ri].h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > sw) x1 = sw;
        if (y1 > sh) y1 = sh;
        if (x0 >= x1 || y0 >= y1) continue;
        uint32_t rw = (uint32_t)(x1 - x0);
        uint32_t rh = (uint32_t)(y1 - y0);

        gfx_clip_set(x0, y0, rw, rh);
        draw_desktop();
        shell_draw_icons();

        int order[GUI_MAX_WINDOWS], n_ord = 0;
        int normal[GUI_MAX_WINDOWS], nn = 0;
        int atop[GUI_MAX_WINDOWS], na = 0;
        for (int k = 0; k < GUI_MAX_WINDOWS; k++) {
            if (windows[k].in_use && windows[k].visible && !windows[k].minimized &&
                windows[k].parent < 0) {
                if (windows[k].flags & GUI_WIN_ALWAYS_TOP) atop[na++] = k;
                else normal[nn++] = k;
            }
        }
        SORT_ARR(normal, nn);
        SORT_ARR(atop, na);
        for (int k = 0; k < nn; k++) order[n_ord++] = normal[k];
        for (int k = 0; k < na; k++) order[n_ord++] = atop[k];

        for (int k = 0; k < n_ord; k++) {
            const gui_win_t *w = &windows[order[k]];
            int off = active_theme.shadow_offset;
            int32_t wx0 = w->x - 1, wy0 = w->y - 1;
            int32_t wx1 = w->x + (int32_t)w->w + off + 1;
            int32_t wy1 = w->y + (int32_t)w->h + off + 1;
            if (wx1 <= x0 || wy1 <= y0 || wx0 >= x1 || wy0 >= y1) continue;
            gfx_clip_set(x0, y0, rw, rh);
            blit_window_tree(order[k], x0, y0, x1, y1);
        }
        gfx_clip_set(x0, y0, rw, rh);

        if (!gui_presentation_active()) {
            draw_snap_preview();
            shell_draw_taskbar();
            shell_draw_notifications();
        }

        if (have_cursor &&
            cursor_intersects_rect(cursor_x, cursor_y, x0, y0, rw, rh)) {
            draw_cursor(cursor_x, cursor_y);
        }

        gfx_flip_rect(x0, y0, rw, rh);
        gfx_clip_clear();
        total_px += (uint64_t)rw * rh;
    }

    cursor_commit_presented(have_cursor, cursor_x, cursor_y);

    perfstat_add(PERF_COMPOSITOR_FRAMES_PARTIAL, 1);
    perfstat_add(PERF_COMPOSITOR_PIXELS_COMPOSITED, gfx_take_px_counter());
    perfstat_add(PERF_COMPOSITOR_PIXELS_FLIPPED, total_px);
}

static void compositor_render(int have_cursor, int cursor_x, int cursor_y) {
    draw_desktop();

    /* Draw desktop icons. */
    shell_draw_icons();

    /* Build z-sorted index of visible windows. */
    int order[GUI_MAX_WINDOWS], n = 0;
    /* Always-on-top windows come last (drawn on top). */
    int normal[GUI_MAX_WINDOWS], nn = 0;
    int atop[GUI_MAX_WINDOWS], na = 0;

    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        /* CW-1: top-level only; children draw via their parent's subtree. */
        if (windows[i].in_use && windows[i].visible && !windows[i].minimized &&
            windows[i].parent < 0) {
            if (windows[i].flags & GUI_WIN_ALWAYS_TOP) atop[na++] = i;
            else normal[nn++] = i;
        }
    }

    /* Insertion sort each group by z ascending. */
    SORT_ARR(normal, nn);
    SORT_ARR(atop, na);

    n = 0;
    for (int i = 0; i < nn; i++) order[n++] = normal[i];
    for (int i = 0; i < na; i++) order[n++] = atop[i];

    {
        int32_t fw = (int32_t)gfx_get_width(), fh = (int32_t)gfx_get_height();
        for (int i = 0; i < n; i++) {
            /* Restore the full-screen clip: a previous window's subtree left a
             * narrower child clip armed. */
            gfx_clip_clear();
            blit_window_tree(order[i], 0, 0, fw, fh);
        }
        gfx_clip_clear();
    }

    /* Desktop chrome yields completely to a presentation surface. */
    if (!gui_presentation_active()) { draw_snap_preview(); shell_draw_taskbar(); shell_draw_notifications(); }

    /* Cursor is composited last and remains in the back buffer. */
    cursor_draw_and_flip_full(have_cursor, cursor_x, cursor_y);

    /* OPT_PLAN.md O0/O4: full path — the pixel count is the gfx layer's
     * real post-clip store count now (clip is full-screen here). */
    {
        uint64_t pw = gfx_get_width(), ph = gfx_get_height();
        perfstat_add(PERF_COMPOSITOR_FRAMES_FULL, 1);
        perfstat_add(PERF_COMPOSITOR_PIXELS_COMPOSITED, gfx_take_px_counter());
        perfstat_add(PERF_COMPOSITOR_PIXELS_FLIPPED, pw * ph);
    }
}

/* ===================================================================
 * Hit-testing
 * =================================================================== */

/* CW-1: descend from a top-level window into the deepest visible child that
 * contains (mx,my), so clicks route to the pane / listview under the cursor.
 * Returns pidx itself if no child is hit. */
static int hit_child(int pidx, int32_t mx, int32_t my) {
    int best = -1, bestz = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_win_t *c = &windows[i];
        if (!c->in_use || !c->visible || c->minimized || c->parent != pidx)
            continue;
        int32_t ax = win_abs_x(c), ay = win_abs_y(c);
        uint32_t r = (c->flags & GUI_WIN_NO_DECOR) ? 0 : active_theme.win_round;
            if (point_in_round_rect(mx, my, ax, ay, c->w, c->h, r)) {
            /* WR-3: clicks fall through the see-through hole exactly like
             * pixels do, or the tab would swallow the editor's mouse.  The
             * hole lives in buffer pixels, whose origin is the content
             * origin (identical to the window origin for a borderless
             * child, but spelled out for a decorated one). */
            if (c->hole_r > c->hole_l && c->hole_b > c->hole_t) {
                int32_t lx = mx - content_x(c), ly = my - content_y(c);
                if (lx >= c->hole_l && lx < c->hole_r &&
                    ly >= c->hole_t && ly < c->hole_b)
                    continue;
            }
            if (c->z >= bestz) { bestz = c->z; best = i; }
        }
    }
    if (best < 0) return pidx;
    return hit_child(best, mx, my);
}

static int hit_window(int32_t mx, int32_t my) {
    int best = -1, bestz = -1;
    /* Check always-on-top windows first.  CW-1: only TOP-LEVEL windows are
     * hit-tested here; a hit then descends into that window's child subtree. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            if (!windows[i].in_use || !windows[i].visible || windows[i].minimized) continue;
            if (windows[i].parent >= 0) continue;   /* children handled below */
            if (pass == 0 && !(windows[i].flags & GUI_WIN_ALWAYS_TOP)) continue;
            if (pass == 1 && (windows[i].flags & GUI_WIN_ALWAYS_TOP)) continue;
            gui_win_t *w = &windows[i];
            uint32_t r = (w->flags & GUI_WIN_NO_DECOR) ? 0 : active_theme.win_round;
            if (point_in_round_rect(mx, my, w->x, w->y, w->w, w->h, r)) {
                if (w->z > bestz) { bestz = w->z; best = i; }
            }
        }
        if (best >= 0 && pass == 0) return hit_child(best, mx, my); /* always-on-top hit */
    }
    return best >= 0 ? hit_child(best, mx, my) : -1;
}

/* Hit part: 0=client, 1=titlebar, 2=close, 3=max/restore, 4=minimize,
 * 5=resize grip, 6=outside, 7=edge-left, 8=edge-right, 9=edge-top, 10=edge-bottom */
static int hit_part(const gui_win_t *w, int32_t mx, int32_t my) {
    if (w->flags & GUI_WIN_NO_DECOR) return 0;
    if (mx < w->x || mx >= (int32_t)(w->x + w->w) ||
        my < w->y || my >= (int32_t)(w->y + w->h)) return 6;

    uint32_t brd = c_border();
    uint32_t tbh = c_titlebar();
    uint32_t grip = active_theme.resize_grip;
    uint32_t btn_w = 18, btn_pad = 2;

    /* Edge resize (only if resizable and not maximized). */
    if ((w->flags & GUI_WIN_RESIZABLE) && !w->maximized) {
        /* Bottom-right grip. */
        int32_t gx = w->x + (int32_t)w->w - (int32_t)grip - 1;
        int32_t gy = w->y + (int32_t)w->h - (int32_t)grip - 1;
        if (mx >= gx && my >= gy) return 5;
        /* Left edge. */
        if (mx < w->x + (int32_t)brd + 3) return 7;
        /* Right edge. */
        if (mx >= w->x + (int32_t)w->w - (int32_t)brd - 3) return 8;
        /* Top edge (but only below titlebar if at the very top). */
        if (my < w->y + (int32_t)brd + 3 && my >= w->y + (int32_t)tbh) return 9;
        /* Bottom edge. */
        if (my >= w->y + (int32_t)w->h - (int32_t)brd - 3) return 10;
    }

    /* Title bar region. */
    if (my < w->y + (int32_t)brd + (int32_t)tbh) {
        /* Close button. */
        if (w->flags & GUI_WIN_HAS_CLOSE) {
            int32_t bx = w->x + (int32_t)w->w - (int32_t)brd - (int32_t)btn_w - (int32_t)btn_pad;
            int32_t by = w->y + (int32_t)brd + ((int32_t)tbh - 16) / 2;
            if (mx >= bx && mx < bx + (int32_t)btn_w && my >= by && my < by + 16) return 2;
        }
        /* Max/Min buttons. */
        if (w->flags & GUI_WIN_HAS_MINMAX) {
            int32_t bx = w->x + (int32_t)w->w - (int32_t)brd - 2 * ((int32_t)btn_w + (int32_t)btn_pad);
            int32_t by = w->y + (int32_t)brd + ((int32_t)tbh - 16) / 2;
            if (mx >= bx && mx < bx + (int32_t)btn_w && my >= by && my < by + 16) return 3;
            int32_t mx2 = bx - (int32_t)btn_w - (int32_t)btn_pad;
            if (mx >= mx2 && mx < mx2 + (int32_t)btn_w && my >= by && my < by + 16) return 4;
        }
        return 1; /* title bar */
    }
    return 0; /* client area */
}

/* W32A-11: the native GUI sender submits an absolute pathname; the
 * compositor selects a DIFFERENT visible client-area window by the actual
 * pointer position. A generation token links the event to a one-shot path
 * slot so no user address leaks into another address space. */
int gui_send_file_drop(int source_wid, uint64_t source_pid, const char *path) {
    if (!path || path[0] != '/') return -1;
    size_t n = 0;
    while (n < GUI_DROP_PATH_MAX && path[n]) {
        if ((unsigned char)path[n] < 32u) return -1;
        ++n;
    }
    if (n < 2 || n >= GUI_DROP_PATH_MAX) return -1;
    int32_t mx, my;
    if (gui_mouse_position(&mx, &my) != 1) return -1;
    spinlock_acquire(&gui_lock);
    if (!source_pid || !win_alive(source_wid) ||
        (uint32_t)windows[source_wid].owner_pid != source_pid) {
        spinlock_release(&gui_lock); return -1;
    }
    int dst = hit_window(mx, my);
    if (dst < 0 || dst == source_wid || hit_part(&windows[dst], mx, my) != 0) {
        spinlock_release(&gui_lock); return -1;
    }
    gui_win_t *w = &windows[dst];
    gui_event_t evt = { GUI_EVT_DROP, 0, 0, 0, 0, 0, 0 };
    evt.x = mx - content_x(w);
    evt.y = my - content_y(w);
    spinlock_acquire(&gui_evt_lock);
    int slot = -1;
    for (int i = 0; i < GUI_DROP_SLOTS; ++i)
        if (!w->drop_busy[i]) { slot = i; break; }
    if (slot < 0) {
        spinlock_release(&gui_evt_lock);
        spinlock_release(&gui_lock);
        return -1;
    }
    uint16_t gen = (uint16_t)((drop_epoch[dst][slot] + 1u) & 0x0fffu);
    if (!gen) gen = 1;
    drop_epoch[dst][slot] = gen;
    w->drop_busy[slot] = 1;
    w->drop_generation[slot] = gen;
    memset(w->drop_paths[slot], 0, GUI_DROP_PATH_MAX);
    memcpy(w->drop_paths[slot], path, n + 1);
    evt.data = (uint16_t)((gen << 4) | (unsigned)(slot + 1));
    gui_post_event_locked(w, &evt);
    spinlock_release(&gui_evt_lock);
    spinlock_release(&gui_lock);
    wq_wake_all(&gui_evt_wq);
    return 0;
}
int gui_take_file_drop(int destination_wid, uint64_t destination_pid,
                       uint16_t token, char out[GUI_DROP_PATH_MAX]) {
    if (!out || !destination_pid) return -1;
    spinlock_acquire(&gui_evt_lock);
    if (!win_alive(destination_wid) ||
        (uint32_t)windows[destination_wid].owner_pid != destination_pid) {
        spinlock_release(&gui_evt_lock); return -1;
    }
    gui_win_t *w = &windows[destination_wid];
    unsigned index = token & 15u;
    if (!index || index > GUI_DROP_SLOTS || !w->drop_busy[index - 1] ||
        w->drop_generation[index - 1] != (token >> 4)) {
        spinlock_release(&gui_evt_lock); return -1;
    }
    memcpy(out, w->drop_paths[index - 1], GUI_DROP_PATH_MAX);
    w->drop_busy[index - 1] = 0;
    spinlock_release(&gui_evt_lock);
    return 0;
}

/* ===================================================================
 * Input → event routing
 * =================================================================== */

/* Detect snap zone from mouse position while dragging. */
static gui_snap_t detect_snap_zone(int32_t mx, int32_t my) {
    uint32_t fw = gfx_get_width();
    (void)gfx_get_height(); /* used indirectly via active_theme.taskbar_h */
    int32_t margin = 8;

    if (mx <= margin) return GUI_SNAP_LEFT;
    if (mx >= (int32_t)fw - margin) return GUI_SNAP_RIGHT;
    if (my <= margin) return GUI_SNAP_MAXIMIZED;
    return GUI_SNAP_NONE;
}

static void update_button_hover(void) {
    int mx = -1, my = -1;
    if (mouse_get_position(&mx, &my) != 1) {
        if (hover_btn_wid != -1 || hover_btn_kind != 0) {
            hover_btn_wid = -1; hover_btn_kind = 0;
            full_dirty = 1;
        }
        return;
    }
    int wid = hit_window(mx, my);
    int kind = 0;
    if (wid >= 0) {
        int part = hit_part(&windows[wid], mx, my);
        if      (part == 2) kind = 1;
        else if (part == 3) kind = 2;
        else if (part == 4) kind = 3;
    }
    if (wid != hover_btn_wid || kind != hover_btn_kind) {
        hover_btn_wid = wid;
        hover_btn_kind = kind;
        full_dirty = 1;
    }
}

static void route_mouse_event(const mouse_event_t *ev) {
    int32_t mx = ev->abs_x, my = ev->abs_y;
    update_button_hover();     /* NEW */
    int wid = hit_window(mx, my);
    gui_cursor_t target_cursor = GUI_CURSOR_ARROW;

    /* W32A-5: a live capture redirects every mouse event to the capturing
     * window's client area, whatever is under the cursor.  Release is
     * explicit (SetCapture/ReleaseCapture); button-up does not clear it --
     * the implicit capture of a title-bar drag is the drag_mode branch
     * below and is unaffected. */
    int capture_redirect = 0;
    if (captured >= 0) {
        if (!win_alive(captured)) {
            captured = -1;
        } else if (!drag_mode) {
            wid = captured;
            capture_redirect = 1;
        }
    }

    /* ---- Active drag/resize ---- */
    if (drag_mode) {
        gui_win_t *w = &windows[drag_wid];
        if (!win_alive(drag_wid)) { drag_mode = 0; }
        else if (drag_mode == 1) {
            /* Move. */
            int32_t nx = mx - drag_dx;
            int32_t ny = my - drag_dy;
            /* Clamp to screen. */
            if (nx < -(int32_t)(w->w - 40)) nx = -(int32_t)(w->w - 40);
            if (ny < 0) ny = 0;
            uint32_t fh = gfx_get_height();
            if (ny > (int32_t)(fh - active_theme.taskbar_h - 20))
                ny = (int32_t)(fh - active_theme.taskbar_h - 20);
            mark_window_dirty(w);
            w->x = nx; w->y = ny;
            mark_window_dirty(w);

            /* Snap zone detection. */
            gui_snap_t snap = detect_snap_zone(mx, my);
            if (snap != GUI_SNAP_NONE) {
                snap_preview_active = 1;
                snap_preview_type = snap;
            } else {
                snap_preview_active = 0;
            }
            target_cursor = GUI_CURSOR_MOVE;
        } else if (drag_mode == 2) {
            /* Resize. */
            int32_t nx = w->x, ny_pos = w->y;
            uint32_t nw = w->w, nh = w->h;

            if (drag_edge & 1) { /* left */
                int32_t dx = mx - drag_dx;
                nw = drag_orig_w - (uint32_t)dx;
                nx = drag_orig_x + dx;
                if (nw < 80) { nw = 80; nx = drag_orig_x + (int32_t)drag_orig_w - 80; }
            }
            if (drag_edge & 2) { /* right */
                nw = (uint32_t)((int32_t)drag_orig_w + (mx - drag_dx));
                if (nw < 80) nw = 80;
            }
            if (drag_edge & 4) { /* top */
                int32_t dy = my - drag_dy;
                nh = drag_orig_h - (uint32_t)dy;
                ny_pos = drag_orig_y + dy;
                if (nh < 60) { nh = 60; ny_pos = drag_orig_y + (int32_t)drag_orig_h - 60; }
            }
            if (drag_edge & 8) { /* bottom */
                nh = (uint32_t)((int32_t)drag_orig_h + (my - drag_dy));
                if (nh < 60) nh = 60;
            }

            mark_window_dirty(w);
            w->x = nx; w->y = ny_pos;
            gui_resize_window(drag_wid, nw, nh);
            mark_window_dirty(w);
            full_dirty = 1;

            if (drag_edge & 0x3) target_cursor = GUI_CURSOR_HRESIZE;
            else if (drag_edge & 0xC) target_cursor = GUI_CURSOR_VRESIZE;
            else target_cursor = GUI_CURSOR_DRESIZE;
        }

        if (ev->released & MOUSE_BTN_LEFT) {
            int rel_wid = drag_wid;
            gui_snap_t apply_snap = snap_preview_type;
            drag_mode = 0;
            drag_wid  = -1;
            snap_preview_active = 0;
            snap_preview_type = GUI_SNAP_NONE;
            /* FIX (snap-on-release): the old code cleared drag_wid before
             * checking it, so the snap never applied — the preview drew but
             * the window never snapped.  Apply the pending snap to the
             * window that was actually being dragged. */
            if (apply_snap != GUI_SNAP_NONE && rel_wid >= 0 && win_alive(rel_wid)) {
                gui_snap_window(rel_wid, apply_snap);
            }
            full_dirty = 1;
        }
        gui_set_cursor(target_cursor);
        return;
    }

    /* ---- No drag active ---- */

    /* Start menu is not a normal window.  Keep it open while the cursor moves
     * from the Start button into the menu; only a left-click outside closes it. */
    if (shell_handle_start_menu_click(mx, my, wid,
            (ev->pressed & MOUSE_BTN_LEFT) != 0)) {
        return;
    }

    /* OF0: the taskbar overflow list, when open, is modal-ish: it sits above
     * the taskbar and swallows clicks inside itself. */
    if (shell_handle_overflow_click(mx, my,
            (ev->pressed & MOUSE_BTN_LEFT) != 0)) {
        return;
    }

    if (wid < 0) {
        int left = (ev->pressed & MOUSE_BTN_LEFT) != 0;
        if (!shell_handle_taskbar_click(mx, my, left)) {
            shell_handle_icon_click(mx, my, left);
        }
        gui_set_cursor(GUI_CURSOR_ARROW);
        last_hover_wid = -1;
        return;
    }

    gui_win_t *w = &windows[wid];
    int part = hit_part(w, mx, my);
    /* A captured window sees client-area events only: no title-bar drag, no
     * resize edges (SetCapture is a client-area contract). */
    if (capture_redirect) part = 0;

    /* Cursor shape from hover. */
    switch (part) {
        case 5: case 7: case 8:
            target_cursor = GUI_CURSOR_HRESIZE; break;
        case 9: case 10:
            target_cursor = GUI_CURSOR_VRESIZE; break;
        default:
            target_cursor = GUI_CURSOR_ARROW; break;
    }
    gui_set_cursor(target_cursor);

    /* ---- Left button pressed ---- */
    if (ev->pressed & MOUSE_BTN_LEFT) {
        /* CW-1: clicking a child raises (and activates) its top-level ancestor
         * — the whole window comes to front — while the mouse event itself is
         * still delivered to the child (wid) below, so its pane/listview reacts. */
        int top = wid;
        while (top >= 0 && win_alive(top) && windows[top].parent >= 0)
            top = windows[top].parent;
        gui_focus_window(top >= 0 ? top : wid);
        switch (part) {
            case 2: /* Close */
                request_window_close(wid);
                return;
            case 3: /* Max/Restore */
                if (w->maximized) gui_restore_window(wid);
                else              gui_maximize_window(wid);
                return;
            case 4: /* Minimize */
                gui_minimize_window(wid);
                return;
            case 1: { /* Title bar — drag or double-click maximize */
                uint32_t now = (uint32_t)timer_get_ticks();
                int dbl = gui_is_double_click(now, mx, my, wid, part, MOUSE_BTN_LEFT);
                gui_record_click(now, mx, my, wid, part, MOUSE_BTN_LEFT);
                if (dbl && (w->flags & GUI_WIN_RESIZABLE)) {
                    if (w->maximized) gui_restore_window(wid);
                    else              gui_maximize_window(wid);
                    return;
                }
                if (!w->maximized && (w->flags & GUI_WIN_MOVABLE)) {
                    drag_wid  = wid;
                    drag_mode = 1;
                    drag_dx   = mx - w->x;
                    drag_dy   = my - w->y;
                    snap_preview_active = 0;
                }
                return;
            }
            case 5: { /* Resize grip */
                drag_wid    = wid;
                drag_mode   = 2;
                drag_edge   = 2 | 8;  /* right + bottom */
                drag_orig_w = w->w;
                drag_orig_h = w->h;
                drag_orig_x = w->x;
                drag_orig_y = w->y;
                drag_dx     = mx;
                drag_dy     = my;
                return;
            }
            case 7: { /* Left edge */
                drag_wid = wid; drag_mode = 2; drag_edge = 1;
                drag_orig_w = w->w; drag_orig_h = w->h;
                drag_orig_x = w->x; drag_orig_y = w->y;
                drag_dx = mx; drag_dy = my;
                return;
            }
            case 8: { /* Right edge */
                drag_wid = wid; drag_mode = 2; drag_edge = 2;
                drag_orig_w = w->w; drag_orig_h = w->h;
                drag_orig_x = w->x; drag_orig_y = w->y;
                drag_dx = mx; drag_dy = my;
                return;
            }
            case 9: { /* Top edge */
                drag_wid = wid; drag_mode = 2; drag_edge = 4;
                drag_orig_w = w->w; drag_orig_h = w->h;
                drag_orig_x = w->x; drag_orig_y = w->y;
                drag_dx = mx; drag_dy = my;
                return;
            }
            case 10: { /* Bottom edge */
                drag_wid = wid; drag_mode = 2; drag_edge = 8;
                drag_orig_w = w->w; drag_orig_h = w->h;
                drag_orig_x = w->x; drag_orig_y = w->y;
                drag_dx = mx; drag_dy = my;
                return;
            }
            case 0: { /* Client area */
                uint32_t now = (uint32_t)timer_get_ticks();
                int dbl = gui_is_double_click(now, mx, my, wid, part, MOUSE_BTN_LEFT);
                gui_record_click(now, mx, my, wid, part, MOUSE_BTN_LEFT);
                kprintf("gui_dbg_click now=%u wid=%d part=%d dbl=%d "
                       "(last tick=%u wid=%d part=%d)\n",
                       now, wid, part, dbl, last_click_tick, last_click_wid,
                       last_click_part);
                /* WR-2 gap 2: a click gives the KEYBOARD to whatever was hit,
                 * child included -- `wid` comes from hit_window(), which
                 * already descends the child subtree.  Without this the panel
                 * under the cursor still would not own Backspace after the
                 * user clicked on it. */
                if (wid != key_focus) {
                    int prev = gui_key_target();
                    key_focus = wid;
                    if (prev >= 0 && prev != wid && win_alive(prev)) {
                        gui_event_t be = { GUI_EVT_BLUR, 0,0,0,0,0,0 };
                        gui_post_event(prev, &be);
                    }
                    gui_event_t fe = { GUI_EVT_FOCUS, 0,0,0,0,0,0 };
                    gui_post_event(wid, &fe);
                    if (win_has_parent(&windows[wid]))
                        kprintf("[gui] key focus -> child wid %d (parent %d)\n",
                                wid, windows[wid].parent);
                }
                gui_event_t e = {0};
                e.type    = dbl ? GUI_EVT_MOUSE_DBLCLICK : GUI_EVT_MOUSE_DOWN;
                e.x       = mx - content_x(w);
                e.y       = my - content_y(w);
                e.buttons = ev->buttons;
                e.mods    = keyboard_get_mods();
                gui_post_event(wid, &e);
                return;
            }
        }
    }

    /* ---- Right button pressed ---- */
    if (ev->pressed & MOUSE_BTN_RIGHT) {
        if (part == 0) {
            gui_event_t e = { GUI_EVT_CONTEXT_MENU, 0,0,0,0,0,0 };
            e.x = mx - content_x(w);
            e.y = my - content_y(w);
            e.mods = keyboard_get_mods();
            gui_post_event(wid, &e);
        }
        return;
    }

    /* ---- Middle button pressed ---- */
    if (ev->pressed & 0x04) {
        if (part == 0) {
            gui_event_t e = { GUI_EVT_MOUSE_MIDDLE_DOWN, 0,0,0,0,0,0 };
            e.x = mx - content_x(w);
            e.y = my - content_y(w);
            gui_post_event(wid, &e);
        }
        return;
    }

    /* ---- Left button released ---- */
    if (ev->released & MOUSE_BTN_LEFT) {
        if (part == 0) {
            gui_event_t e = { GUI_EVT_MOUSE_UP, 0,0,0,0,0,0 };
            e.x = mx - content_x(w);
            e.y = my - content_y(w);
            e.buttons = ev->buttons;
            e.mods    = keyboard_get_mods();
            gui_post_event(wid, &e);
        }
        return;
    }
    if (ev->released & MOUSE_BTN_RIGHT) {
        if (part == 0) {
            gui_event_t e = { GUI_EVT_MOUSE_RIGHT_UP, 0,0,0,0,0,0 };
            e.x = mx - content_x(w);
            e.y = my - content_y(w);
            gui_post_event(wid, &e);
        }
        return;
    }
    if (ev->released & 0x04) {
        if (part == 0) {
            gui_event_t e = { GUI_EVT_MOUSE_MIDDLE_UP, 0,0,0,0,0,0 };
            e.x = mx - content_x(w);
            e.y = my - content_y(w);
            gui_post_event(wid, &e);
        }
        return;
    }

    /* ---- Mouse wheel ---- */
    if (ev->wheel) {
        gui_event_t e = { GUI_EVT_MOUSE_WHEEL, 0,0,0,0,0,0 };
        e.x = mx - content_x(w);
        e.y = my - content_y(w);
        e.key = (uint32_t)(int32_t)ev->wheel;
        gui_post_event(wid, &e);
        return;
    }

    /* ---- Mouse move ---- */
    if (ev->dx || ev->dy) {
        gui_event_t e = { GUI_EVT_MOUSE_MOVE, 0,0,0,0,0,0 };
        e.x = mx - content_x(w);
        e.y = my - content_y(w);
        e.buttons = ev->buttons;
        gui_post_event(wid, &e);
    }
}

/* ---- Keyboard routing ---- */

static void route_key_event(const kb_event_t *ke) {
    /* Global shortcuts. */
    if (ke->pressed) {
        uint8_t mods = ke->mods;
        /* Alt+F4 → close focused window. */
        if ((mods & KB_MOD_ALT) && ke->key == KB_KEY_F4) {
            if (focused >= 0) {
                request_window_close(focused);
                return;
            }
        }
    }

    /* WR-2 gap 2: keys go to the KEYBOARD target, which may be a WS_CHILD
     * pane -- the top-level WM focus is chrome-only and would swallow every
     * keystroke a child control owns. */
    int ktarget = gui_key_target();
    if (ktarget < 0) return;
    gui_event_t e = {0};
    e.type = ke->pressed ? GUI_EVT_KEY_DOWN : GUI_EVT_KEY_UP;
    e.key  = ke->key;
    e.mods = ke->mods;
    /* OFFICE_PLAN OF1: the Unicode codepoint rides in `data`, which key events
     * have never used.  An application that ignores it sees exactly the events
     * it saw before this field existed. */
    e.data = ke->cp;
    gui_post_event(ktarget, &e);
}

/* ===================================================================
 * Compositor tick
 * =================================================================== */

void gui_compositor_tick(void) {
    /* Pump mouse events. */
    mouse_event_t me;
    while (mouse_get_event(&me)) route_mouse_event(&me);

    /* Pump keyboard events. */
    kb_event_t ke;
    while (keyboard_get_event(&ke)) route_key_event(&ke);

    /* Capture the pointer once.  The exact same snapshot drives both damage
     * calculation and cursor drawing/flip, preventing cursor/frame divergence. */
    int cursor_x = -1;
    int cursor_y = -1;
    int have_cursor = (mouse_get_position(&cursor_x, &cursor_y) == 1);

    if (have_cursor) {
        if (!last_drawn_valid ||
            last_drawn_mx != cursor_x || last_drawn_my != cursor_y) {
            if (last_drawn_valid)
                gui_mark_cursor_dirty(last_drawn_mx, last_drawn_my);
            gui_mark_cursor_dirty(cursor_x, cursor_y);
        }
    } else if (last_drawn_valid) {
        /* Mouse device disappeared or became unavailable: erase its old stamp. */
        gui_mark_cursor_dirty(last_drawn_mx, last_drawn_my);
    }

    /* Clock tick: mark taskbar dirty once per second for the clock. */
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t ticks = timer_get_ticks();
    uint64_t elapsed_ticks = (ticks >= gui_clock_base_ticks) ?
        (ticks - gui_clock_base_ticks) : ticks;
    uint64_t now_sec = elapsed_ticks / hz;
    if (now_sec != gui_clock_last_second) {
        gui_clock_last_second = now_sec;
        gui_mark_dirty(0, (int32_t)(gfx_get_height() - active_theme.taskbar_h),
                       gfx_get_width(), active_theme.taskbar_h);
    }

    /* Check for any window content that was dirtied. */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].visible && windows[i].content_dirty) {
            windows[i].content_dirty = 0;
            mark_window_dirty(&windows[i]);
        }
    }

    /* Expire notifications. */
    shell_expire_notifications();

    /* The dirty list lives in Rust (gui_dirty_snapshot).  It is drained by
    * gui_dirty_snapshot() before the render, so a concurrent gui_mark_dirty()
    * from a syscall thread lands in the NEXT tick, not in this frame. */
    if (full_dirty) {
        full_dirty = 0;
        gui_dirty_reset();
        compositor_render(have_cursor, cursor_x, cursor_y);
    } else {
        gui_dirty_rect_t rects[GUI_MAX_DIRTY_RECTS];
        size_t n = gui_dirty_snapshot(rects, GUI_MAX_DIRTY_RECTS);
        if (n > 0)
            compositor_render_dirty(rects, n, have_cursor, cursor_x, cursor_y);
    }
}

void gui_render_now(void) {
    int cursor_x = -1, cursor_y = -1;
    int have_cursor = (mouse_get_position(&cursor_x, &cursor_y) == 1);
    full_dirty = 0;
    gui_dirty_reset();
    compositor_render(have_cursor, cursor_x, cursor_y);
}

/* ===================================================================
 * Compositor thread
 * =================================================================== */

void gui_compositor_thread(void *arg) {
    (void)arg;
    fb_set_console_enabled(0);
    full_dirty = 1;

    /* Add default desktop icons. */
    gui_add_icon(20, 20, "Terminal", 1);
    gui_add_icon(20, 80, "Files", 2);
    gui_add_icon(20, 140, "Editor", 3);
    gui_add_icon(20, 200, "Calc", 4);
    gui_add_icon(20, 260, "SysMon", 5);
    gui_add_icon(20, 320, "About", 6);
    /* OF0: the suite on the desktop too (OFFICE_PLAN OF9 asks for the Start
     * and desktop entries; the entry exists as soon as there is a binary). */
    gui_add_icon(20, 380, "Office", 11);

    for (;;) {
        /* Sleep until someone creates work (O4).  The pending flag is
         * consumed under the lock so a poke racing this check cannot be
         * lost — it either flips the flag before we test it, or finds
         * us already on the wait queue and wakes us. */
        uint64_t fl = spinlock_acquire_irqsave(&gui_wake_lock);
        while (!__atomic_load_n(&gui_pending, __ATOMIC_ACQUIRE)) {
            wq_wait(&gui_wq, &gui_wake_lock);
        }
        __atomic_store_n(&gui_pending, 0, __ATOMIC_RELEASE);
        spinlock_release_irqrestore(&gui_wake_lock, fl);

        /* Drain: tick until the scene settles, pacing at the historical
         * ~100 Hz — but through a BLOCKING sleep (sleep_deadline via
         * timer_sleep_ms), not a yield spin, so the pacing gap is idle
         * time the scheduler can actually give away. */
        do {
            gui_compositor_tick();
            timer_sleep_ms(10);
        } while (__atomic_load_n(&gui_pending, __ATOMIC_ACQUIRE) ||
            full_dirty || !gui_dirty_is_empty());
    }
}

/* ===================================================================
 * Self-test
 * =================================================================== */

void gui_self_test(void) {
    kprintf("[gui] self-test: creating test windows...\n");
    int w1 = gui_create_window(80, 60, 360, 200, "Hello GUI", GUI_WIN_DEFAULT);
    int w2 = gui_create_window(460, 100, 300, 220, "Second Window", GUI_WIN_DEFAULT);
    if (w1 < 0 || w2 < 0) {
        kprintf("[gui] FAIL: could not create windows\n");
        return;
    }
    gui_clear(w1, 0x00F8F8FF);
    gui_draw_text(w1, 12, 16, "AuraLite GUI v2.0 subsystem online.", 0x00000000);
    gui_draw_text(w1, 12, 32, "New: themes, icons, snap, notifications!", 0x00404080);
    gui_draw_text(w1, 12, 48, "Drag to edges to snap windows.", 0x00404080);
    gui_fill_rect(w1, 12, 70, 100, 30, 0x004080C0);
    gui_draw_rect(w1, 12, 70, 100, 30, 0x00000000);
    gui_draw_text(w1, 30, 80, "Button", 0x00FFFFFF);

    gui_clear(w2, 0x00FFFFE8);
    gui_draw_text(w2, 12, 16, "Second window!", 0x00800000);
    gui_draw_text(w2, 12, 32, "Try right-click for context menu.", 0x00400040);
    gui_draw_text(w2, 12, 48, "Double-click titlebar to maximize.", 0x00400040);

    gui_show_window(w1);
    gui_show_window(w2);

    /* Post a test notification. */
    gui_notify("GUI subsystem initialized.", 0x004CAF50, 4000);

    gui_render_now();
    kprintf("[gui] PASS: GUI v2.0 subsystem rendered initial composition\n");
}