/* libauragui — user-space GUI toolkit for AuraLite OS.
 *
 * Thin wrappers around the SYS_GUI_* syscalls plus a small widget toolkit
 * (button, label, textbox, checkbox, slider, listbox, menu, progress) and
 * a top-level event loop.
 */
#ifndef AURAGUI_H
#define AURAGUI_H

#include <stdint.h>
#include "unistd.h"

/* Mirror of kernel/gui/gui.h enums (kept here so user apps don't include
 * kernel headers).  Values must match exactly. */
#define AG_WIN_RESIZABLE   0x01
#define AG_WIN_MOVABLE     0x02
#define AG_WIN_HAS_TITLE   0x04
#define AG_WIN_HAS_CLOSE   0x08
#define AG_WIN_HAS_MINMAX  0x10
#define AG_WIN_MODAL       0x20
#define AG_WIN_NO_DECOR    0x40
#define AG_WIN_DEFAULT     (AG_WIN_RESIZABLE | AG_WIN_MOVABLE | \
                            AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_HAS_MINMAX)

enum {
    AG_CURSOR_ARROW = 0, AG_CURSOR_IBEAM, AG_CURSOR_HAND,
    AG_CURSOR_HRESIZE, AG_CURSOR_VRESIZE, AG_CURSOR_DRESIZE, AG_CURSOR_WAIT
};

enum {
    AG_EVT_NONE = 0,
    AG_EVT_MOUSE_MOVE, AG_EVT_MOUSE_DOWN, AG_EVT_MOUSE_UP,
    AG_EVT_MOUSE_DBLCLICK, AG_EVT_MOUSE_WHEEL,
    AG_EVT_KEY_DOWN, AG_EVT_KEY_UP,
    AG_EVT_FOCUS, AG_EVT_BLUR,
    AG_EVT_RESIZE, AG_EVT_CLOSE_REQ,
    AG_EVT_TIMER, AG_EVT_PAINT,
};

/* Same struct as kernel's gui_event_t; layout must match. */
typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint32_t key;
    uint8_t  buttons;
    uint8_t  mods;
    uint16_t data;
} ag_event_t;

/* Common colors (0x00RRGGBB). */
#define AG_BLACK   0x00000000
#define AG_WHITE   0x00FFFFFF
#define AG_GRAY    0x00C0C4CC
#define AG_DARK    0x00404048
#define AG_RED     0x00C03020
#define AG_GREEN   0x00308040
#define AG_BLUE    0x002F60C0
#define AG_YELLOW  0x00E0C040
#define AG_ORANGE  0x00E07020
#define AG_PANEL   0x00ECEDF1
#define AG_BG      0x00FFFFFF
#define AG_ACCENT  0x002F60C0

/* ---- Low-level window API ---- */
int  ag_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                      const char *title, uint32_t flags);
int  ag_window_show(int wid);
int  ag_window_hide(int wid);
int  ag_window_destroy(int wid);
int  ag_window_move(int wid, int32_t x, int32_t y);
int  ag_window_resize(int wid, uint32_t w, uint32_t h);
int  ag_window_set_title(int wid, const char *title);
int  ag_window_invalidate(int wid);
int  ag_window_get_size(int wid, uint32_t *w, uint32_t *h);
void ag_render_now(void);
void ag_set_cursor(int cursor);

/* ---- Drawing ---- */
int  ag_clear(int wid, uint32_t color);
int  ag_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int  ag_draw_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int  ag_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
int  ag_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color);
int  ag_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color);
/* "Text in a box" with horizontal centering. */
int  ag_draw_text_centered(int wid, int32_t x, int32_t y, uint32_t w,
                           const char *s, uint32_t color);

/* ---- Events ---- */
int  ag_poll_event(int wid, ag_event_t *out);   /* 0 = none, 1 = got one */
int  ag_wait_event(int wid, ag_event_t *out);   /* blocking */

/* ---- Toolkit / widgets ---- */

/* Widget kinds. */
enum ag_widget_kind {
    AG_W_LABEL = 1,
    AG_W_BUTTON,
    AG_W_TEXTBOX,
    AG_W_CHECKBOX,
    AG_W_SLIDER,
    AG_W_PROGRESS,
    AG_W_LISTBOX,
    AG_W_PANEL,
};

struct ag_widget;
typedef void (*ag_callback_t)(struct ag_widget *w, void *user);

#define AG_MAX_WIDGET_TEXT 128
#define AG_MAX_LIST_ITEMS  128

typedef struct ag_widget {
    int           kind;
    int           id;
    int32_t       x, y;
    uint32_t      w, h;
    uint32_t      fg, bg;
    char          text[AG_MAX_WIDGET_TEXT];
    int           state;        /* button: 1=hover, 2=pressed; checkbox: 0/1 */
    int           value;        /* slider/progress: 0..max */
    int           value_max;
    int           cursor_pos;   /* textbox */
    int           selected;     /* listbox */
    const char   *items[AG_MAX_LIST_ITEMS];
    int           item_count;
    int           focused;
    int           visible;
    ag_callback_t on_click;     /* button */
    ag_callback_t on_change;    /* textbox / slider / checkbox */
    ag_callback_t on_select;    /* listbox */
    void         *user;
} ag_widget_t;

typedef struct {
    int           wid;            /* window id */
    ag_widget_t  *widgets;        /* caller-supplied array */
    int           widget_count;
    int           widget_cap;
    int           focused_widget; /* -1 = none */
    uint32_t      bg;
} ag_view_t;

void ag_view_init(ag_view_t *v, int wid, ag_widget_t *buf, int cap, uint32_t bg);
ag_widget_t *ag_add_label   (ag_view_t *v, int32_t x, int32_t y, const char *text, uint32_t color);
ag_widget_t *ag_add_button  (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h,
                             const char *text, ag_callback_t cb, void *user);
ag_widget_t *ag_add_textbox (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h,
                             const char *initial);
ag_widget_t *ag_add_checkbox(ag_view_t *v, int32_t x, int32_t y, const char *text, int checked);
ag_widget_t *ag_add_slider  (ag_view_t *v, int32_t x, int32_t y, uint32_t w, int max, int value);
ag_widget_t *ag_add_progress(ag_view_t *v, int32_t x, int32_t y, uint32_t w, int max, int value);
ag_widget_t *ag_add_listbox (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h);
ag_widget_t *ag_add_panel   (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t bg);

/* Listbox helpers. */
void ag_listbox_clear(ag_widget_t *lb);
int  ag_listbox_add(ag_widget_t *lb, const char *item);

/* Set textbox content (defensive copy). */
void ag_textbox_set(ag_widget_t *tb, const char *s);

/* Render every widget into the window back buffer. */
void ag_view_render(ag_view_t *v);

/* Dispatch one event.  Returns 1 if it was a CLOSE_REQ (caller may quit). */
int  ag_view_dispatch(ag_view_t *v, const ag_event_t *e);

/* Convenient blocking event loop.  Calls user_cb after each event so the
 * client can post timers / refresh state.  Returns when the close button is
 * pressed (or the user_cb returns non-zero to break out). */
typedef int (*ag_loop_cb)(ag_view_t *v, const ag_event_t *e, void *user);
void ag_view_run(ag_view_t *v, ag_loop_cb cb, void *user);

/* ---- Modal helpers ---- */
void ag_alert(const char *title, const char *message);
int  ag_confirm(const char *title, const char *message);  /* 1 = yes, 0 = no */

#endif
