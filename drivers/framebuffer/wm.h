#ifndef AURALITE_DRIVERS_FRAMEBUFFER_WM_H
#define AURALITE_DRIVERS_FRAMEBUFFER_WM_H

#include <stdint.h>
#include "drivers/framebuffer/graphics.h"

/*
 * Minimal window manager: z-ordered windows with title bars, compositing,
 * and mouse-driven focus + drag.
 *
 * Windows are rendered to the graphics back buffer (double-buffered), then
 * flipped to the visible framebuffer. The mouse cursor is drawn on top.
 */

#define WM_MAX_WINDOWS 16
#define WM_TITLE_BAR_H 20
#define WM_BORDER_W    2

typedef struct wm_window {
    uint32_t x, y;          /* top-left position (pixels)             */
    uint32_t w, h;          /* width, height (including title + border) */
    char     title[64];     /* title bar text                         */
    color_t  title_color;   /* title bar background colour           */
    int      visible;       /* 0 = hidden                             */
    int      z_order;       /* higher = on top                       */
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

/* Composite all visible windows + the mouse cursor and flip to the screen. */
void wm_render(void);

/*
 * Poll for mouse events and update the window manager state (focus, drag).
 * Should be called periodically from the main loop. Returns 1 if a redraw
 * is needed, 0 otherwise.
 */
int wm_poll_mouse(void);

/* Draw the mouse cursor at its current position. */
void wm_draw_cursor(void);

/* Demo: create a few windows with text and let the user drag them. */
void wm_demo(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_WM_H */
