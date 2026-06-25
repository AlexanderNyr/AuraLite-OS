/* gui.c — kernel-side GUI core: compositor, window manager, event router.
 *
 * Design notes:
 *   - Each window has a private back-buffer (kmalloc'd).  Drawing into the
 *     window updates only that buffer; the compositor composes everything
 *     onto the front framebuffer in z-order during gui_compositor_tick().
 *   - Mouse and keyboard events from the kernel drivers are routed once per
 *     compositor tick: hover/down/up to the window under the cursor, key
 *     events to the focused window, with hit-tested decoration handling
 *     (title-bar drag, [X], resize grip) intercepted by the WM first.
 *   - Locking is single-threaded for now: the compositor thread is the only
 *     mutator of window geometry.  User-space-bound mutations go through
 *     syscalls that run in the calling thread but the underlying state is
 *     guarded by a spinlock.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/gui/gui.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/mm/kheap.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/font.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/timer/pit.h"

/* ---- Window state ---- */
typedef struct gui_win {
    int       in_use;
    int       visible;
    int       minimized;
    int       maximized;
    int       focused;
    int32_t   x, y;             /* outer top-left (titlebar) */
    uint32_t  w, h;             /* outer size */
    int32_t   restore_x, restore_y;
    uint32_t  restore_w, restore_h;
    uint32_t  flags;
    int       z;
    char      title[GUI_TITLE_MAX];
    uint32_t *back;             /* content buffer (content_w * content_h pixels) */
    uint32_t  back_w;           /* in pixels */
    uint32_t  back_h;
    /* per-window event ring */
    gui_event_t events[GUI_EVT_RING_SIZE];
    volatile uint32_t  evt_head, evt_tail;
    /* "owner" — pid that may receive events.  0 means kernel-owned. */
    int       owner_pid;
} gui_win_t;

static gui_win_t windows[GUI_MAX_WINDOWS];
static spinlock_t gui_lock;
static int focused = -1;
static gui_cursor_t cursor = GUI_CURSOR_ARROW;

/* Compositor dirty flag: 1 means screen will be redrawn next tick. */
static volatile int dirty = 1;
static uint64_t gui_clock_base_ticks = 0;
static uint64_t gui_clock_last_second = (uint64_t)-1;

/* Drag/resize state. */
static int  drag_wid = -1;
static int  drag_mode = 0;       /* 0=none, 1=move, 2=resize */
static int32_t drag_dx, drag_dy;
static uint32_t drag_orig_w, drag_orig_h;

/* Last hover state, for cursor change + double-click detection. */
static int last_hover_wid = -1;
static uint32_t last_click_tick = 0;
static int32_t  last_click_x = -999, last_click_y = -999;

/* ---- Theme colors ---- */
#define COL_DESKTOP_TOP     0x00102040
#define COL_DESKTOP_BOT     0x00000010
#define COL_WIN_BG          0x00ECEDF1
#define COL_WIN_CONTENT     0x00FFFFFF
#define COL_TITLE_ACTIVE    0x002F60C0
#define COL_TITLE_INACTIVE  0x00606870
#define COL_TITLE_TEXT      0x00FFFFFF
#define COL_BORDER          0x00303540
#define COL_BORDER_ACTIVE   0x00204080
#define COL_TASKBAR         0x00202028
#define COL_TASKBAR_TEXT    0x00DDEEFF
#define COL_CLOSE_BG        0x00C04040
#define COL_MAX_BG          0x004CAF50
#define COL_MIN_BG          0x00FFC107

/* ---- Public init ---- */
void gui_init(void) {
    memset(windows, 0, sizeof(windows));
    spinlock_init(&gui_lock);
    focused = -1;
    cursor  = GUI_CURSOR_ARROW;
    gui_clock_base_ticks = timer_get_ticks();
    gui_clock_last_second = (uint64_t)-1;
    dirty   = 1;
}

/* ---- Z-order helpers ---- */
static void recompute_focus(void) {
    int top = -1;
    int topz = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].visible && !windows[i].minimized) {
            if (windows[i].z > topz && !(windows[i].flags & GUI_WIN_NO_DECOR)) {
                topz = windows[i].z;
                top = i;
            }
        }
    }
    if (top != focused) {
        if (focused >= 0 && windows[focused].in_use) {
            windows[focused].focused = 0;
            gui_event_t e = { GUI_EVT_BLUR, 0,0,0,0,0,0 };
            gui_post_event(focused, &e);
        }
        focused = top;
        if (focused >= 0) {
            windows[focused].focused = 1;
            gui_event_t e = { GUI_EVT_FOCUS, 0,0,0,0,0,0 };
            gui_post_event(focused, &e);
        }
    }
}

int gui_raise_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    spinlock_acquire(&gui_lock);
    int maxz = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].in_use && windows[i].z > maxz) maxz = windows[i].z;
    windows[wid].z = maxz + 1;
    recompute_focus();
    dirty = 1;
    spinlock_release(&gui_lock);
    return 0;
}

int gui_focus_window(int wid) { return gui_raise_window(wid); }

/* ---- Window lifecycle ---- */
static uint32_t content_w(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->w;
    return w->w - 2 * GUI_BORDER;
}
static uint32_t content_h(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->h;
    return w->h - GUI_TITLEBAR_H - 2 * GUI_BORDER;
}
static int32_t content_x(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->x;
    return w->x + GUI_BORDER;
}
static int32_t content_y(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->y;
    return w->y + GUI_TITLEBAR_H + GUI_BORDER;
}

int gui_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                      const char *title, uint32_t flags) {
    if (w == 0 || h == 0) return -1;
    spinlock_acquire(&gui_lock);
    int id = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) { id = i; break; }
    }
    if (id < 0) { spinlock_release(&gui_lock); return -1; }
    gui_win_t *win = &windows[id];
    memset(win, 0, sizeof(*win));
    win->in_use = 1;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->flags = flags;
    tcb_t *owner = sched_current();
    win->owner_pid = owner ? (int)owner->id : 0;
    win->visible = 0;
    win->minimized = 0;
    win->maximized = 0;
    win->z = id;
    if (title) {
        strncpy(win->title, title, GUI_TITLE_MAX - 1);
        win->title[GUI_TITLE_MAX - 1] = 0;
    }
    /* Allocate back buffer sized to the *content* area (not the whole window;
     * decorations are drawn by the compositor). */
    uint32_t bw = content_w(win), bh = content_h(win);
    if (bw == 0 || bh == 0) {
        spinlock_release(&gui_lock);
        return -1;
    }
    win->back_w = bw;
    win->back_h = bh;
    win->back = (uint32_t *)kmalloc((size_t)bw * bh * 4);
    if (!win->back) {
        win->in_use = 0;
        spinlock_release(&gui_lock);
        return -1;
    }
    for (uint32_t i = 0; i < bw * bh; i++) win->back[i] = COL_WIN_CONTENT;
    dirty = 1;
    spinlock_release(&gui_lock);
    return id;
}

int gui_destroy_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    spinlock_acquire(&gui_lock);
    if (windows[wid].back) kfree(windows[wid].back);
    memset(&windows[wid], 0, sizeof(windows[wid]));
    if (focused == wid) focused = -1;
    if (drag_wid == wid) { drag_wid = -1; drag_mode = 0; }
    recompute_focus();
    dirty = 1;
    spinlock_release(&gui_lock);
    return 0;
}

int gui_window_owned_by(int wid, uint64_t owner_pid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return 0;
    return (uint64_t)(uint32_t)windows[wid].owner_pid == owner_pid;
}

uint64_t gui_window_owner(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return (uint64_t)-1;
    return (uint64_t)(uint32_t)windows[wid].owner_pid;
}

void gui_cleanup_process(uint64_t owner_pid) {
    /* PID 0 is kmain/kernel-owned in AuraLite; keep those demo windows until
     * explicit destruction. */
    if (owner_pid == 0) return;

    int cleaned = 0;
    spinlock_acquire(&gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        if ((uint64_t)(uint32_t)windows[i].owner_pid != owner_pid) continue;
        if (windows[i].back) kfree(windows[i].back);
        memset(&windows[i], 0, sizeof(windows[i]));
        if (focused == i) focused = -1;
        if (drag_wid == i) { drag_wid = -1; drag_mode = 0; }
        cleaned++;
    }
    if (cleaned) {
        recompute_focus();
        dirty = 1;
    }
    spinlock_release(&gui_lock);

    if (cleaned) {
        kprintf("[gui] cleaned %d window(s) for pid %llu\n",
                cleaned, (unsigned long long)owner_pid);
    }
}

int gui_show_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    windows[wid].visible = 1;
    windows[wid].minimized = 0;
    gui_raise_window(wid);
    return 0;
}
int gui_hide_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    windows[wid].visible = 0;
    dirty = 1;
    if (focused == wid) { focused = -1; recompute_focus(); }
    return 0;
}
int gui_move_window(int wid, int32_t x, int32_t y) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    windows[wid].x = x;
    windows[wid].y = y;
    dirty = 1;
    return 0;
}

int gui_resize_window(int wid, uint32_t w, uint32_t h) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    if (w < 60 || h < 40) return -1;
    spinlock_acquire(&gui_lock);
    gui_win_t *win = &windows[wid];
    win->w = w; win->h = h;
    uint32_t bw = content_w(win), bh = content_h(win);
    if (bw != win->back_w || bh != win->back_h) {
        uint32_t *nb = (uint32_t *)kmalloc((size_t)bw * bh * 4);
        if (!nb) { spinlock_release(&gui_lock); return -1; }
        for (uint32_t i = 0; i < bw * bh; i++) nb[i] = COL_WIN_CONTENT;
        /* Copy overlapping region. */
        uint32_t cw = bw < win->back_w ? bw : win->back_w;
        uint32_t ch = bh < win->back_h ? bh : win->back_h;
        for (uint32_t row = 0; row < ch; row++) {
            memcpy(nb + row * bw, win->back + row * win->back_w, cw * 4);
        }
        if (win->back) kfree(win->back);
        win->back   = nb;
        win->back_w = bw;
        win->back_h = bh;
    }
    /* Post resize event to client. */
    gui_event_t e = { GUI_EVT_RESIZE, 0,0,0,0,0,0 };
    e.x = (int32_t)bw; e.y = (int32_t)bh;
    spinlock_release(&gui_lock);
    gui_post_event(wid, &e);
    dirty = 1;
    return 0;
}

int gui_set_title(int wid, const char *title) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    if (title) {
        strncpy(windows[wid].title, title, GUI_TITLE_MAX - 1);
        windows[wid].title[GUI_TITLE_MAX - 1] = 0;
    } else {
        windows[wid].title[0] = 0;
    }
    dirty = 1;
    return 0;
}

int gui_minimize_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    windows[wid].minimized = 1;
    dirty = 1;
    if (focused == wid) { focused = -1; recompute_focus(); }
    return 0;
}
int gui_maximize_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    gui_win_t *w = &windows[wid];
    if (w->maximized) return 0;
    w->restore_x = w->x; w->restore_y = w->y;
    w->restore_w = w->w; w->restore_h = w->h;
    w->x = 0; w->y = 0;
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height() - GUI_TASKBAR_H;
    gui_resize_window(wid, fw, fh);
    w->maximized = 1;
    dirty = 1;
    return 0;
}
int gui_restore_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    gui_win_t *w = &windows[wid];
    if (!w->maximized) { w->minimized = 0; dirty = 1; return 0; }
    w->x = w->restore_x; w->y = w->restore_y;
    gui_resize_window(wid, w->restore_w, w->restore_h);
    w->maximized = 0;
    w->minimized = 0;
    dirty = 1;
    return 0;
}

int gui_get_window_size(int wid, uint32_t *w, uint32_t *h) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    if (w) *w = windows[wid].back_w;
    if (h) *h = windows[wid].back_h;
    return 0;
}

uint32_t *gui_window_buffer(int wid, uint32_t *out_pitch) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return NULL;
    if (out_pitch) *out_pitch = windows[wid].back_w;
    return windows[wid].back;
}

int gui_invalidate_window(int wid) {
    (void)wid;
    dirty = 1;
    return 0;
}
int gui_invalidate_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    (void)wid; (void)x; (void)y; (void)w; (void)h;
    dirty = 1; /* per-rect tracking is a future optimisation */
    return 0;
}

void gui_request_redraw(void) { dirty = 1; }

void gui_set_cursor(gui_cursor_t c) { cursor = c; dirty = 1; }

/* ---- Drawing primitives (write into window back buffer) ---- */

static int win_alive(int wid) {
    return wid >= 0 && wid < GUI_MAX_WINDOWS && windows[wid].in_use;
}

static int32_t abs_diff(int32_t a, int32_t b) { return a > b ? a - b : b - a; }

int gui_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    if (x < 0 || y < 0 || (uint32_t)x >= w->back_w || (uint32_t)y >= w->back_h) return 0;
    w->back[y * (int32_t)w->back_w + x] = color;
    return 0;
}

int gui_fill_rect(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    if (x < 0) { if (W <= (uint32_t)(-x)) return 0; W -= (uint32_t)(-x); x = 0; }
    if (y < 0) { if (H <= (uint32_t)(-y)) return 0; H -= (uint32_t)(-y); y = 0; }
    if ((uint32_t)x >= w->back_w || (uint32_t)y >= w->back_h) return 0;
    if ((uint32_t)x + W > w->back_w) W = w->back_w - (uint32_t)x;
    if ((uint32_t)y + H > w->back_h) H = w->back_h - (uint32_t)y;
    for (uint32_t row = 0; row < H; row++) {
        uint32_t *p = w->back + ((uint32_t)y + row) * w->back_w + (uint32_t)x;
        for (uint32_t col = 0; col < W; col++) p[col] = color;
    }
    dirty = 1;
    return 0;
}

int gui_draw_rect(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H, uint32_t color) {
    if (W == 0 || H == 0) return 0;
    gui_fill_rect(wid, x,         y,         W, 1, color);
    gui_fill_rect(wid, x,         y + (int32_t)H - 1, W, 1, color);
    gui_fill_rect(wid, x,         y,         1, H, color);
    gui_fill_rect(wid, x + (int32_t)W - 1, y, 1, H, color);
    return 0;
}

int gui_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (!win_alive(wid)) return -1;
    int dx =  ((x1 > x0) ? (x1 - x0) : (x0 - x1));
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gui_draw_pixel(wid, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2*err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    dirty = 1;
    return 0;
}

int gui_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color) {
    if (!win_alive(wid)) return -1;
    int32_t cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += 8; continue; }
        const char *glyph = font8x8_basic[(unsigned char)*s & 0x7F];
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                if (glyph[gy] & (1 << gx)) gui_draw_pixel(wid, cx + gx, y + gy, color);
            }
        }
        cx += 8;
    }
    dirty = 1;
    return 0;
}

int gui_clear(int wid, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    for (uint32_t i = 0; i < w->back_w * w->back_h; i++) w->back[i] = color;
    dirty = 1;
    return 0;
}

int gui_blit(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H,
             const uint32_t *src, uint32_t src_stride) {
    if (!win_alive(wid) || !src) return -1;
    gui_win_t *w = &windows[wid];
    for (uint32_t row = 0; row < H; row++) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= w->back_h) continue;
        for (uint32_t col = 0; col < W; col++) {
            int32_t dx = x + (int32_t)col;
            if (dx < 0 || (uint32_t)dx >= w->back_w) continue;
            w->back[(uint32_t)dy * w->back_w + (uint32_t)dx] = src[row * src_stride + col];
        }
    }
    dirty = 1;
    return 0;
}

/* ---- Event ring ---- */
int gui_post_event(int wid, const gui_event_t *evt) {
    if (!win_alive(wid) || !evt) return -1;
    gui_win_t *w = &windows[wid];
    uint32_t next = (w->evt_head + 1) % GUI_EVT_RING_SIZE;
    if (next == w->evt_tail) {
        /* Ring full — drop oldest. */
        w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    }
    w->events[w->evt_head] = *evt;
    w->evt_head = next;
    return 0;
}
int gui_poll_event(int wid, gui_event_t *out) {
    if (!win_alive(wid) || !out) return 0;
    gui_win_t *w = &windows[wid];
    if (w->evt_head == w->evt_tail) return 0;
    *out = w->events[w->evt_tail];
    w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    return 1;
}
int gui_wait_event(int wid, gui_event_t *out) {
    while (!gui_poll_event(wid, out)) {
        sched_yield();
        if (!win_alive(wid)) return -1;
    }
    return 1;
}

/* ---- Compositor ---- */

extern struct limine_framebuffer *limine_get_framebuffer(void);

/* Directly access the visible framebuffer via gfx_* — gui owns its own
 * front-render path because we compose lots of small back buffers. */

static void blit_window_decor(const gui_win_t *win) {
    int active = win->focused;
    uint32_t border = active ? COL_BORDER_ACTIVE : COL_BORDER;
    uint32_t titlec = active ? COL_TITLE_ACTIVE  : COL_TITLE_INACTIVE;

    /* Shadow. */
    gfx_fill_rect((uint32_t)win->x + 3, (uint32_t)win->y + 3, win->w, win->h, 0x00080810);

    if (win->flags & GUI_WIN_NO_DECOR) {
        return; /* content drawn separately */
    }

    /* Border (full window). */
    gfx_fill_rect((uint32_t)win->x, (uint32_t)win->y, win->w, win->h, border);

    /* Title bar. */
    gfx_fill_rect((uint32_t)win->x + GUI_BORDER, (uint32_t)win->y + GUI_BORDER,
                  win->w - 2 * GUI_BORDER, GUI_TITLEBAR_H, titlec);
    /* Title text (clipped). */
    gfx_draw_text((uint32_t)win->x + 8, (uint32_t)win->y + GUI_BORDER + 7,
                  win->title, COL_TITLE_TEXT);

    /* Buttons: [_][O][X] right-aligned. */
    if (win->flags & GUI_WIN_HAS_CLOSE) {
        uint32_t bx = (uint32_t)win->x + win->w - GUI_BORDER - 22;
        uint32_t by = (uint32_t)win->y + GUI_BORDER + 3;
        gfx_fill_rect(bx, by, 18, 16, COL_CLOSE_BG);
        gfx_draw_text(bx + 5, by + 4, "X", 0x00FFFFFF);
    }
    if (win->flags & GUI_WIN_HAS_MINMAX) {
        uint32_t bx = (uint32_t)win->x + win->w - GUI_BORDER - 44;
        uint32_t by = (uint32_t)win->y + GUI_BORDER + 3;
        gfx_fill_rect(bx, by, 18, 16, COL_MAX_BG);
        gfx_draw_text(bx + 5, by + 4, win->maximized ? "R" : "O", 0x00FFFFFF);
        gfx_fill_rect(bx - 22, by, 18, 16, COL_MIN_BG);
        gfx_draw_text(bx - 22 + 5, by + 4, "_", 0x00000000);
    }

    /* Resize grip. */
    if ((win->flags & GUI_WIN_RESIZABLE) && !win->maximized) {
        uint32_t gx = (uint32_t)win->x + win->w - GUI_RESIZE_GRIP - 1;
        uint32_t gy = (uint32_t)win->y + win->h - GUI_RESIZE_GRIP - 1;
        for (int i = 0; i < GUI_RESIZE_GRIP; i += 3) {
            gfx_draw_line(gx + i, gy + GUI_RESIZE_GRIP - 1, gx + GUI_RESIZE_GRIP - 1, gy + i, 0x00FFFFFF);
        }
    }
}

static void blit_window_content(const gui_win_t *win) {
    int32_t cx = content_x(win);
    int32_t cy = content_y(win);
    /* Clip to screen. */
    int32_t fw = (int32_t)gfx_get_width();
    int32_t fh = (int32_t)gfx_get_height();
    for (uint32_t row = 0; row < win->back_h; row++) {
        int32_t py = cy + (int32_t)row;
        if (py < 0 || py >= fh) continue;
        for (uint32_t col = 0; col < win->back_w; col++) {
            int32_t px = cx + (int32_t)col;
            if (px < 0 || px >= fw) continue;
            gfx_putpixel((uint32_t)px, (uint32_t)py, win->back[row * win->back_w + col]);
        }
    }
}

static void draw_desktop(void) {
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    /* Vertical gradient. */
    gfx_gradient_v(0, 0, w, h, COL_DESKTOP_TOP, COL_DESKTOP_BOT);

    /* Faint diagonal "stripes" for texture (cheap pattern). */
    for (uint32_t y = 0; y < h; y += 32) {
        gfx_draw_line(0, y, w, y, 0x00000020);
    }
}

static void draw_cursor(void) {
    int mx, my;
    if (!mouse_get_position(&mx, &my)) return;
    switch (cursor) {
        case GUI_CURSOR_ARROW:
        default: {
            /* Simple filled arrow + outline. */
            for (int i = 0; i < 14; i++) {
                for (int j = 0; j <= i / 2; j++) {
                    gfx_putpixel((uint32_t)(mx + j), (uint32_t)(my + i), 0x00FFFFFF);
                }
            }
            /* Outline */
            gfx_draw_line((uint32_t)mx, (uint32_t)my,         (uint32_t)mx, (uint32_t)my + 13, 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)my + 13,    (uint32_t)mx + 6, (uint32_t)my + 9, 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)my,         (uint32_t)mx + 9, (uint32_t)my + 9, 0x00000000);
            break;
        }
        case GUI_CURSOR_IBEAM:
            for (int i = -7; i <= 7; i++) gfx_putpixel((uint32_t)mx, (uint32_t)(my + i), 0x00000000);
            gfx_draw_line((uint32_t)mx - 3, (uint32_t)my - 7, (uint32_t)mx + 3, (uint32_t)my - 7, 0x00000000);
            gfx_draw_line((uint32_t)mx - 3, (uint32_t)my + 7, (uint32_t)mx + 3, (uint32_t)my + 7, 0x00000000);
            break;
        case GUI_CURSOR_HAND:
            gfx_fill_rect((uint32_t)mx - 3, (uint32_t)my, 6, 10, 0x00FFFFFF);
            gfx_draw_rect((uint32_t)mx - 3, (uint32_t)my, 6, 10, 0x00000000);
            break;
        case GUI_CURSOR_HRESIZE:
            gfx_draw_line((uint32_t)mx - 7, (uint32_t)my, (uint32_t)mx + 7, (uint32_t)my, 0x00FFFFFF);
            gfx_draw_line((uint32_t)mx - 7, (uint32_t)my - 1, (uint32_t)mx + 7, (uint32_t)my - 1, 0x00000000);
            gfx_draw_line((uint32_t)mx - 7, (uint32_t)my + 1, (uint32_t)mx + 7, (uint32_t)my + 1, 0x00000000);
            break;
        case GUI_CURSOR_VRESIZE:
            gfx_draw_line((uint32_t)mx, (uint32_t)my - 7, (uint32_t)mx, (uint32_t)my + 7, 0x00FFFFFF);
            break;
        case GUI_CURSOR_DRESIZE:
            for (int i = -6; i <= 6; i++) gfx_putpixel((uint32_t)(mx + i), (uint32_t)(my + i), 0x00FFFFFF);
            break;
        case GUI_CURSOR_WAIT:
            gfx_fill_circle((uint32_t)mx, (uint32_t)my, 5, 0x00FFFFFF);
            gfx_draw_circle((uint32_t)mx, (uint32_t)my, 6, 0x00000000);
            break;
    }
}

static void draw_taskbar(void) {
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    uint32_t tb_y = h - GUI_TASKBAR_H;

    gfx_fill_rect(0, tb_y, w, GUI_TASKBAR_H, COL_TASKBAR);
    gfx_draw_line(0, tb_y, w, tb_y, 0x00404050);

    /* Start button. */
    gfx_fill_rect(4, tb_y + 4, 80, GUI_TASKBAR_H - 8, 0x004080C0);
    gfx_draw_rect(4, tb_y + 4, 80, GUI_TASKBAR_H - 8, 0x00FFFFFF);
    gfx_draw_text(14, tb_y + 10, "AuraLite", COL_TASKBAR_TEXT);

    /* Window list. */
    uint32_t bx = 96;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & GUI_WIN_NO_DECOR) continue;
        if (bx + 130 > w - 80) break;
        uint32_t col = (i == focused) ? 0x004060A0 : 0x00303840;
        if (windows[i].minimized) col = 0x00202830;
        gfx_fill_rect(bx, tb_y + 4, 124, GUI_TASKBAR_H - 8, col);
        gfx_draw_rect(bx, tb_y + 4, 124, GUI_TASKBAR_H - 8, 0x00606878);
        char title_short[16];
        int n = 0;
        while (n < 15 && windows[i].title[n]) { title_short[n] = windows[i].title[n]; n++; }
        title_short[n] = 0;
        gfx_draw_text(bx + 6, tb_y + 10, title_short, COL_TASKBAR_TEXT);
        bx += 128;
    }

    /* Clock + GUI uptime, right side.  Use the configured PIT frequency and a
     * GUI-start baseline so it begins at 00:00:00 instead of freezing at the
     * kernel boot-time offset. */
    uint64_t ticks = timer_get_ticks();
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t elapsed_ticks = (ticks >= gui_clock_base_ticks) ?
        (ticks - gui_clock_base_ticks) : ticks;
    uint64_t s = elapsed_ticks / hz;
    uint32_t mm = (uint32_t)((s / 60) % 60);
    uint32_t hh = (uint32_t)((s / 3600) % 24);
    uint32_t ss = (uint32_t)(s % 60);
    char clk[16];
    int p = 0;
    clk[p++] = '0' + (hh / 10) % 10;
    clk[p++] = '0' + hh % 10;
    clk[p++] = ':';
    clk[p++] = '0' + (mm / 10) % 10;
    clk[p++] = '0' + mm % 10;
    clk[p++] = ':';
    clk[p++] = '0' + (ss / 10) % 10;
    clk[p++] = '0' + ss % 10;
    clk[p]   = 0;
    gfx_draw_text(w - 80, tb_y + 10, clk, COL_TASKBAR_TEXT);
}

/* Recompose entire screen. */
static void compositor_render(void) {
    draw_desktop();
    /* Build z-sorted index of visible windows. */
    int order[GUI_MAX_WINDOWS], n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].visible && !windows[i].minimized)
            order[n++] = i;
    }
    /* Simple insertion sort by z ascending. */
    for (int i = 1; i < n; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && windows[order[j]].z > windows[v].z) {
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        blit_window_decor(&windows[order[i]]);
        blit_window_content(&windows[order[i]]);
    }
    draw_taskbar();
    draw_cursor();
    gfx_flip();
}

/* ---- Hit-test ---- */
static int hit_window(int32_t mx, int32_t my) {
    int best = -1, bestz = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible || windows[i].minimized) continue;
        gui_win_t *w = &windows[i];
        if (mx >= w->x && mx < (int32_t)(w->x + w->w) &&
            my >= w->y && my < (int32_t)(w->y + w->h)) {
            if (w->z > bestz) { bestz = w->z; best = i; }
        }
    }
    return best;
}
/* 0 = client, 1 = titlebar, 2 = close btn, 3 = max btn, 4 = min btn,
 * 5 = resize grip, 6 = outside */
static int hit_part(const gui_win_t *w, int32_t mx, int32_t my) {
    if (w->flags & GUI_WIN_NO_DECOR) return 0;
    if (mx < w->x || mx >= (int32_t)(w->x + w->w) ||
        my < w->y || my >= (int32_t)(w->y + w->h)) return 6;
    /* Resize grip. */
    if ((w->flags & GUI_WIN_RESIZABLE) && !w->maximized) {
        int32_t gx = w->x + (int32_t)w->w - GUI_RESIZE_GRIP - 1;
        int32_t gy = w->y + (int32_t)w->h - GUI_RESIZE_GRIP - 1;
        if (mx >= gx && my >= gy) return 5;
    }
    if (my < w->y + GUI_BORDER + GUI_TITLEBAR_H) {
        /* in title bar */
        if (w->flags & GUI_WIN_HAS_CLOSE) {
            int32_t bx = w->x + (int32_t)w->w - GUI_BORDER - 22;
            int32_t by = w->y + GUI_BORDER + 3;
            if (mx >= bx && mx < bx + 18 && my >= by && my < by + 16) return 2;
        }
        if (w->flags & GUI_WIN_HAS_MINMAX) {
            int32_t bx = w->x + (int32_t)w->w - GUI_BORDER - 44;
            int32_t by = w->y + GUI_BORDER + 3;
            if (mx >= bx && mx < bx + 18 && my >= by && my < by + 16) return 3;
            if (mx >= bx - 22 && mx < bx - 4 && my >= by && my < by + 16) return 4;
        }
        return 1;
    }
    return 0;
}

/* ---- Input → event routing ---- */
static int taskbar_hit(int32_t mx, int32_t my) {
    uint32_t h = gfx_get_height();
    if ((uint32_t)my < h - GUI_TASKBAR_H) return -1;
    /* Window list buttons in same layout as draw_taskbar. */
    uint32_t bx = 96;
    uint32_t w = gfx_get_width();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & GUI_WIN_NO_DECOR) continue;
        if (bx + 130 > w - 80) break;
        if ((uint32_t)mx >= bx && (uint32_t)mx < bx + 124) return i;
        bx += 128;
    }
    return -1;
}

static void route_mouse_event(const mouse_event_t *ev) {
    int32_t mx = ev->abs_x, my = ev->abs_y;
    int wid = hit_window(mx, my);
    gui_cursor_t target_cursor = GUI_CURSOR_ARROW;

    /* Drag/resize already active? */
    if (drag_mode) {
        gui_win_t *w = &windows[drag_wid];
        if (!win_alive(drag_wid)) { drag_mode = 0; }
        else if (drag_mode == 1) {
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            dirty = 1;
            target_cursor = GUI_CURSOR_ARROW;
        } else if (drag_mode == 2) {
            int32_t nw = (int32_t)drag_orig_w + (mx - drag_dx);
            int32_t nh = (int32_t)drag_orig_h + (my - drag_dy);
            if (nw < 80) nw = 80;
            if (nh < 60) nh = 60;
            gui_resize_window(drag_wid, (uint32_t)nw, (uint32_t)nh);
            target_cursor = GUI_CURSOR_DRESIZE;
            dirty = 1;
        }
        if (ev->released & MOUSE_BTN_LEFT) {
            drag_mode = 0;
            drag_wid  = -1;
            dirty = 1;
        }
        gui_set_cursor(target_cursor);
        return;
    }

    if (wid < 0) {
        /* Check taskbar. */
        int tb = taskbar_hit(mx, my);
        if (ev->pressed & MOUSE_BTN_LEFT) {
            if (tb >= 0) {
                if (windows[tb].minimized) gui_restore_window(tb);
                else if (focused == tb)    gui_minimize_window(tb);
                else                       gui_focus_window(tb);
                dirty = 1;
            }
        }
        gui_set_cursor(GUI_CURSOR_ARROW);
        last_hover_wid = -1;
        return;
    }

    gui_win_t *w = &windows[wid];
    int part = hit_part(w, mx, my);

    /* Cursor shape. */
    if (part == 5) target_cursor = GUI_CURSOR_DRESIZE;
    else target_cursor = GUI_CURSOR_ARROW;
    gui_set_cursor(target_cursor);

    if (ev->pressed & MOUSE_BTN_LEFT) {
        gui_focus_window(wid);
        if (part == 2) {
            /* Close: post CLOSE_REQ. */
            gui_event_t e = { GUI_EVT_CLOSE_REQ, 0,0,0,0,0,0 };
            gui_post_event(wid, &e);
            return;
        }
        if (part == 3) {
            if (w->maximized) gui_restore_window(wid);
            else              gui_maximize_window(wid);
            return;
        }
        if (part == 4) {
            gui_minimize_window(wid);
            return;
        }
        if (part == 1) {
            /* Start drag. */
            if (!w->maximized && (w->flags & GUI_WIN_MOVABLE)) {
                drag_wid  = wid;
                drag_mode = 1;
                drag_dx   = mx - w->x;
                drag_dy   = my - w->y;
            }
            return;
        }
        if (part == 5) {
            drag_wid    = wid;
            drag_mode   = 2;
            drag_orig_w = w->w;
            drag_orig_h = w->h;
            drag_dx     = mx;
            drag_dy     = my;
            return;
        }
        /* Client area → forward to window with local coords. */
        uint32_t now = (uint32_t)timer_get_ticks();
        int dbl = (last_click_tick && now - last_click_tick < MOUSE_DBLCLICK_TICKS
                   && abs_diff(last_click_x, mx) < 5
                   && abs_diff(last_click_y, my) < 5);
        last_click_tick = now;
        last_click_x    = mx;
        last_click_y    = my;
        gui_event_t e = {0};
        e.type     = dbl ? GUI_EVT_MOUSE_DBLCLICK : GUI_EVT_MOUSE_DOWN;
        e.x = mx - content_x(w);
        e.y = my - content_y(w);
        e.buttons  = ev->buttons;
        e.mods     = keyboard_get_mods();
        gui_post_event(wid, &e);
        return;
    }
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
    if (ev->wheel) {
        gui_event_t e = { GUI_EVT_MOUSE_WHEEL, 0,0,0,0,0,0 };
        e.x = mx - content_x(w);
        e.y = my - content_y(w);
        e.key = (uint32_t)(int32_t)ev->wheel;
        gui_post_event(wid, &e);
        return;
    }
    /* Plain move: only post if window expects move events (always for now). */
    if (ev->dx || ev->dy) {
        gui_event_t e = { GUI_EVT_MOUSE_MOVE, 0,0,0,0,0,0 };
        e.x = mx - content_x(w);
        e.y = my - content_y(w);
        e.buttons = ev->buttons;
        gui_post_event(wid, &e);
    }
}

/* Helper. */

static void route_key_event(const kb_event_t *ke) {
    if (focused < 0) return;
    gui_event_t e = {0};
    e.type = ke->pressed ? GUI_EVT_KEY_DOWN : GUI_EVT_KEY_UP;
    e.key  = ke->key;
    e.mods = ke->mods;
    gui_post_event(focused, &e);
}

/* Compositor "tick": pump inputs into events, recompose if dirty. */
void gui_compositor_tick(void) {
    mouse_event_t me;
    while (mouse_get_event(&me)) route_mouse_event(&me);
    kb_event_t ke;
    while (keyboard_get_event(&ke)) route_key_event(&ke);

    /* The taskbar clock changes even when there is no mouse/keyboard/window
     * activity, so force one redraw per second. */
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t ticks = timer_get_ticks();
    uint64_t elapsed_ticks = (ticks >= gui_clock_base_ticks) ?
        (ticks - gui_clock_base_ticks) : ticks;
    uint64_t now_sec = elapsed_ticks / hz;
    if (now_sec != gui_clock_last_second) {
        gui_clock_last_second = now_sec;
        dirty = 1;
    }

    if (dirty) {
        dirty = 0;
        compositor_render();
    }
}

void gui_render_now(void) {
    dirty = 0;
    compositor_render();
}

/* Dedicated kernel thread that runs the compositor + input pump forever. */
void gui_compositor_thread(void *arg) {
    (void)arg;
    /* From this point on the GUI owns the framebuffer; mute the text console
     * so kprintf no longer scribbles on top of windows (UART output is kept
     * for serial logging and integration tests). */
    fb_set_console_enabled(0);
    dirty = 1;
    for (;;) {
        gui_compositor_tick();
        timer_sleep_ms(33);
    }
}

/* ---- Self-test ---- */
void gui_self_test(void) {
    kprintf("[gui] self-test: creating test windows...\n");
    int w1 = gui_create_window(80, 60, 360, 200, "Hello GUI", GUI_WIN_DEFAULT);
    int w2 = gui_create_window(460, 100, 300, 220, "Second Window", GUI_WIN_DEFAULT);
    if (w1 < 0 || w2 < 0) {
        kprintf("[gui] FAIL: could not create windows\n");
        return;
    }
    gui_clear(w1, 0x00F8F8FF);
    gui_draw_text(w1, 12, 16, "AuraLite GUI subsystem online.", 0x00000000);
    gui_draw_text(w1, 12, 32, "Try clicking, dragging, resizing!", 0x00404080);
    gui_fill_rect(w1, 12, 60, 100, 30, 0x004080C0);
    gui_draw_rect(w1, 12, 60, 100, 30, 0x00000000);
    gui_draw_text(w1, 30, 70, "Button", 0x00FFFFFF);

    gui_clear(w2, 0x00FFFFE8);
    gui_draw_text(w2, 12, 16, "Second window!", 0x00800000);
    gui_draw_text(w2, 12, 32, "Drag, resize, focus.", 0x00400040);

    gui_show_window(w1);
    gui_show_window(w2);
    gui_render_now();
    kprintf("[gui] PASS: GUI subsystem rendered initial composition\n");
}
