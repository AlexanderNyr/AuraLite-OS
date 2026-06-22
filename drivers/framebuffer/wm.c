/* wm.c — full window manager with compositing, widgets, and taskbar.
 *
 * Features:
 *   - Z-ordered windows with title bars, close [X] buttons, borders
 *   - Desktop background gradient
 *   - Taskbar at the bottom with system info
 *   - Mouse-driven: focus, drag, close, widget interaction
 *   - Widget framework: buttons, labels, text, progress bars, rectangles
 *   - Double-buffered compositing
 */

#include <stdint.h>
#include "drivers/framebuffer/wm.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/psf.h"
#include "drivers/mouse/mouse.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/string.h"

static wm_window_t windows[WM_MAX_WINDOWS];
static int window_count = 0;
static int focused_window = -1;

/* Drag state. */
static int dragging = 0;
static int drag_win_id = -1;
static int32_t drag_offset_x = 0;
static int32_t drag_offset_y = 0;

/* Button press state. */
static int btn_pressed_win = -1;
static int btn_pressed_widget = -1;

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    window_count = 0;
    focused_window = -1;
    dragging = 0;
    btn_pressed_win = -1;
    btn_pressed_widget = -1;
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
    windows[id].has_close = 1;
    windows[id].widget_count = 0;
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

/* ---- Widget API ---- */

static wm_widget_t *alloc_widget(int win_id) {
    if (win_id < 0 || win_id >= window_count) return NULL;
    wm_window_t *win = &windows[win_id];
    if (win->widget_count >= WM_MAX_WIDGETS) return NULL;
    return &win->widgets[win->widget_count++];
}

int wm_add_button(int win_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const char *text, color_t bg, color_t fg) {
    wm_widget_t *wid = alloc_widget(win_id);
    if (!wid) return -1;
    wid->type = WIDGET_BUTTON;
    wid->x = x; wid->y = y; wid->w = w; wid->h = h;
    wid->fg = fg; wid->bg = bg;
    wid->state = 0;
    strncpy(wid->text, text, sizeof(wid->text) - 1);
    return windows[win_id].widget_count - 1;
}

int wm_add_label(int win_id, uint32_t x, uint32_t y,
                 const char *text, color_t color) {
    wm_widget_t *wid = alloc_widget(win_id);
    if (!wid) return -1;
    wid->type = WIDGET_LABEL;
    wid->x = x; wid->y = y; wid->w = strlen(text) * 8; wid->h = 8;
    wid->fg = color; wid->bg = 0;
    strncpy(wid->text, text, sizeof(wid->text) - 1);
    return windows[win_id].widget_count - 1;
}

int wm_add_progress(int win_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    int initial_progress) {
    wm_widget_t *wid = alloc_widget(win_id);
    if (!wid) return -1;
    wid->type = WIDGET_PROGRESS;
    wid->x = x; wid->y = y; wid->w = w; wid->h = h;
    wid->fg = GFX_GREEN; wid->bg = GFX_GRAY;
    wid->progress = initial_progress;
    return windows[win_id].widget_count - 1;
}

void wm_set_progress(int win_id, int widget_id, int progress) {
    if (win_id < 0 || win_id >= window_count) return;
    if (widget_id < 0 || widget_id >= windows[win_id].widget_count) return;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    windows[win_id].widgets[widget_id].progress = progress;
}

void wm_set_label_text(int win_id, int widget_id, const char *text) {
    if (win_id < 0 || win_id >= window_count) return;
    if (widget_id < 0 || widget_id >= windows[win_id].widget_count) return;
    strncpy(windows[win_id].widgets[widget_id].text, text, 63);
}

/* ---- Widget rendering ---- */

static void draw_widget(wm_window_t *win, wm_widget_t *wid) {
    uint32_t wx = win->x + WM_BORDER_W;
    uint32_t wy = win->y + WM_TITLE_BAR_H + WM_BORDER_W;
    uint32_t x = wx + wid->x;
    uint32_t y = wy + wid->y;

    switch (wid->type) {
    case WIDGET_NONE:
        break;
    case WIDGET_BUTTON: {
        /* Button background. */
        color_t bg = wid->bg;
        if (wid->state == 1) bg = GFX_YELLOW;       /* pressed */
        gfx_fill_rect(x, y, wid->w, wid->h, bg);
        /* Border. */
        gfx_draw_rect(x, y, wid->w, wid->h, GFX_WHITE);
        /* Text (centered). */
        uint32_t text_w = strlen(wid->text) * 8;
        uint32_t tx = x + (wid->w - text_w) / 2;
        uint32_t ty = y + (wid->h - 8) / 2;
        gfx_draw_string(tx, ty, wid->text, wid->fg);
        break;
    }
    case WIDGET_LABEL:
        gfx_draw_string(x, y, wid->text, wid->fg);
        break;
    case WIDGET_TEXT:
        gfx_draw_string(x, y, wid->text, wid->fg);
        break;
    case WIDGET_PROGRESS: {
        /* Background. */
        gfx_fill_rect(x, y, wid->w, wid->h, wid->bg);
        /* Filled portion. */
        uint32_t fill_w = (wid->w * (uint32_t)wid->progress) / 100;
        gfx_fill_rect(x, y, fill_w, wid->h, wid->fg);
        /* Border. */
        gfx_draw_rect(x, y, wid->w, wid->h, GFX_WHITE);
        /* Percentage text. */
        char pct[8];
        int pos = 0;
        int p = wid->progress;
        if (p >= 100) { pct[pos++] = '1'; pct[pos++] = '0'; pct[pos++] = '0'; }
        else if (p >= 10) { pct[pos++] = '0' + p / 10; pct[pos++] = '0' + p % 10; }
        else { pct[pos++] = '0' + p; }
        pct[pos++] = '%';
        pct[pos] = 0;
        uint32_t txt_w = pos * 8;
        gfx_draw_string(x + (wid->w - txt_w) / 2, y + 1, pct, GFX_WHITE);
        break;
    }
    case WIDGET_RECT:
        gfx_fill_rect(x, y, wid->w, wid->h, wid->bg);
        break;
    }
}

/* ---- Window rendering ---- */

static void draw_window(wm_window_t *win) {
    /* Window shadow (offset). */
    gfx_fill_rect(win->x + 3, win->y + 3, win->w, win->h, 0x00080808);

    /* Window background. */
    gfx_fill_rect(win->x, win->y, win->w, win->h, GFX_GRAY);

    /* Content background. */
    gfx_fill_rect(win->x + WM_BORDER_W,
                  win->y + WM_TITLE_BAR_H + WM_BORDER_W,
                  win->w - 2 * WM_BORDER_W,
                  win->h - WM_TITLE_BAR_H - 2 * WM_BORDER_W,
                  0x00181828);

    /* Title bar. */
    uint32_t tb_color = (focused_window >= 0 &&
                         &windows[focused_window] == win)
                        ? win->title_color
                        : 0x00202030;
    gfx_fill_rect(win->x, win->y, win->w, WM_TITLE_BAR_H, tb_color);

    /* Title text. */
    gfx_draw_string(win->x + 6, win->y + 4, win->title, GFX_WHITE);

    /* Close button [X]. */
    if (win->has_close) {
        uint32_t bx = win->x + win->w - 16;
        uint32_t by = win->y + 4;
        gfx_fill_rect(bx, by, 12, 12, GFX_RED);
        gfx_draw_string(bx + 2, by + 1, "X", GFX_WHITE);
    }

    /* Border. */
    gfx_draw_rect(win->x, win->y, win->w, win->h,
                  (focused_window >= 0 && &windows[focused_window] == win)
                      ? GFX_WHITE : 0x00404040);

    /* Render widgets. */
    for (int i = 0; i < win->widget_count; i++) {
        draw_widget(win, &win->widgets[i]);
    }
}

/* ---- Desktop background ---- */

void wm_render_desktop(void) {
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();

    /* Vertical gradient from dark blue to black. */
    for (uint32_t y = 0; y < h; y++) {
        uint8_t r = (uint8_t)((y * 10) / h);
        uint8_t g = (uint8_t)((y * 20) / h);
        uint8_t b = (uint8_t)(20 + (y * 40) / h);
        uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (uint32_t x = 0; x < w; x++) {
            gfx_putpixel(x, y, rgb);
        }
    }
}

/* ---- Taskbar ---- */

void wm_render_taskbar(void) {
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    uint32_t tb_y = h - WM_TASKBAR_H;

    /* Taskbar background. */
    gfx_fill_rect(0, tb_y, w, WM_TASKBAR_H, 0x00101018);

    /* Top border. */
    gfx_draw_line(0, tb_y, w, tb_y, 0x00404050);

    /* AuraLite logo / start text. */
    gfx_draw_string(8, tb_y + 6, "[ AuraLite OS ]", GFX_CYAN);

    /* Window list. */
    uint32_t wx = 160;
    for (int i = 0; i < window_count && i < 6; i++) {
        if (!windows[i].visible) continue;
        color_t bg = (i == focused_window) ? GFX_BLUE : 0x00202030;
        gfx_fill_rect(wx, tb_y + 3, 100, WM_TASKBAR_H - 6, bg);
        gfx_draw_rect(wx, tb_y + 3, 100, WM_TASKBAR_H - 6, GFX_GRAY);
        /* Truncate title to fit. */
        char short_title[12];
        strncpy(short_title, windows[i].title, 11);
        short_title[11] = 0;
        gfx_draw_string(wx + 4, tb_y + 7, short_title, GFX_WHITE);
        wx += 104;
    }

    /* Clock (uptime in ticks). */
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / 100;
    uint64_t mins = seconds / 60;
    uint64_t secs = seconds % 60;
    char clock[16];
    int pos = 0;
    /* mins */
    if (mins >= 10) clock[pos++] = '0' + (mins / 10) % 10;
    clock[pos++] = '0' + mins % 10;
    clock[pos++] = ':';
    clock[pos++] = '0' + (secs / 10) % 10;
    clock[pos++] = '0' + secs % 10;
    clock[pos++] = 0;
    gfx_draw_string(w - 60, tb_y + 6, clock, GFX_GREEN);
}

/* ---- Mouse ---- */

static int hit_test_title(int32_t mx, int32_t my) {
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

static int hit_test_close(int32_t mx, int32_t my) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i].visible || !windows[i].has_close) continue;
        wm_window_t *w = &windows[i];
        uint32_t bx = w->x + w->w - 16;
        uint32_t by = w->y + 4;
        if (mx >= (int32_t)bx && mx < (int32_t)(bx + 12) &&
            my >= (int32_t)by && my < (int32_t)(by + 12)) {
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
            dragging = 0;
        } else {
            wm_window_t *w = &windows[drag_win_id];
            int32_t nx = mx - drag_offset_x;
            int32_t ny = my - drag_offset_y;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            uint32_t fw = gfx_get_width();
            uint32_t fh = gfx_get_height();
            if (nx + (int32_t)w->w > (int32_t)fw) nx = fw - w->w;
            if (ny + (int32_t)w->h > (int32_t)(fh - WM_TASKBAR_H))
                ny = fh - WM_TASKBAR_H - w->h;
            w->x = (uint32_t)nx;
            w->y = (uint32_t)ny;
            need_redraw = 1;
        }
    } else if (left) {
        /* Check for close button first. */
        int close_hit = hit_test_close(mx, my);
        if (close_hit >= 0) {
            windows[close_hit].visible = 0;
            if (focused_window == close_hit) focused_window = -1;
            need_redraw = 1;
            return need_redraw;
        }

        /* Check for title bar click (focus + drag). */
        int hit = hit_test_title(mx, my);
        if (hit >= 0) {
            focused_window = hit;
            drag_win_id = hit;
            dragging = 1;
            drag_offset_x = mx - (int32_t)windows[hit].x;
            drag_offset_y = my - (int32_t)windows[hit].y;
            need_redraw = 1;
        }

        /* Check for button widget clicks. */
        for (int wi = window_count - 1; wi >= 0; wi--) {
            if (!windows[wi].visible) continue;
            wm_window_t *win = &windows[wi];
            uint32_t wx = win->x + WM_BORDER_W;
            uint32_t wy = win->y + WM_TITLE_BAR_H + WM_BORDER_W;
            for (int gi = 0; gi < win->widget_count; gi++) {
                wm_widget_t *wid = &win->widgets[gi];
                if (wid->type != WIDGET_BUTTON) continue;
                uint32_t bx = wx + wid->x;
                uint32_t by = wy + wid->y;
                if (mx >= (int32_t)bx && mx < (int32_t)(bx + wid->w) &&
                    my >= (int32_t)by && my < (int32_t)(by + wid->h)) {
                    wid->state = 1;  /* pressed */
                    btn_pressed_win = wi;
                    btn_pressed_widget = gi;
                    need_redraw = 1;
                    break;
                }
            }
            if (need_redraw) break;
        }
    } else {
        /* Release: reset pressed buttons. */
        if (btn_pressed_win >= 0 && btn_pressed_widget >= 0) {
            windows[btn_pressed_win].widgets[btn_pressed_widget].state = 0;
            need_redraw = 1;
            btn_pressed_win = -1;
            btn_pressed_widget = -1;
        }
    }

    if (mouse_poll_event()) {
        need_redraw = 1;
    }

    return need_redraw;
}

void wm_draw_cursor(void) {
    int mx, my;
    if (!mouse_get_position(&mx, &my)) return;

    /* Arrow cursor with outline. */
    for (int i = 0; i < 12; i++) {
        gfx_putpixel((uint32_t)mx + i, (uint32_t)my + i, GFX_WHITE);
        if (i > 0) {
            gfx_putpixel((uint32_t)mx + i - 1, (uint32_t)my + i, GFX_WHITE);
        }
    }
    gfx_draw_line((uint32_t)mx, (uint32_t)my,
                  (uint32_t)mx + 11, (uint32_t)my + 11, GFX_BLACK);
    gfx_draw_line((uint32_t)mx, (uint32_t)my,
                  (uint32_t)mx, (uint32_t)my + 7, GFX_BLACK);
    gfx_draw_line((uint32_t)mx, (uint32_t)my + 7,
                  (uint32_t)mx + 4, (uint32_t)my + 5, GFX_BLACK);
    gfx_draw_line((uint32_t)mx + 4, (uint32_t)my + 5,
                  (uint32_t)mx + 11, (uint32_t)my + 11, GFX_BLACK);
}

/* ---- Composite ---- */

void wm_render(void) {
    /* Desktop background. */
    wm_render_desktop();

    /* Windows (bottom to top). */
    for (int i = 0; i < window_count; i++) {
        if (windows[i].visible) {
            draw_window(&windows[i]);
        }
    }

    /* Taskbar. */
    wm_render_taskbar();

    /* Mouse cursor on top. */
    wm_draw_cursor();

    /* Flip. */
    gfx_flip();
}

/* ---- Full GUI demo ---- */

void wm_demo(void) {
    wm_init();

    /* Window 1: Terminal. */
    int w1 = wm_create_window(80, 60, 400, 220,
                              "Terminal", GFX_BLUE);
    wm_clear_window(w1, 0x00181828);
    wm_draw_text(w1, 1, 1, "AuraLite OS v1.0.0", GFX_GREEN);
    wm_draw_text(w1, 1, 2, "Full GUI Demo", GFX_CYAN);
    wm_draw_text(w1, 1, 4, "Features:", GFX_WHITE);
    wm_draw_text(w1, 3, 5, "Draggable windows", GFX_GRAY);
    wm_draw_text(w1, 3, 6, "Close buttons [X]", GFX_GRAY);
    wm_draw_text(w1, 3, 7, "Widget framework", GFX_GRAY);
    wm_draw_text(w1, 3, 8, "Taskbar with clock", GFX_GRAY);
    wm_draw_text(w1, 3, 9, "Desktop gradient", GFX_GRAY);
    wm_add_button(w1, 8, 150, 80, 20, "OK", GFX_GREEN, GFX_WHITE);
    wm_add_button(w1, 96, 150, 80, 20, "Cancel", GFX_RED, GFX_WHITE);

    /* Window 2: System Monitor. */
    int w2 = wm_create_window(500, 80, 320, 200,
                              "System Monitor", GFX_DARKBLUE);
    wm_clear_window(w2, 0x00181828);
    wm_add_label(w2, 8, 8, "CPU Usage:", GFX_WHITE);
    wm_add_progress(w2, 8, 20, 280, 16, 42);
    wm_add_label(w2, 8, 44, "Memory:", GFX_WHITE);
    wm_add_progress(w2, 8, 56, 280, 16, 67);
    wm_add_label(w2, 8, 80, "Network:", GFX_WHITE);
    wm_add_progress(w2, 8, 92, 280, 16, 15);
    wm_add_label(w2, 8, 116, "Disk I/O:", GFX_WHITE);
    wm_add_progress(w2, 8, 128, 280, 16, 88);

    /* Window 3: About. */
    int w3 = wm_create_window(200, 300, 300, 140,
                              "About AuraLite OS", GFX_DARKBLUE);
    wm_clear_window(w3, 0x00181828);
    wm_draw_text(w3, 1, 1, "AuraLite OS v1.0.0", GFX_YELLOW);
    wm_draw_text(w3, 1, 3, "x86_64 bare-metal kernel", GFX_WHITE);
    wm_draw_text(w3, 1, 4, "Booted via Limine", GFX_GRAY);
    wm_draw_text(w3, 1, 6, "Phases 0-14 + extensions", GFX_CYAN);
    wm_add_button(w3, 100, 80, 80, 20, "Close", GFX_RED, GFX_WHITE);

    wm_render();
}
