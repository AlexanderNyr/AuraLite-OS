/* kernel/gui/gui_shell.h
 *
 * Internal C ABI bridge between gui.c and the Rust desktop subsystem.
 *
 * Rust implementations live in kernel/gui/desktop/:
 *   - apps.rs
 *   - icons.rs
 *   - notifications.rs
 *   - start_menu.rs
 *   - taskbar.rs
 *   - taskbar_layout.rs
 *
 * gui.c owns window management, compositor orchestration and input routing.
 * Rust owns desktop state, rendering and shell interaction handlers.
 *
 * Everything declared here is private to the GUI implementation;
 * user space continues to use the public API from gui.h.
 */
#ifndef AURALITE_KERNEL_GUI_SHELL_H
#define AURALITE_KERNEL_GUI_SHELL_H

#include <stdint.h>

/* ---- Lifecycle ---------------------------------------------------- */

/* Initialise shell-owned state (icon table, notification table, menu
 * flags).  Called from gui_init(). */
void gui_shell_init(void);

/* Destroy desktop icons owned by a dead process.  Called from
 * gui_cleanup_process() under gui_lock. */
void shell_cleanup_icons(uint64_t owner_pid);

/* ---- Compositor hooks --------------------------------------------- */

/* Draw the shell layers.  Called from compositor_render() and
 * compositor_render_dirty() when the desktop chrome is not hidden by a
 * presentation window. */
void shell_draw_icons(void);
void shell_draw_taskbar(void);
void shell_draw_notifications(void);

/* Expire notifications past their duration.  Called once per compositor
 * tick; schedules a full redraw when something expired. */
void shell_expire_notifications(void);

/* ---- Interaction handlers ----------------------------------------- */
/*
 * Each handler returns 1 when it consumed the event (caller must return
 * from route_mouse_event) and 0 to fall through.  State (start menu /
 * overflow open, icon selection) lives inside gui_shell.c and is
 * managed internally; the caller only dispatches.
 *
 * `left_pressed` is true when the left button went down this event.
 * `wid` is the window under the cursor (-1 when none) and is needed by
 * the start-menu handler so it does not swallow the click that is about
 * to toggle the Start button itself.
 */
int shell_handle_start_menu_click(int32_t mx, int32_t my, int wid,
                                  int left_pressed);
int shell_handle_overflow_click  (int32_t mx, int32_t my, int left_pressed);
int shell_handle_taskbar_click   (int32_t mx, int32_t my, int left_pressed);
int shell_handle_icon_click      (int32_t mx, int32_t my, int left_pressed);

/* ---- Small helpers exposed by gui.c to gui_shell.c ---------------- */

/* Height of the active font's cell, for vertical text centring.  Returns
 * 8 if no font is loaded yet. */
uint32_t gui_font_h(void);

/* True when a visible presentation window is active; shell drawing and
 * taskbar hit testing must yield to it. */
int gui_presentation_active(void);

/* Iterate taskbar-eligible windows (in_use, visible, not NO_DECOR,
 * not TOOL_WINDOW) in stable order.  gui_taskbar_win_at() returns the
 * wid of the Nth eligible window, or -1. */
int gui_taskbar_win_count(void);
int gui_taskbar_win_at(int slot);

/* Copy a window's title into buf (always NUL-terminated).  No-op when
 * wid is dead.  Shell uses this instead of touching the window table. */
void gui_get_window_title(int wid, char *buf, uint32_t size);

/* Attribute readers. */
int  gui_window_is_minimized(int wid);

/* Double-click detection shared between titlebar (gui.c) and desktop
 * icons (gui_shell.c).  The (wid, part) key keeps the two uses from
 * interfering: titlebar uses part=1, icons use part=1000+idx. */
int  gui_is_double_click(uint32_t now, int32_t x, int32_t y,
                         int wid, int part, uint8_t button);
void gui_record_click(uint32_t now, int32_t x, int32_t y,
                      int wid, int part, uint8_t button);

#endif /* AURALITE_KERNEL_GUI_SHELL_H */