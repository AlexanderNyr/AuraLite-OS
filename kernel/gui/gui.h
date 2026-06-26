#ifndef AURALITE_KERNEL_GUI_H
#define AURALITE_KERNEL_GUI_H

#include <stdint.h>
#include <stddef.h>

/*
 * AuraLite GUI subsystem — v2.0 rewrite.
 *
 * Architecture:
 *   - Each window owns a private back-buffer (in kernel heap).
 *   - User-space draws into its window via syscalls (gui_draw_*).
 *   - The compositor composes visible windows + decorations + taskbar + cursor
 *     onto the front framebuffer in z-order, driven by a dirty-rect tracker
 *     that avoids full redraws when only small regions changed.
 *   - GUI events (mouse, keyboard) are routed per tick: hover/down/up to the
 *     window under the cursor, key events to the focused window, with
 *     hit-tested decoration handling intercepted by the WM first.
 *   - A theme engine controls colors, border widths, titlebar height, etc.
 *     The default theme can be overridden at runtime.
 *   - Desktop icons provide quick-launch shortcuts.
 *   - A notification system pops transient messages above the taskbar.
 */

/* ---- Limits ---- */
#define GUI_MAX_WINDOWS       64
#define GUI_TITLE_MAX         64
#define GUI_EVT_RING_SIZE     128
#define GUI_MAX_ICONS         32
#define GUI_MAX_NOTIFICATIONS 8
#define GUI_MAX_DIRTY_RECTS   64

/* ---- Theme ---- */
typedef struct gui_theme {
    /* Desktop. */
    uint32_t desktop_top;
    uint32_t desktop_bot;
    /* Window. */
    uint32_t win_bg;
    uint32_t win_content;
    uint32_t title_active;
    uint32_t title_inactive;
    uint32_t title_text;
    uint32_t border;
    uint32_t border_active;
    /* Taskbar. */
    uint32_t taskbar_bg;
    uint32_t taskbar_border;
    uint32_t taskbar_text;
    uint32_t start_btn_bg;
    uint32_t start_btn_text;
    /* Window buttons. */
    uint32_t close_bg;
    uint32_t close_bg_hover;
    uint32_t max_bg;
    uint32_t min_bg;
    /* Desktop icons. */
    uint32_t icon_text;
    uint32_t icon_selected;
    /* Notifications. */
    uint32_t notif_bg;
    uint32_t notif_border;
    uint32_t notif_text;
    /* Shadows. */
    uint32_t shadow_color;
    int      shadow_offset;
    /* Dimensions. */
    uint32_t taskbar_h;
    uint32_t titlebar_h;
    uint32_t border_w;
    uint32_t resize_grip;
    uint32_t icon_size;
    uint32_t icon_pad;
    /* Rounding (0 = off). */
    uint32_t win_round;
} gui_theme_t;

/* Default theme accessor. */
const gui_theme_t *gui_default_theme(void);

/* Set the active theme (copies the struct). */
void gui_set_theme(const gui_theme_t *t);

/* Get the active theme (read-only). */
const gui_theme_t *gui_get_theme(void);

/* ---- Layout constants (derived from theme at init) ---- */
#define GUI_TASKBAR_H         gui_get_theme()->taskbar_h
#define GUI_TITLEBAR_H        gui_get_theme()->titlebar_h
#define GUI_BORDER            gui_get_theme()->border_w
#define GUI_RESIZE_GRIP       gui_get_theme()->resize_grip

/* Backward-compatible defaults for static initializers / old code. */
#define GUI_TASKBAR_H_DEFAULT 28
#define GUI_TITLEBAR_H_DEFAULT 22
#define GUI_BORDER_DEFAULT     2
#define GUI_RESIZE_GRIP_DEFAULT 12

/* ---- Cursor shapes ---- */
typedef enum {
    GUI_CURSOR_ARROW = 0,
    GUI_CURSOR_IBEAM,
    GUI_CURSOR_HAND,
    GUI_CURSOR_HRESIZE,
    GUI_CURSOR_VRESIZE,
    GUI_CURSOR_DRESIZE,
    GUI_CURSOR_WAIT,
    GUI_CURSOR_MOVE,
    GUI_CURSOR_CROSSHAIR,
    GUI_CURSOR_NOT_ALLOWED,
    GUI_CURSOR_COUNT
} gui_cursor_t;

/* ---- Window flags ---- */
#define GUI_WIN_RESIZABLE     0x001
#define GUI_WIN_MOVABLE       0x002
#define GUI_WIN_HAS_TITLE     0x004
#define GUI_WIN_HAS_CLOSE     0x008
#define GUI_WIN_HAS_MINMAX    0x010
#define GUI_WIN_MODAL         0x020
#define GUI_WIN_NO_DECOR      0x040
#define GUI_WIN_ALWAYS_TOP    0x080   /* stays above normal windows */
#define GUI_WIN_TOOL_WINDOW   0x100   /* smaller title, no taskbar entry */
#define GUI_WIN_BORDERLESS    0x200   /* no border, but has back buffer */
#define GUI_WIN_HAS_MENU      0x400   /* reserved: menu bar slot */

/* Default flag set for an "app window". */
#define GUI_WIN_DEFAULT (GUI_WIN_RESIZABLE | GUI_WIN_MOVABLE | \
                         GUI_WIN_HAS_TITLE | GUI_WIN_HAS_CLOSE | GUI_WIN_HAS_MINMAX)

/* ---- Window snap states ---- */
typedef enum {
    GUI_SNAP_NONE = 0,
    GUI_SNAP_LEFT,         /* snapped to left half */
    GUI_SNAP_RIGHT,        /* snapped to right half */
    GUI_SNAP_TOP,          /* snapped to top half */
    GUI_SNAP_BOTTOM,       /* snapped to bottom half */
    GUI_SNAP_MAXIMIZED,    /* full screen (no decor overlap) */
} gui_snap_t;

/* ---- GUI event types ----
 *
 * IMPORTANT: Values are explicit #defines so that the kernel and the
 * user-space mirror in libauragui/include/auragui.h stay in exact lock-step.
 * NEVER rely on auto-increment — always assign a value to every entry.
 */
#define GUI_EVT_NONE              0
#define GUI_EVT_MOUSE_MOVE        1
#define GUI_EVT_MOUSE_DOWN        2   /* left button down */
#define GUI_EVT_MOUSE_UP          3
#define GUI_EVT_MOUSE_DBLCLICK    4
#define GUI_EVT_MOUSE_WHEEL       5
#define GUI_EVT_MOUSE_RIGHT_DOWN  6   /* right button down */
#define GUI_EVT_MOUSE_RIGHT_UP    7
#define GUI_EVT_MOUSE_MIDDLE_DOWN 8   /* middle button down */
#define GUI_EVT_MOUSE_MIDDLE_UP   9
#define GUI_EVT_KEY_DOWN          10
#define GUI_EVT_KEY_UP            11
#define GUI_EVT_FOCUS             12
#define GUI_EVT_BLUR              13
#define GUI_EVT_RESIZE            14  /* content area resized */
#define GUI_EVT_CLOSE_REQ         15  /* user clicked [X] */
#define GUI_EVT_TIMER             16
#define GUI_EVT_PAINT             17
#define GUI_EVT_CONTEXT_MENU      18  /* right-click in client area */
#define GUI_EVT_SNAP_CHANGED      19  /* window snap state changed */
#define GUI_EVT_DROP              20  /* drag-drop completed (future) */
#define GUI_EVT_ICON_CLICK        21  /* desktop icon activated */
#define GUI_EVT_COUNT             22  /* sentinel — must be last */

typedef uint32_t gui_evt_type_t;

typedef struct {
    uint32_t type;          /* gui_evt_type_t */
    int32_t  x, y;          /* event-local coordinates (within window content) */
    uint32_t key;           /* for KEY_*: key code; for WHEEL: signed delta */
    uint8_t  buttons;       /* mouse buttons currently held */
    uint8_t  mods;          /* keyboard mods at event time */
    uint16_t data;          /* multipurpose */
} gui_event_t;

/* ---- Desktop icon ---- */
typedef struct {
    int    in_use;
    int32_t x, y;
    char   label[32];
    int    owner_pid;       /* 0 = kernel-owned */
    int    icon_id;         /* app identifier for routing */
} gui_icon_t;

/* ---- Notification ---- */
typedef struct {
    int      in_use;
    char     text[128];
    uint32_t color;
    uint32_t start_tick;
    uint32_t duration_ms;   /* 0 = default 3000ms */
} gui_notification_t;

/* ---- Dirty rectangle ---- */
typedef struct {
    int32_t  x, y;
    uint32_t w, h;
} gui_rect_t;

/* ---- Public kernel API ---- */

/* Init the GUI subsystem (call once before compositor thread). */
void gui_init(void);

/* Compositor tick: pump inputs, recompose dirty regions. */
void gui_compositor_tick(void);

/* Force a full-screen redraw next tick. */
void gui_request_redraw(void);

/* Mark a specific rectangle dirty (screen coords). */
void gui_mark_dirty(int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Process cleanup — destroy all windows owned by a PID. */
void gui_cleanup_process(uint64_t owner_pid);
int  gui_window_owned_by(int wid, uint64_t owner_pid);
uint64_t gui_window_owner(int wid);

/* ---- Window lifecycle ---- */
int  gui_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       const char *title, uint32_t flags);
int  gui_destroy_window(int wid);
int  gui_show_window(int wid);
int  gui_hide_window(int wid);
int  gui_move_window(int wid, int32_t x, int32_t y);
int  gui_resize_window(int wid, uint32_t w, uint32_t h);
int  gui_set_title(int wid, const char *title);
int  gui_focus_window(int wid);
int  gui_raise_window(int wid);
int  gui_minimize_window(int wid);
int  gui_maximize_window(int wid);
int  gui_restore_window(int wid);

/* Snap window to a screen edge. */
int  gui_snap_window(int wid, gui_snap_t snap);

/* Get window geometry. */
int  gui_get_window_size(int wid, uint32_t *w, uint32_t *h);
int  gui_get_window_pos(int wid, int32_t *x, int32_t *y);
int  gui_get_window_rect(int wid, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);

/* Get window flags. */
uint32_t gui_get_window_flags(int wid);

/* Back-buffer access for in-kernel apps. */
uint32_t *gui_window_buffer(int wid, uint32_t *out_pitch_pixels);

/* Invalidation. */
int  gui_invalidate_window(int wid);
int  gui_invalidate_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Event delivery. */
int  gui_post_event(int wid, const gui_event_t *evt);
int  gui_poll_event(int wid, gui_event_t *out);
int  gui_wait_event(int wid, gui_event_t *out);

/* Cursor. */
void gui_set_cursor(gui_cursor_t c);

/* ---- Desktop icons ---- */
int  gui_add_icon(int32_t x, int32_t y, const char *label, int icon_id);
int  gui_remove_icon(int icon_idx);
int  gui_icon_count(void);

/* ---- Notifications ---- */
int  gui_notify(const char *text, uint32_t color, uint32_t duration_ms);

/* ---- Drawing primitives (window back buffer) ---- */
int gui_clear(int wid, uint32_t color);
int gui_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int gui_draw_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int gui_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
int gui_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color);
int gui_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color);
int gui_blit(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
             const uint32_t *src, uint32_t src_stride);

/* Blit with alpha blending (src has per-pixel alpha in high byte). */
int gui_blit_alpha(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                   const uint32_t *src, uint32_t src_stride);

/* Convenience: re-render everything right now. */
void gui_render_now(void);

/* Compositor thread entry point. */
void gui_compositor_thread(void *arg);

/* Self-test for boot. */
void gui_self_test(void);

#endif /* AURALITE_KERNEL_GUI_H */
