/* wm.c — minimal window manager with compositing and mouse interaction.
 *
 * Windows are z-ordered. The compositor draws each visible window from
 * bottom to top into the graphics back buffer, then draws the mouse cursor
 * on top and flips to the visible framebuffer.
 *
 * Mouse interaction:
 *   - Click in a title bar → focus + begin drag
 *   - Move with button held → drag the window
 *   - Release → stop dragging
 */

#include <stdint.h>
#include "drivers/framebuffer/wm.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/mouse/mouse.h"
#include "kernel/lib/string.h"

static wm_window_t windows[WM_MAX_WINDOWS];
static int window_count = 0;
static int focused_window = -1;

/* Drag state. */
static int dragging = 0;
static int drag_win_id = -1;
static int32_t drag_offset_x = 0;
static int32_t drag_offset_y = 0;

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    window_count = 0;
    focused_window = -1;
    dragging = 0;
}

int wm_create_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const char *title, color_t title_color) {
    if (window_count >= WM_MAX_WINDOWS) return -1;
    int id = window_count++;
    windows[id].x = x;
    windows[id].y = y;
    windows[id].w = w;
    windows[id].h = h;
    windows[id].title_color = title_color;
    windows[id].visible = 1;
    windows[id].z_order = id;
    strncpy(windows[id].title, title, sizeof(windows[id].title) - 1);
    focused_window = id;
    return id;
}

void wm_draw_text(int win_id, uint32_t col, uint32_t row,
                  const char *text, color_t color) {
    if (win_id < 0 || win_id >= window_count) return;
    wm_window_t *win = &windows[win_id];
    uint32_t px = win->x + WM_BORDER_W + col * 8;
    uint32_t py = win->y + WM_TITLE_BAR_H + WM_BORDER_W + row * 8;
    gfx_draw_string(px, py, text, color);
}

void wm_clear_window(int win_id, color_t color) {
    if (win_id < 0 || win_id >= window_count) return;
    wm_window_t *win = &windows[win_id];
    gfx_fill_rect(win->x + WM_BORDER_W,
                  win->y + WM_TITLE_BAR_H + WM_BORDER_W,
                  win->w - 2 * WM_BORDER_W,
                  win->h - WM_TITLE_BAR_H - 2 * WM_BORDER_W,
                  color);
}

void wm_fill_window_rect(int win_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, color_t color) {
    if (win_id < 0 || win_id >= window_count) return;
    wm_window_t *win = &windows[win_id];
    gfx_fill_rect(win->x + WM_BORDER_W + x,
                  win->y + WM_TITLE_BAR_H + WM_BORDER_W + y,
                  w, h, color);
}

static void draw_window(wm_window_t *win) {
    /* Window background (content area). */
    gfx_fill_rect(win->x, win->y, win->w, win->h, GFX_GRAY);

    /* Title bar. */
    uint32_t tb_color = (focused_window >= 0 &&
                         &windows[focused_window] == win)
                        ? win->title_color
                        : GFX_DARKBLUE;
    gfx_fill_rect(win->x, win->y, win->w, WM_TITLE_BAR_H, tb_color);

    /* Title text. */
    gfx_draw_string(win->x + 6, win->y + 5, win->title, GFX_WHITE);

    /* Border. */
    gfx_draw_rect(win->x, win->y, win->w, win->h,
                  (focused_window >= 0 && &windows[focused_window] == win)
                      ? GFX_WHITE : GFX_GRAY);
}

static int hit_test_title(int32_t mx, int32_t my) {
    /* Find the topmost window whose title bar contains (mx, my). */
    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i].visible) continue;
        wm_window_t *w = &windows[i];
        if (mx >= (int32_t)w->x && mx < (int32_t)(w->x + w->w) &&
            my >= (int32_t)w->y && my < (int32_t)(w->y + WM_TITLE_BAR_H)) {
            return i;
        }
    }
    return -1;
}

int wm_poll_mouse(void) {
    int mx, my;
    if (!mouse_get_position(&mx, &my)) return 0;

    uint8_t btns = mouse_get_buttons();
    int left = btns & 0x01;
    int need_redraw = 0;

    if (dragging) {
        if (!left) {
            /* Released: stop dragging. */
            dragging = 0;
        } else {
            /* Move the window to follow the cursor. */
            wm_window_t *w = &windows[drag_win_id];
            int32_t nx = mx - drag_offset_x;
            int32_t ny = my - drag_offset_y;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            uint32_t fw = gfx_get_width();
            uint32_t fh = gfx_get_height();
            if (nx + (int32_t)w->w > (int32_t)fw) nx = fw - w->w;
            if (ny + (int32_t)w->h > (int32_t)fh) ny = fh - w->h;
            w->x = (uint32_t)nx;
            w->y = (uint32_t)ny;
            need_redraw = 1;
        }
    } else if (left) {
        /* Check for a title-bar click. */
        int hit = hit_test_title(mx, my);
        if (hit >= 0) {
            /* Focus + begin dragging. */
            focused_window = hit;
            drag_win_id = hit;
            dragging = 1;
            drag_offset_x = mx - (int32_t)windows[hit].x;
            drag_offset_y = my - (int32_t)windows[hit].y;
            need_redraw = 1;
        }
    }

    /* Redraw on any mouse movement so the cursor stays smooth. */
    if (mouse_poll_event()) {
        need_redraw = 1;
    }

    return need_redraw;
}

void wm_draw_cursor(void) {
    int mx, my;
    if (!mouse_get_position(&mx, &my)) return;

    /* Draw a simple arrow cursor: a filled white triangle with a black outline. */
    for (int i = 0; i < 12; i++) {
        gfx_putpixel((uint32_t)mx + i, (uint32_t)my + i, GFX_WHITE);
        if (i > 0) {
            gfx_putpixel((uint32_t)mx + i - 1, (uint32_t)my + i, GFX_WHITE);
        }
    }
    /* Outline. */
    gfx_draw_line((uint32_t)mx, (uint32_t)my,
                  (uint32_t)mx + 11, (uint32_t)my + 11, GFX_BLACK);
    gfx_draw_line((uint32_t)mx, (uint32_t)my,
                  (uint32_t)mx, (uint32_t)my + 7, GFX_BLACK);
    gfx_draw_line((uint32_t)mx, (uint32_t)my + 7,
                  (uint32_t)mx + 4, (uint32_t)my + 5, GFX_BLACK);
    gfx_draw_line((uint32_t)mx + 4, (uint32_t)my + 5,
                  (uint32_t)mx + 11, (uint32_t)my + 11, GFX_BLACK);
}

void wm_render(void) {
    /* Composite all visible windows bottom-to-top. */
    for (int i = 0; i < window_count; i++) {
        if (windows[i].visible) {
            draw_window(&windows[i]);
        }
    }
    /* Draw the mouse cursor on top. */
    wm_draw_cursor();
    /* Flip to the visible framebuffer. */
    gfx_flip();
}

void wm_demo(void) {
    wm_init();

    /* Create a few windows. */
    int w1 = wm_create_window(100, 80, 360, 200,
                              "AuraLite Terminal", GFX_BLUE);
    wm_clear_window(w1, 0x00181828);
    wm_draw_text(w1, 1, 1, "AuraLite OS v1.0.0", GFX_GREEN);
    wm_draw_text(w1, 1, 2, "Window Manager Demo", GFX_CYAN);
    wm_draw_text(w1, 1, 4, "Click and drag title bars", GFX_GRAY);
    wm_draw_text(w1, 1, 5, "to move windows.", GFX_GRAY);

    int w2 = wm_create_window(500, 120, 280, 160,
                              "System Info", GFX_DARKBLUE);
    wm_clear_window(w2, 0x00181828);
    wm_draw_text(w2, 1, 1, "CPU: x86_64 (4 cores)", GFX_WHITE);
    wm_draw_text(w2, 1, 2, "RAM: 512 MiB", GFX_WHITE);
    wm_draw_text(w2, 1, 3, "NIC: e1000", GFX_WHITE);
    wm_draw_text(w2, 1, 4, "NET: ARP+IP+UDP+DNS", GFX_GREEN);

    int w3 = wm_create_window(200, 320, 400, 100,
                              "Tip", GFX_DARKBLUE);
    wm_clear_window(w3, 0x00181828);
    wm_draw_text(w3, 1, 1, "Mouse + Window Manager active!", GFX_YELLOW);
    wm_draw_text(w3, 1, 2, "Try dragging these windows.", GFX_GRAY);

    wm_render();
}
