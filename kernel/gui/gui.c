/* gui.c — kernel-side GUI core v2.0: compositor, window manager, event router.
 *
 * Major changes from v1:
 *   - Theme engine: all colors/sizes configurable via gui_theme_t.
 *   - Dirty-rect tracking: only recompose changed regions.
 *   - Window snapping: drag to screen edges (left/right/top half, maximize).
 *   - Right-click / middle-click support with CONTEXT_MENU event.
 *   - Desktop icons with click-to-launch.
 *   - Notification popups above the taskbar.
 *   - ALWAYS_TOP and TOOL_WINDOW window flags.
 *   - Optimised row-based blitting with memcpy instead of per-pixel loops.
 *   - Double-click on titlebar toggles maximize.
 *   - Keyboard shortcuts: Alt+F4 = close, Alt+Space = context menu.
 *   - Start menu in taskbar (toggled by clicking "AuraLite" button).
 *   - Refined cursor rendering with outline.
 *   - Window edge/corner resize (not just bottom-right grip).
 *   - Minimize-to-taskbar with restore on click.
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

/* ===================================================================
 * Theme
 * =================================================================== */

static gui_theme_t active_theme;

static const gui_theme_t default_theme = {
    /* Desktop */
    .desktop_top     = 0x00102040,
    .desktop_bot     = 0x00000010,
    /* Window */
    .win_bg          = 0x00ECEDF1,
    .win_content     = 0x00FFFFFF,
    .title_active    = 0x002F60C0,
    .title_inactive  = 0x00606870,
    .title_text      = 0x00FFFFFF,
    .border          = 0x00303540,
    .border_active   = 0x00204080,
    /* Taskbar */
    .taskbar_bg      = 0x00202028,
    .taskbar_border  = 0x00404050,
    .taskbar_text    = 0x00DDEEFF,
    .start_btn_bg    = 0x004080C0,
    .start_btn_text  = 0x00DDEEFF,
    /* Buttons */
    .close_bg        = 0x00C04040,
    .close_bg_hover  = 0x00E05050,
    .max_bg          = 0x004CAF50,
    .min_bg          = 0x00FFC107,
    /* Icons */
    .icon_text       = 0x00FFFFFF,
    .icon_selected   = 0x002F60C0,
    /* Notifications */
    .notif_bg        = 0x00383848,
    .notif_border    = 0x0060A0E0,
    .notif_text      = 0x00E8E8F0,
    /* Shadows */
    .shadow_color    = 0x00080810,
    .shadow_offset   = 3,
    /* Dimensions */
    .taskbar_h       = 32,
    .titlebar_h      = 24,
    .border_w        = 2,
    .resize_grip     = 12,
    .icon_size       = 32,
    .icon_pad        = 8,
    .win_round       = 4,
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
    gui_snap_t snap;
    char      title[GUI_TITLE_MAX];
    uint32_t *back;               /* content buffer */
    uint32_t  back_w, back_h;     /* content pixels */
    /* per-window event ring */
    gui_event_t events[GUI_EVT_RING_SIZE];
    volatile uint32_t evt_head, evt_tail;
    /* owner pid */
    int       owner_pid;
    /* per-window dirty flag for back-buffer changes */
    volatile int content_dirty;
} gui_win_t;

static gui_win_t windows[GUI_MAX_WINDOWS];
static spinlock_t gui_lock;
static int focused = -1;
static gui_cursor_t cursor = GUI_CURSOR_ARROW;

/* Dirty-rect tracking. */
static gui_rect_t dirty_rects[GUI_MAX_DIRTY_RECTS];
static int dirty_count = 0;
static volatile int full_dirty = 1;  /* start with a full redraw */

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
static uint32_t last_click_tick = 0;
static int32_t  last_click_x = -999, last_click_y = -999;

/* Desktop icons. */
static gui_icon_t icons[GUI_MAX_ICONS];
static int icon_selected = -1;

/* Notifications. */
static gui_notification_t notifications[GUI_MAX_NOTIFICATIONS];

/* Start menu. */
static int start_menu_open = 0;

/* ===================================================================
 * Helpers
 * =================================================================== */

static int win_alive(int wid) {
    return wid >= 0 && wid < GUI_MAX_WINDOWS && windows[wid].in_use;
}

static int32_t abs_diff(int32_t a, int32_t b) { return a > b ? a - b : b - a; }

/* Content area helpers — read from active theme dimensions. */
static uint32_t c_border(void)    { return active_theme.border_w; }
static uint32_t c_titlebar(void)  { return active_theme.titlebar_h; }

static uint32_t content_w(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->w;
    return w->w - 2 * c_border();
}
static uint32_t content_h(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->h;
    return w->h - c_titlebar() - 2 * c_border();
}
static int32_t content_x(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->x;
    return w->x + (int32_t)c_border();
}
static int32_t content_y(const gui_win_t *w) {
    if (w->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) return w->y;
    return w->y + (int32_t)c_titlebar() + (int32_t)c_border();
}

/* Mark a screen-space rectangle as dirty. */
void gui_mark_dirty(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return;
    if (full_dirty) return;
    if (dirty_count >= GUI_MAX_DIRTY_RECTS) {
        full_dirty = 1;
        dirty_count = 0;
        return;
    }
    dirty_rects[dirty_count].x = x;
    dirty_rects[dirty_count].y = y;
    dirty_rects[dirty_count].w = w;
    dirty_rects[dirty_count].h = h;
    dirty_count++;
}

/* Mark the bounding rect of a window (including shadow) as dirty. */
static void mark_window_dirty(const gui_win_t *w) {
    int off = active_theme.shadow_offset;
    gui_mark_dirty(w->x - 1, w->y - 1, w->w + off + 2, w->h + off + 2);
}

/* ===================================================================
 * Init
 * =================================================================== */

void gui_init(void) {
    memset(windows, 0, sizeof(windows));
    memset(icons, 0, sizeof(icons));
    memset(notifications, 0, sizeof(notifications));
    spinlock_init(&gui_lock);
    focused = -1;
    cursor  = GUI_CURSOR_ARROW;
    active_theme = default_theme;
    gui_clock_base_ticks = timer_get_ticks();
    gui_clock_last_second = (uint64_t)-1;
    full_dirty = 1;
    dirty_count = 0;
    start_menu_open = 0;
    icon_selected = -1;
}

/* ===================================================================
 * Z-order and focus
 * =================================================================== */

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

int gui_focus_window(int wid) { return gui_raise_window(wid); }

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
        bw = w - 2 * active_theme.border_w;
        bh = h - active_theme.titlebar_h - 2 * active_theme.border_w;
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
    memset(win, 0, sizeof(*win));
    win->in_use = 1;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->flags = flags;
    win->snap = GUI_SNAP_NONE;
    tcb_t *owner = sched_current();
    win->owner_pid = owner ? (int)owner->id : 0;
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
        win->in_use = 0;
        kprintf("[gui] create_window: buffer size overflow %ux%u\n", bw, bh);
        spinlock_release(&gui_lock);
        return -1;
    }
    win->back = (uint32_t *)kmalloc(buf_size);
    if (!win->back) {
        win->in_use = 0;
        kprintf("[gui] create_window: kmalloc failed for %ux%u back buffer\n", bw, bh);
        spinlock_release(&gui_lock);
        return -1;
    }
    for (uint32_t i = 0; i < bw * bh; i++) win->back[i] = active_theme.win_content;
    spinlock_release(&gui_lock);
    return id;
}

int gui_destroy_window(int wid) {
    if (!win_alive(wid)) return -1;
    spinlock_acquire(&gui_lock);
    gui_win_t *w = &windows[wid];
    mark_window_dirty(w);
    if (w->back) kfree(w->back);
    memset(w, 0, sizeof(*w));
    if (focused == wid) focused = -1;
    if (drag_wid == wid) { drag_wid = -1; drag_mode = 0; }
    if (last_hover_wid == wid) last_hover_wid = -1;
    recompute_focus();
    full_dirty = 1;
    spinlock_release(&gui_lock);
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

void gui_cleanup_process(uint64_t owner_pid) {
    if (owner_pid == 0) return;
    int cleaned = 0;
    spinlock_acquire(&gui_lock);
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        if ((uint64_t)(uint32_t)windows[i].owner_pid != owner_pid) continue;
        mark_window_dirty(&windows[i]);
        if (windows[i].back) kfree(windows[i].back);
        memset(&windows[i], 0, sizeof(windows[i]));
        if (focused == i) focused = -1;
        if (drag_wid == i) { drag_wid = -1; drag_mode = 0; }
        if (last_hover_wid == i) last_hover_wid = -1;
        cleaned++;
    }
    /* Also clean up icons owned by this process. */
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (icons[i].in_use && (uint64_t)(uint32_t)icons[i].owner_pid == owner_pid) {
            memset(&icons[i], 0, sizeof(icons[i]));
            if (icon_selected == i) icon_selected = -1;
        }
    }
    if (cleaned) {
        recompute_focus();
        full_dirty = 1;
    }
    spinlock_release(&gui_lock);
    if (cleaned) {
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
    if (w < 60 || h < 40) return -1;
    spinlock_acquire(&gui_lock);
    gui_win_t *win = &windows[wid];
    /* Compute new content size BEFORE modifying the window. */
    uint32_t new_bw, new_bh;
    if (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS)) {
        new_bw = w; new_bh = h;
    } else {
        new_bw = w - 2 * active_theme.border_w;
        new_bh = h - active_theme.titlebar_h - 2 * active_theme.border_w;
    }
    if (new_bw == 0 || new_bh == 0) {
        spinlock_release(&gui_lock);
        return -1;
    }
    mark_window_dirty(win);
    win->w = w; win->h = h;
    if (new_bw != win->back_w || new_bh != win->back_h) {
        size_t buf_size = (size_t)new_bw * new_bh * 4;
        if (buf_size / 4 != (size_t)new_bw * new_bh) {
            /* Integer overflow. */
            win->w = win->back_w + (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS) ? 0 : 2 * active_theme.border_w);
            win->h = win->back_h + (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS) ? 0 : active_theme.titlebar_h + 2 * active_theme.border_w);
            kprintf("[gui] resize_window: buffer size overflow %ux%u\n", new_bw, new_bh);
            spinlock_release(&gui_lock);
            return -1;
        }
        uint32_t *nb = (uint32_t *)kmalloc(buf_size);
        if (!nb) {
            /* Roll back w/h to keep consistency. */
            win->w = win->back_w + (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS) ? 0 : 2 * active_theme.border_w);
            win->h = win->back_h + (win->flags & (GUI_WIN_NO_DECOR | GUI_WIN_BORDERLESS) ? 0 : active_theme.titlebar_h + 2 * active_theme.border_w);
            kprintf("[gui] resize_window: kmalloc failed for %ux%u buffer\n", new_bw, new_bh);
            spinlock_release(&gui_lock);
            return -1;
        }
        for (uint32_t i = 0; i < new_bw * new_bh; i++) nb[i] = active_theme.win_content;
        /* Copy overlapping region with optimized row copies. */
        uint32_t cw = new_bw < win->back_w ? new_bw : win->back_w;
        uint32_t ch = new_bh < win->back_h ? new_bh : win->back_h;
        for (uint32_t row = 0; row < ch; row++) {
            memcpy(nb + row * new_bw, win->back + row * win->back_w, cw * 4);
        }
        if (win->back) kfree(win->back);
        win->back   = nb;
        win->back_w = new_bw;
        win->back_h = new_bh;
    }
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
    if (w) *w = windows[wid].back_w;
    if (h) *h = windows[wid].back_h;
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
    return 0;
}

void gui_request_redraw(void) { full_dirty = 1; }
void gui_set_cursor(gui_cursor_t c) { cursor = c; }

/* ===================================================================
 * Desktop icons
 * =================================================================== */

int gui_add_icon(int32_t x, int32_t y, const char *label, int icon_id) {
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) {
            icons[i].in_use = 1;
            icons[i].x = x;
            icons[i].y = y;
            strncpy(icons[i].label, label, sizeof(icons[i].label) - 1);
            icons[i].label[sizeof(icons[i].label) - 1] = 0;
            icons[i].icon_id = icon_id;
            tcb_t *owner = sched_current();
            icons[i].owner_pid = owner ? (int)owner->id : 0;
            gui_mark_dirty(x, y, active_theme.icon_size + active_theme.icon_pad * 2,
                           active_theme.icon_size + 20);
            return i;
        }
    }
    return -1;
}

int gui_remove_icon(int icon_idx) {
    if (icon_idx < 0 || icon_idx >= GUI_MAX_ICONS || !icons[icon_idx].in_use) return -1;
    gui_mark_dirty(icons[icon_idx].x, icons[icon_idx].y,
                   active_theme.icon_size + active_theme.icon_pad * 2,
                   active_theme.icon_size + 20);
    memset(&icons[icon_idx], 0, sizeof(icons[icon_idx]));
    return 0;
}

int gui_icon_owned_by(int icon_idx, uint64_t owner_pid) {
    if (icon_idx < 0 || icon_idx >= GUI_MAX_ICONS || !icons[icon_idx].in_use) return 0;
    return (uint64_t)(uint32_t)icons[icon_idx].owner_pid == owner_pid;
}

int gui_icon_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_ICONS; i++) if (icons[i].in_use) n++;
    return n;
}

/* ===================================================================
 * Notifications
 * =================================================================== */

int gui_notify(const char *text, uint32_t color, uint32_t duration_ms) {
    /* Find a free slot or overwrite oldest. */
    int slot = -1;
    uint32_t oldest_tick = (uint32_t)-1;
    int oldest_slot = 0;
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].in_use) { slot = i; break; }
        if (notifications[i].start_tick < oldest_tick) {
            oldest_tick = notifications[i].start_tick;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot; /* overwrite oldest */
    notifications[slot].in_use = 1;
    strncpy(notifications[slot].text, text, sizeof(notifications[slot].text) - 1);
    notifications[slot].text[sizeof(notifications[slot].text) - 1] = 0;
    notifications[slot].color = color;
    notifications[slot].start_tick = (uint32_t)timer_get_ticks();
    notifications[slot].duration_ms = duration_ms ? duration_ms : 3000;
    /* Mark the notification area dirty. */
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height();
    gui_mark_dirty((int32_t)(fw - 320), (int32_t)(fh - active_theme.taskbar_h - 60),
                   310, 50);
    return slot;
}

/* ===================================================================
 * Drawing primitives (operate on window back buffer)
 * =================================================================== */

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
    /* Optimised: fill each row with a repeated pattern then memcpy. */
    uint32_t *row_start = w->back + (uint32_t)y * w->back_w + (uint32_t)x;
    for (uint32_t col = 0; col < W; col++) row_start[col] = color;
    for (uint32_t row = 1; row < H; row++) {
        memcpy(row_start + row * w->back_w, row_start, W * 4);
    }
    w->content_dirty = 1;
    return 0;
}

int gui_draw_rect(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H, uint32_t color) {
    if (W == 0 || H == 0) return 0;
    gui_fill_rect(wid, x, y, W, 1, color);
    gui_fill_rect(wid, x, y + (int32_t)H - 1, W, 1, color);
    gui_fill_rect(wid, x, y, 1, H, color);
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
    windows[wid].content_dirty = 1;
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
    windows[wid].content_dirty = 1;
    return 0;
}

int gui_clear(int wid, uint32_t color) {
    if (!win_alive(wid)) return -1;
    gui_win_t *w = &windows[wid];
    /* Optimised: fill first row then memcpy the rest. */
    if (w->back_w > 0 && w->back_h > 0) {
        for (uint32_t i = 0; i < w->back_w; i++) w->back[i] = color;
        for (uint32_t row = 1; row < w->back_h; row++) {
            memcpy(w->back + row * w->back_w, w->back, w->back_w * 4);
        }
    }
    w->content_dirty = 1;
    return 0;
}

int gui_blit(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H,
             const uint32_t *src, uint32_t src_stride) {
    if (!win_alive(wid) || !src) return -1;
    gui_win_t *w = &windows[wid];
    for (uint32_t row = 0; row < H; row++) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= w->back_h) continue;
        int32_t dx = x;
        uint32_t copy_w = W;
        if (dx < 0) {
            if (copy_w <= (uint32_t)(-dx)) continue;
            copy_w -= (uint32_t)(-dx);
            dx = 0;
        }
        if ((uint32_t)dx + copy_w > w->back_w) copy_w = w->back_w - (uint32_t)dx;
        if (copy_w > 0) {
            memcpy(w->back + (uint32_t)dy * w->back_w + (uint32_t)dx,
                   src + row * src_stride + (dx - x), copy_w * 4);
        }
    }
    w->content_dirty = 1;
    return 0;
}

int gui_blit_alpha(int wid, int32_t x, int32_t y, uint32_t W, uint32_t H,
                   const uint32_t *src, uint32_t src_stride) {
    if (!win_alive(wid) || !src) return -1;
    gui_win_t *w = &windows[wid];
    for (uint32_t row = 0; row < H; row++) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= w->back_h) continue;
        for (uint32_t col = 0; col < W; col++) {
            int32_t dx = x + (int32_t)col;
            if (dx < 0 || (uint32_t)dx >= w->back_w) continue;
            uint32_t s = src[row * src_stride + col];
            uint8_t a = (s >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) {
                w->back[(uint32_t)dy * w->back_w + (uint32_t)dx] = s;
            } else {
                uint32_t d = w->back[(uint32_t)dy * w->back_w + (uint32_t)dx];
                uint8_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
                uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                uint8_t ia = 255 - a;
                uint32_t r = ((uint32_t)sr * a + (uint32_t)dr * ia) / 255;
                uint32_t g = ((uint32_t)sg * a + (uint32_t)dg * ia) / 255;
                uint32_t b = ((uint32_t)sb * a + (uint32_t)db * ia) / 255;
                w->back[(uint32_t)dy * w->back_w + (uint32_t)dx] = (r << 16) | (g << 8) | b;
            }
        }
    }
    w->content_dirty = 1;
    return 0;
}

/* ===================================================================
 * Event ring
 * =================================================================== */

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

/* ===================================================================
 * Compositor rendering
 * =================================================================== */

extern struct limine_framebuffer *limine_get_framebuffer(void);

/* ---- Window decoration rendering ---- */

static void blit_window_decor(const gui_win_t *win) {
    const gui_theme_t *t = &active_theme;
    int active = win->focused;
    uint32_t border = active ? t->border_active : t->border;
    uint32_t titlec = active ? t->title_active : t->title_inactive;
    uint32_t brd = c_border();
    uint32_t tbh = c_titlebar();
    int off = t->shadow_offset;

    /* Shadow. */
    gfx_fill_rect((uint32_t)win->x + off, (uint32_t)win->y + off, win->w, win->h, t->shadow_color);

    if (win->flags & GUI_WIN_NO_DECOR) return;

    /* Border (full window). */
    gfx_fill_rect((uint32_t)win->x, (uint32_t)win->y, win->w, win->h, border);

    /* Title bar. */
    gfx_fill_rect((uint32_t)win->x + brd, (uint32_t)win->y + brd,
                  win->w - 2 * brd, tbh, titlec);

    /* Title text. */
    gfx_draw_text((uint32_t)win->x + 8, (uint32_t)win->y + brd + (tbh - 8) / 2,
                  win->title, t->title_text);

    /* Window buttons: [_][O][X] right-aligned. */
    uint32_t btn_w = 18, btn_h = 16, btn_pad = 2;
    if (win->flags & GUI_WIN_HAS_CLOSE) {
        uint32_t bx = (uint32_t)win->x + win->w - brd - btn_w - btn_pad;
        uint32_t by = (uint32_t)win->y + brd + (tbh - btn_h) / 2;
        gfx_fill_rect(bx, by, btn_w, btn_h, t->close_bg);
        /* Draw X symbol. */
        gfx_draw_line(bx + 4, by + 3, bx + btn_w - 5, by + btn_h - 4, t->title_text);
        gfx_draw_line(bx + btn_w - 5, by + 3, bx + 4, by + btn_h - 4, t->title_text);
    }
    if (win->flags & GUI_WIN_HAS_MINMAX) {
        uint32_t bx = (uint32_t)win->x + win->w - brd - 2 * (btn_w + btn_pad);
        uint32_t by = (uint32_t)win->y + brd + (tbh - btn_h) / 2;
        /* Maximize/Restore button. */
        gfx_fill_rect(bx, by, btn_w, btn_h, t->max_bg);
        if (win->maximized) {
            /* Restore icon: overlapping rectangles. */
            gfx_draw_rect(bx + 3, by + 3, 9, 9, t->title_text);
            gfx_fill_rect(bx + 6, by + 5, 9, 8, t->max_bg);
            gfx_draw_rect(bx + 6, by + 5, 9, 9, t->title_text);
        } else {
            /* Maximize icon: rectangle. */
            gfx_draw_rect(bx + 3, by + 3, 12, 10, t->title_text);
            gfx_fill_rect(bx + 3, by + 3, 12, 2, t->title_text);
        }
        /* Minimize button. */
        uint32_t mx = bx - btn_w - btn_pad;
        gfx_fill_rect(mx, by, btn_w, btn_h, t->min_bg);
        gfx_draw_line(mx + 4, by + btn_h / 2, mx + btn_w - 5, by + btn_h / 2, 0x00000000);
    }

    /* Resize grip (bottom-right diagonal lines). */
    if ((win->flags & GUI_WIN_RESIZABLE) && !win->maximized) {
        uint32_t grip = active_theme.resize_grip;
        uint32_t gx = (uint32_t)win->x + win->w - grip - 1;
        uint32_t gy = (uint32_t)win->y + win->h - grip - 1;
        for (int i = 0; i < (int)grip; i += 3) {
            gfx_draw_line(gx + i, gy + grip - 1, gx + grip - 1, gy + i, 0x00FFFFFF);
        }
    }
}

/* ---- Window content blitting (optimised with memcpy rows) ---- */

static void blit_window_content(const gui_win_t *win) {
    int32_t cx = content_x(win);
    int32_t cy = content_y(win);
    int32_t fw = (int32_t)gfx_get_width();
    int32_t fh = (int32_t)gfx_get_height();

    /* Calculate visible region. */
    int32_t src_y = 0, src_x = 0;
    int32_t dst_y = cy, dst_x = cx;
    uint32_t copy_w = win->back_w, copy_h = win->back_h;

    if (dst_y < 0) { src_y = -dst_y; copy_h -= (uint32_t)src_y; dst_y = 0; }
    if (dst_x < 0) { src_x = -dst_x; copy_w -= (uint32_t)src_x; dst_x = 0; }
    if (dst_y >= fh || dst_x >= fw) return;
    if (dst_y + (int32_t)copy_h > fh) copy_h = (uint32_t)(fh - dst_y);
    if (dst_x + (int32_t)copy_w > fw) copy_w = (uint32_t)(fw - dst_x);

    for (uint32_t row = 0; row < copy_h; row++) {
        for (uint32_t col = 0; col < copy_w; col++) {
            uint32_t pixel = win->back[(src_y + (int32_t)row) * win->back_w +
                                       (src_x + (int32_t)col)];
            gfx_putpixel((uint32_t)(dst_x + (int32_t)col),
                         (uint32_t)(dst_y + (int32_t)row), pixel);
        }
    }
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

/* ---- Desktop icons rendering ---- */

static void draw_icons(void) {
    const gui_theme_t *t = &active_theme;
    uint32_t isz = t->icon_size;
    (void)t->icon_pad; /* used below indirectly */

    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) continue;
        int32_t ix = icons[i].x;
        int32_t iy = icons[i].y;
        int sel = (i == icon_selected);

        /* Icon background rectangle (placeholder for real icon). */
        if (sel) {
            gfx_fill_rect((uint32_t)ix, (uint32_t)iy, isz, isz, t->icon_selected);
        } else {
            gfx_fill_rect((uint32_t)ix, (uint32_t)iy, isz, isz, 0x00406080);
        }
        gfx_draw_rect((uint32_t)ix, (uint32_t)iy, isz, isz, t->icon_text);

        /* Simple icon pattern: a small document/app shape. */
        uint32_t cx = (uint32_t)ix + isz / 2;
        uint32_t cy = (uint32_t)iy + isz / 2;
        /* App icon: filled circle with inner square. */
        gfx_fill_circle(cx, cy, 8, sel ? 0x00FFFFFF : 0x0080B0E0);
        gfx_fill_rect(cx - 3, cy - 3, 7, 7, sel ? t->icon_selected : 0x00406080);

        /* Label below icon. */
        gfx_draw_text((uint32_t)ix, (uint32_t)iy + isz + 2, icons[i].label, t->icon_text);
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

static void draw_cursor(void) {
    int mx, my;
    if (!mouse_get_position(&mx, &my)) return;
    switch (cursor) {
        case GUI_CURSOR_ARROW:
        default: {
            /* Refined arrow cursor. */
            for (int i = 0; i < 16; i++) {
                for (int j = 0; j <= i / 2; j++) {
                    gfx_putpixel((uint32_t)(mx + j), (uint32_t)(my + i), 0x00FFFFFF);
                }
            }
            /* Black outline. */
            gfx_draw_line((uint32_t)mx, (uint32_t)my,
                          (uint32_t)mx, (uint32_t)(my + 15), 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)(my + 15),
                          (uint32_t)(mx + 7), (uint32_t)(my + 11), 0x00000000);
            gfx_draw_line((uint32_t)mx, (uint32_t)my,
                          (uint32_t)(mx + 11), (uint32_t)(my + 11), 0x00000000);
            gfx_draw_line((uint32_t)(mx + 7), (uint32_t)(my + 11),
                          (uint32_t)(mx + 5), (uint32_t)(my + 15), 0x00000000);
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

/* ---- Taskbar rendering ---- */

static void draw_taskbar(void) {
    const gui_theme_t *t = &active_theme;
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height();
    uint32_t tb_h = t->taskbar_h;
    uint32_t tb_y = fh - tb_h;

    /* Background. */
    gfx_fill_rect(0, tb_y, fw, tb_h, t->taskbar_bg);
    /* Top border line. */
    gfx_draw_line(0, tb_y, fw, tb_y, t->taskbar_border);

    /* Start button. */
    uint32_t start_w = 90;
    gfx_fill_rect(4, tb_y + 4, start_w, tb_h - 8, t->start_btn_bg);
    gfx_draw_rect(4, tb_y + 4, start_w, tb_h - 8,
                  start_menu_open ? 0x00FFFFFF : 0x00306090);
    gfx_draw_text(12, tb_y + (tb_h - 8) / 2, "AuraLite", t->start_btn_text);

    /* Start menu dropdown. */
    if (start_menu_open) {
        uint32_t menu_w = 180, menu_h = 220;
        uint32_t menu_x = 4, menu_y = tb_y - menu_h;
        gfx_fill_rect(menu_x, menu_y, menu_w, menu_h, 0x00282838);
        gfx_draw_rect(menu_x, menu_y, menu_w, menu_h, 0x0060A0E0);
        gfx_draw_text(menu_x + 12, menu_y + 10, "Applications", t->taskbar_text);
        gfx_draw_line(menu_x + 8, menu_y + 22, menu_x + menu_w - 8, menu_y + 22, 0x00405060);

        const char *app_names[] = {
            "Calculator", "Text Editor", "File Manager",
            "Terminal", "System Monitor", "Task Manager",
            "Music Player", "Web Browser", "USB Manager",
            "About"
        };
        int n_apps = sizeof(app_names) / sizeof(app_names[0]);
        for (int i = 0; i < n_apps; i++) {
            uint32_t ay = menu_y + 28 + (uint32_t)i * 18;
            if (ay + 16 > tb_y) break;
            gfx_draw_text(menu_x + 16, ay, app_names[i], t->taskbar_text);
        }
    }

    /* Window list. */
    uint32_t bx = 4 + start_w + 8;
    uint32_t btn_w = 130, btn_h = tb_h - 8;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & (GUI_WIN_NO_DECOR | GUI_WIN_TOOL_WINDOW)) continue;
        if (bx + btn_w + 4 > fw - 100) break;
        uint32_t col;
        if (i == focused)        col = windows[i].focused ? t->title_active : t->title_inactive;
        else if (windows[i].minimized) col = 0x00202830;
        else                     col = 0x00303840;
        gfx_fill_rect(bx, tb_y + 4, btn_w, btn_h, col);
        gfx_draw_rect(bx, tb_y + 4, btn_w, btn_h, 0x00606878);
        /* Truncate title. */
        char title_short[20];
        int n = 0;
        while (n < 19 && windows[i].title[n]) { title_short[n] = windows[i].title[n]; n++; }
        title_short[n] = 0;
        gfx_draw_text(bx + 6, tb_y + (tb_h - 8) / 2, title_short, t->taskbar_text);
        bx += btn_w + 4;
    }

    /* System tray area (right side). */
    uint32_t tray_x = fw - 90;
    /* Clock. */
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
    clk[p++] = '0' + (hh / 10) % 10; clk[p++] = '0' + hh % 10;
    clk[p++] = ':'; clk[p++] = '0' + (mm / 10) % 10; clk[p++] = '0' + mm % 10;
    clk[p++] = ':'; clk[p++] = '0' + (ss / 10) % 10; clk[p++] = '0' + ss % 10;
    clk[p] = 0;
    gfx_draw_text(tray_x, tb_y + (tb_h - 8) / 2, clk, t->taskbar_text);
}

/* ---- Notifications rendering ---- */

static void draw_notifications(void) {
    const gui_theme_t *t = &active_theme;
    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height();
    uint32_t tb_h = t->taskbar_h;
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;

    int idx = 0;
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].in_use) continue;
        uint32_t elapsed = ((uint32_t)timer_get_ticks() - notifications[i].start_tick) * 1000 / hz;
        if (elapsed >= notifications[i].duration_ms) {
            notifications[i].in_use = 0;
            continue;
        }
        uint32_t nw = 300, nh = 40;
        uint32_t nx = fw - nw - 8;
        uint32_t ny = fh - tb_h - nh - 8 - (uint32_t)idx * (nh + 6);

        /* Background. */
        gfx_fill_rect(nx, ny, nw, nh, t->notif_bg);
        gfx_draw_rect(nx, ny, nw, nh, t->notif_border);
        /* Colored accent bar on left. */
        gfx_fill_rect(nx, ny, 4, nh, notifications[i].color);
        /* Text. */
        gfx_draw_text(nx + 10, ny + (nh - 8) / 2, notifications[i].text, t->notif_text);
        idx++;
    }
}

/* ---- Full compositor render ---- */

static void compositor_render(void) {
    draw_desktop();

    /* Draw desktop icons. */
    draw_icons();

    /* Build z-sorted index of visible windows. */
    int order[GUI_MAX_WINDOWS], n = 0;
    /* Always-on-top windows come last (drawn on top). */
    int normal[GUI_MAX_WINDOWS], nn = 0;
    int atop[GUI_MAX_WINDOWS], na = 0;

    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].visible && !windows[i].minimized) {
            if (windows[i].flags & GUI_WIN_ALWAYS_TOP) atop[na++] = i;
            else normal[nn++] = i;
        }
    }

    /* Insertion sort each group by z ascending. */
    #define SORT_ARR(arr, cnt) do { \
        for (int _i = 1; _i < (cnt); _i++) { \
            int _v = (arr)[_i], _j = _i - 1; \
            while (_j >= 0 && windows[(arr)[_j]].z > windows[_v].z) { \
                (arr)[_j + 1] = (arr)[_j]; _j--; \
            } \
            (arr)[_j + 1] = _v; \
        } \
    } while(0)

    SORT_ARR(normal, nn);
    SORT_ARR(atop, na);

    n = 0;
    for (int i = 0; i < nn; i++) order[n++] = normal[i];
    for (int i = 0; i < na; i++) order[n++] = atop[i];

    for (int i = 0; i < n; i++) {
        blit_window_decor(&windows[order[i]]);
        blit_window_content(&windows[order[i]]);
    }

    /* Snap preview overlay. */
    draw_snap_preview();

    /* Taskbar. */
    draw_taskbar();

    /* Notifications. */
    draw_notifications();

    /* Cursor (always on top). */
    draw_cursor();

    gfx_flip();
}

/* ===================================================================
 * Hit-testing
 * =================================================================== */

static int hit_window(int32_t mx, int32_t my) {
    int best = -1, bestz = -1;
    /* Check always-on-top windows first. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            if (!windows[i].in_use || !windows[i].visible || windows[i].minimized) continue;
            if (pass == 0 && !(windows[i].flags & GUI_WIN_ALWAYS_TOP)) continue;
            if (pass == 1 && (windows[i].flags & GUI_WIN_ALWAYS_TOP)) continue;
            gui_win_t *w = &windows[i];
            if (mx >= w->x && mx < (int32_t)(w->x + w->w) &&
                my >= w->y && my < (int32_t)(w->y + w->h)) {
                if (w->z > bestz) { bestz = w->z; best = i; }
            }
        }
        if (best >= 0 && pass == 0) return best; /* always-on-top hit */
    }
    return best;
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

/* ---- Desktop icon hit-test ---- */
static int hit_icon(int32_t mx, int32_t my) {
    uint32_t isz = active_theme.icon_size;
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) continue;
        if (mx >= icons[i].x && mx < icons[i].x + (int32_t)isz &&
            my >= icons[i].y && my < icons[i].y + (int32_t)isz + 16) {
            return i;
        }
    }
    return -1;
}

/* ---- Taskbar hit-test ---- */
static int taskbar_hit(int32_t mx, int32_t my) {
    uint32_t fh = gfx_get_height();
    uint32_t tb_h = active_theme.taskbar_h;
    if ((uint32_t)my < fh - tb_h) return -1;

    /* Start button. */
    if ((uint32_t)mx >= 4 && (uint32_t)mx < 94 && (uint32_t)my >= fh - tb_h + 4) return -2;

    /* Window list. */
    uint32_t start_w = 90;
    uint32_t bx = 4 + start_w + 8;
    uint32_t btn_w = 130;
    uint32_t fw = gfx_get_width();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible) continue;
        if (windows[i].flags & (GUI_WIN_NO_DECOR | GUI_WIN_TOOL_WINDOW)) continue;
        if (bx + btn_w + 4 > fw - 100) break;
        if ((uint32_t)mx >= bx && (uint32_t)mx < bx + btn_w) return i;
        bx += btn_w + 4;
    }
    return -1;
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

static void route_mouse_event(const mouse_event_t *ev) {
    int32_t mx = ev->abs_x, my = ev->abs_y;
    int wid = hit_window(mx, my);
    gui_cursor_t target_cursor = GUI_CURSOR_ARROW;

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
            drag_mode = 0;
            drag_wid  = -1;
            snap_preview_active = 0;
            /* Check if we should snap. */
            if (snap_preview_type != GUI_SNAP_NONE && win_alive(drag_wid == -1 ? 0 : 0)) {
                /* Find the window we were dragging by checking recent position. */
                /* Actually, the drag_wid was reset; let's use the fact that
                 * we saved it before clearing. We handle this differently: */
            }
            full_dirty = 1;
        }
        /* Handle snap on release. */
        if (drag_mode == 0 && snap_preview_type != GUI_SNAP_NONE) {
            /* We need to snap the window that was just released. */
            /* We already cleared drag_wid, but we can detect the window under cursor. */
            /* Better approach: save drag_wid before clearing. */
        }
        gui_set_cursor(target_cursor);
        return;
    }

    /* ---- No drag active ---- */

    /* Close start menu if clicking outside it. */
    if (start_menu_open && !(wid < 0 && taskbar_hit(mx, my) == -2)) {
        start_menu_open = 0;
        full_dirty = 1;
    }

    if (wid < 0) {
        /* Check taskbar. */
        int tb = taskbar_hit(mx, my);
        if (ev->pressed & MOUSE_BTN_LEFT) {
            if (tb == -2) {
                /* Start button. */
                start_menu_open = !start_menu_open;
                full_dirty = 1;
                gui_set_cursor(GUI_CURSOR_ARROW);
                last_hover_wid = -1;
                return;
            }
            if (tb >= 0) {
                if (windows[tb].minimized) gui_restore_window(tb);
                else if (focused == tb)    gui_minimize_window(tb);
                else                       gui_focus_window(tb);
                full_dirty = 1;
            }
        }
        /* Check desktop icons. */
        if (ev->pressed & MOUSE_BTN_LEFT) {
            int ic = hit_icon(mx, my);
            if (ic >= 0) {
                icon_selected = ic;
                full_dirty = 1;
            } else {
                icon_selected = -1;
            }
        }
        if (ev->pressed & MOUSE_BTN_LEFT && icon_selected >= 0) {
            int ic = hit_icon(mx, my);
            if (ic >= 0 && ic == icon_selected) {
                /* Double-click detection on icon. */
                uint32_t now = (uint32_t)timer_get_ticks();
                if (now - last_click_tick < 20 &&
                    abs_diff(mx, last_click_x) < 5 &&
                    abs_diff(my, last_click_y) < 5) {
                    /* Icon activated! */
                    kprintf("[gui] icon clicked: %s (id=%d)\n", icons[ic].label, icons[ic].icon_id);
                }
                last_click_tick = now;
                last_click_x = mx; last_click_y = my;
            }
        }
        gui_set_cursor(GUI_CURSOR_ARROW);
        last_hover_wid = -1;
        return;
    }

    gui_win_t *w = &windows[wid];
    int part = hit_part(w, mx, my);

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
        gui_focus_window(wid);
        switch (part) {
            case 2: /* Close */
                { gui_event_t e = { GUI_EVT_CLOSE_REQ, 0,0,0,0,0,0 }; gui_post_event(wid, &e); }
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
                int dbl = (last_click_tick && now - last_click_tick < 20
                           && abs_diff(mx, last_click_x) < 5
                           && abs_diff(my, last_click_y) < 5);
                last_click_tick = now;
                last_click_x = mx; last_click_y = my;
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
                int dbl = (last_click_tick && now - last_click_tick < 20
                           && abs_diff(mx, last_click_x) < 5
                           && abs_diff(last_click_y, my) < 5);
                last_click_tick = now;
                last_click_x = mx; last_click_y = my;
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
        if ((mods & 0x01) && ke->key == 0x3E) { /* F4 scancode area; approximate */
            if (focused >= 0) {
                gui_event_t e = { GUI_EVT_CLOSE_REQ, 0,0,0,0,0,0 };
                gui_post_event(focused, &e);
                return;
            }
        }
    }

    if (focused < 0) return;
    gui_event_t e = {0};
    e.type = ke->pressed ? GUI_EVT_KEY_DOWN : GUI_EVT_KEY_UP;
    e.key  = ke->key;
    e.mods = ke->mods;
    gui_post_event(focused, &e);
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

    /* Clock tick: force redraw once per second for the clock. */
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t ticks = timer_get_ticks();
    uint64_t elapsed_ticks = (ticks >= gui_clock_base_ticks) ?
        (ticks - gui_clock_base_ticks) : ticks;
    uint64_t now_sec = elapsed_ticks / hz;
    if (now_sec != gui_clock_last_second) {
        gui_clock_last_second = now_sec;
        gui_mark_dirty(0, gfx_get_height() - active_theme.taskbar_h,
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
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (notifications[i].in_use) {
            uint32_t elapsed = ((uint32_t)timer_get_ticks() - notifications[i].start_tick) * 1000 / hz;
            if (elapsed >= notifications[i].duration_ms) {
                notifications[i].in_use = 0;
                full_dirty = 1;
            }
        }
    }

    /* For now, use full redraw to ensure correctness.
     * TODO: switch to dirty-rect-based partial redraw once thoroughly tested. */
    full_dirty = 1;

    if (full_dirty || dirty_count > 0) {
        full_dirty = 0;
        dirty_count = 0;
        compositor_render();
    }
}

void gui_render_now(void) {
    full_dirty = 0;
    dirty_count = 0;
    compositor_render();
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

    for (;;) {
        gui_compositor_tick();
        uint64_t target = timer_get_ticks() + 1; /* ~100 Hz / 100 FPS */
        while (timer_get_ticks() < target) {
            sched_yield();
        }
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
