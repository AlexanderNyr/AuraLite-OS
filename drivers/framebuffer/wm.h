#ifndef AURALITE_DRIVERS_FRAMEBUFFER_WM_H
#define AURALITE_DRIVERS_FRAMEBUFFER_WM_H

#include <stdint.h>
#include "drivers/framebuffer/graphics.h"

/*
 * Full window manager with compositing, mouse interaction, and widgets.
 *
 * Features:
 *   - Z-ordered windows with title bars, close buttons, and borders
 *   - Double-buffered compositing (render to back buffer, flip)
 *   - Mouse-driven focus, drag, and close
 *   - Widget framework: buttons, labels, text areas, progress bars
 *   - Taskbar with clock and window list
 *   - Desktop background with gradient
 */

#define WM_MAX_WINDOWS  16
#define WM_TITLE_BAR_H  20
#define WM_BORDER_W     2
#define WM_TASKBAR_H    24
#define WM_MAX_WIDGETS  32

/* ---- Widget types ---- */
typedef enum {
    WIDGET_NONE = 0,
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_TEXT,        /* multi-line text area */
    WIDGET_PROGRESS,    /* progress bar */
    WIDGET_RECT,        /* colored rectangle (decoration) */
} widget_type_t;

/* ---- Widget ---- */
typedef struct {
    widget_type_t type;
    uint32_t x, y, w, h;       /* position relative to window content area */
    char text[64];              /* label/button text */
    color_t fg;                 /* foreground color */
    color_t bg;                 /* background color */
    int state;                  /* button: 0=normal, 1=pressed, 2=hover */
    int progress;               /* progress: 0-100 */
} wm_widget_t;

/* ---- Window ---- */
typedef struct wm_window {
    uint32_t x, y;          /* top-left position (pixels)             */
    uint32_t w, h;          /* width, height (including title + border) */
    char     title[64];     /* title bar text                         */
    color_t  title_color;   /* title bar background colour           */
    int      visible;       /* 0 = hidden                             */
    int      z_order;       /* higher = on top                       */
    int      has_close;     /* 1 = show close [X] button             */
    wm_widget_t widgets[WM_MAX_WIDGETS];
    int widget_count;
} wm_window_t;

/* Initialise the window manager. */
void wm_init(void);

/*
 * Create a window. Returns a window ID (>= 0) or -1 if the table is full.
 * The window starts at the top of the z-order.
 */
int wm_create_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const char *title, color_t title_color);

/* Draw a string inside a window's content area (below the title bar). */
void wm_draw_text(int win_id, uint32_t col, uint32_t row,
                  const char *text, color_t color);

/* Clear a window's content area to a colour. */
void wm_clear_window(int win_id, color_t color);

/* Fill a rectangle inside a window's content area. */
void wm_fill_window_rect(int win_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, color_t color);

/* ---- Widget API ---- */

/* Add a button widget to a window. Returns widget index or -1. */
int wm_add_button(int win_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const char *text, color_t bg, color_t fg);

/* Add a label widget. */
int wm_add_label(int win_id, uint32_t x, uint32_t y,
                 const char *text, color_t color);

/* Add a progress bar widget. */
int wm_add_progress(int win_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    int initial_progress);

/* Set a progress bar's value (0-100). */
void wm_set_progress(int win_id, int widget_id, int progress);

/* Set a label's text. */
void wm_set_label_text(int win_id, int widget_id, const char *text);

/* ---- Rendering ---- */

/* Composite all visible windows + taskbar + mouse cursor and flip. */
void wm_render(void);

/* Render the desktop background (gradient). */
void wm_render_desktop(void);

/* Render the taskbar (bottom of screen). */
void wm_render_taskbar(void);

/* ---- Mouse ---- */

/*
 * Poll for mouse events: focus, drag, button clicks, close button.
 * Returns 1 if a redraw is needed, 0 otherwise.
 */
int wm_poll_mouse(void);

/* Draw the mouse cursor at its current position. */
void wm_draw_cursor(void);

/* ---- Demo ---- */

/* Full GUI demo: desktop with multiple windows, widgets, and taskbar. */
void wm_demo(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_WM_H */
