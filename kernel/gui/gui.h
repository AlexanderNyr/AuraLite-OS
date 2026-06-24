#ifndef AURALITE_KERNEL_GUI_H
#define AURALITE_KERNEL_GUI_H

#include <stdint.h>
#include <stddef.h>

/*
 * AuraLite GUI subsystem.
 *
 * Architecture:
 *   - Each window owns a private back-buffer (in kernel heap).
 *   - User-space draws into its window via syscalls (gui_draw_*).
 *   - The compositor copies all visible windows + cursor + decorations into
 *     the framebuffer once per frame (or when invalidated).
 *   - GUI events (mouse, keyboard) are routed to the focused window's per-
 *     window event ring; user-space pulls them via gui_get_event.
 *
 * This file declares the kernel-internal API.  User-visible types and the
 * matching syscall layer live in kernel/gui/gui_syscalls.{c,h} and the
 * matching libauragui/ user library.
 */

#define GUI_MAX_WINDOWS     32
#define GUI_TITLE_MAX       64
#define GUI_EVT_RING_SIZE   64
#define GUI_TASKBAR_H       28
#define GUI_TITLEBAR_H      22
#define GUI_BORDER          2
#define GUI_RESIZE_GRIP     12

/* Cursor shapes. */
typedef enum {
    GUI_CURSOR_ARROW = 0,
    GUI_CURSOR_IBEAM,
    GUI_CURSOR_HAND,
    GUI_CURSOR_HRESIZE,
    GUI_CURSOR_VRESIZE,
    GUI_CURSOR_DRESIZE,    /* diagonal SE */
    GUI_CURSOR_WAIT
} gui_cursor_t;

/* Window flags. */
#define GUI_WIN_RESIZABLE   0x01
#define GUI_WIN_MOVABLE     0x02
#define GUI_WIN_HAS_TITLE   0x04
#define GUI_WIN_HAS_CLOSE   0x08
#define GUI_WIN_HAS_MINMAX  0x10
#define GUI_WIN_MODAL       0x20
#define GUI_WIN_NO_DECOR    0x40    /* desktop / taskbar / cursor layer */

/* Default flag set for an "app window". */
#define GUI_WIN_DEFAULT (GUI_WIN_RESIZABLE | GUI_WIN_MOVABLE | \
                         GUI_WIN_HAS_TITLE | GUI_WIN_HAS_CLOSE | GUI_WIN_HAS_MINMAX)

/* GUI event types delivered to client windows. */
typedef enum {
    GUI_EVT_NONE = 0,
    GUI_EVT_MOUSE_MOVE,
    GUI_EVT_MOUSE_DOWN,
    GUI_EVT_MOUSE_UP,
    GUI_EVT_MOUSE_DBLCLICK,
    GUI_EVT_MOUSE_WHEEL,
    GUI_EVT_KEY_DOWN,
    GUI_EVT_KEY_UP,
    GUI_EVT_FOCUS,         /* this window gained keyboard focus */
    GUI_EVT_BLUR,          /* this window lost focus */
    GUI_EVT_RESIZE,        /* content area resized; data = w<<16 | h */
    GUI_EVT_CLOSE_REQ,     /* user clicked the [X] */
    GUI_EVT_TIMER,
    GUI_EVT_PAINT,         /* compositor wants us to refresh */
} gui_evt_type_t;

typedef struct {
    uint32_t type;          /* gui_evt_type_t */
    int32_t  x, y;          /* event-local coordinates (within window content) */
    uint32_t key;           /* for KEY_*: key code; for WHEEL: signed delta */
    uint8_t  buttons;       /* mouse buttons currently held */
    uint8_t  mods;          /* keyboard mods at event time */
    uint16_t data;          /* multipurpose */
} gui_event_t;

/* ---- Public kernel API (consumed by gui_syscalls and gui apps in-kernel) ---- */

void gui_init(void);
void gui_compositor_tick(void);   /* called from a kernel thread */
void gui_request_redraw(void);    /* mark whole screen dirty */

/* Destroy every window owned by a process/thread.  Called from thread_exit()
 * so GUI resources do not survive after their client exits. */
void gui_cleanup_process(uint64_t owner_pid);
int  gui_window_owned_by(int wid, uint64_t owner_pid);
uint64_t gui_window_owner(int wid);

/* Create a window.  Title is copied.  Returns wid >= 0 or -1.
 * The window starts hidden; call gui_show_window(wid) to make it visible. */
int  gui_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       const char *title, uint32_t flags);

int  gui_destroy_window(int wid);
int  gui_show_window(int wid);
int  gui_hide_window(int wid);
int  gui_move_window(int wid, int32_t x, int32_t y);
int  gui_resize_window(int wid, uint32_t w, uint32_t h);
int  gui_set_title(int wid, const char *title);
int  gui_focus_window(int wid);
int  gui_raise_window(int wid);   /* z-order to top */
int  gui_minimize_window(int wid);
int  gui_maximize_window(int wid);
int  gui_restore_window(int wid);

/* Window geometry queries (returns 0 on success). */
int  gui_get_window_size(int wid, uint32_t *w, uint32_t *h);

/* Back-buffer access for in-kernel apps; do not use across address spaces. */
uint32_t *gui_window_buffer(int wid, uint32_t *out_pitch_pixels);

/* Mark the window's whole content as dirty (will be composited next tick). */
int  gui_invalidate_window(int wid);

/* Mark a sub-rectangle as dirty (more efficient than full invalidate). */
int  gui_invalidate_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Event delivery. */
int  gui_post_event(int wid, const gui_event_t *evt);
int  gui_poll_event(int wid, gui_event_t *out);    /* non-blocking */
int  gui_wait_event(int wid, gui_event_t *out);    /* blocking (yields) */

/* Cursor. */
void gui_set_cursor(gui_cursor_t c);

/* ---- Drawing primitives (operate on a window's back buffer) ---- */
int gui_clear(int wid, uint32_t color);
int gui_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int gui_draw_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int gui_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
int gui_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color);
int gui_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color);
int gui_blit(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
             const uint32_t *src, uint32_t src_stride);

/* Convenience: re-render desktop + windows + cursor right now. */
void gui_render_now(void);

/* Self-test for boot. */
void gui_self_test(void);

#endif /* AURALITE_KERNEL_GUI_H */
