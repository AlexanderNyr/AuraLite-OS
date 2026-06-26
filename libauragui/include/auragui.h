/* libauragui — user-space GUI toolkit for AuraLite OS v2.0.
 *
 * Thin wrappers around the SYS_GUI_* syscalls plus a small widget toolkit
 * (button, label, textbox, checkbox, slider, listbox, menu, progress,
 *  scrollarea, tab) and a top-level event loop.
 *
 * New in v2:
 *   - Right-click / middle-click event support
 *   - Context menu widget
 *   - Notification API
 *   - Theme API (read/write)
 *   - Desktop icon API
 *   - Window snap API
 *   - Window position/rect queries
 *   - Improved cursor shapes
 *   - Scrollarea and Tab widgets
 */
#ifndef AURAGUI_H
#define AURAGUI_H

#include <stdint.h>
#include "unistd.h"

/* ---- Window flags (must match kernel/gui/gui.h) ---- */
#define AG_WIN_RESIZABLE     0x001
#define AG_WIN_MOVABLE       0x002
#define AG_WIN_HAS_TITLE     0x004
#define AG_WIN_HAS_CLOSE     0x008
#define AG_WIN_HAS_MINMAX    0x010
#define AG_WIN_MODAL         0x020
#define AG_WIN_NO_DECOR      0x040
#define AG_WIN_ALWAYS_TOP    0x080
#define AG_WIN_TOOL_WINDOW   0x100
#define AG_WIN_BORDERLESS    0x200
#define AG_WIN_DEFAULT       (AG_WIN_RESIZABLE | AG_WIN_MOVABLE | \
                              AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_HAS_MINMAX)

/* ---- Cursor shapes ---- */
enum {
    AG_CURSOR_ARROW = 0, AG_CURSOR_IBEAM, AG_CURSOR_HAND,
    AG_CURSOR_HRESIZE, AG_CURSOR_VRESIZE, AG_CURSOR_DRESIZE,
    AG_CURSOR_WAIT, AG_CURSOR_MOVE, AG_CURSOR_CROSSHAIR,
    AG_CURSOR_NOT_ALLOWED, AG_CURSOR_COUNT
};

/* ---- Window snap types ---- */
enum {
    AG_SNAP_NONE = 0,
    AG_SNAP_LEFT,
    AG_SNAP_RIGHT,
    AG_SNAP_TOP,
    AG_SNAP_BOTTOM,
    AG_SNAP_MAXIMIZED,
};

/* ---- Event types (must match kernel/gui/gui.h EXACTLY) ----
 *
 * Values are explicit #defines, not auto-increment enum, so that the
 * kernel and user-space ABI never silently diverges.
 */
#define AG_EVT_NONE              0
#define AG_EVT_MOUSE_MOVE        1
#define AG_EVT_MOUSE_DOWN        2
#define AG_EVT_MOUSE_UP          3
#define AG_EVT_MOUSE_DBLCLICK    4
#define AG_EVT_MOUSE_WHEEL       5
#define AG_EVT_MOUSE_RIGHT_DOWN  6   /* right button down */
#define AG_EVT_MOUSE_RIGHT_UP    7
#define AG_EVT_MOUSE_MIDDLE_DOWN 8   /* middle button down */
#define AG_EVT_MOUSE_MIDDLE_UP   9
#define AG_EVT_KEY_DOWN          10
#define AG_EVT_KEY_UP            11
#define AG_EVT_FOCUS             12
#define AG_EVT_BLUR              13
#define AG_EVT_RESIZE            14
#define AG_EVT_CLOSE_REQ         15
#define AG_EVT_TIMER             16
#define AG_EVT_PAINT             17
#define AG_EVT_CONTEXT_MENU      18  /* right-click in client area */
#define AG_EVT_SNAP_CHANGED      19  /* window snap state changed */
#define AG_EVT_DROP              20  /* drag-drop (future) */
#define AG_EVT_ICON_CLICK        21  /* desktop icon activated */
#define AG_EVT_COUNT             22  /* sentinel — must be last */

/* ---- Event struct (must match kernel's gui_event_t) ---- */
typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint32_t key;
    uint8_t  buttons;
    uint8_t  mods;
    uint16_t data;
} ag_event_t;

/* ---- Theme struct (must match kernel's gui_theme_t) ---- */
typedef struct {
    uint32_t desktop_top, desktop_bot;
    uint32_t win_bg, win_content;
    uint32_t title_active, title_inactive, title_text;
    uint32_t border, border_active;
    uint32_t taskbar_bg, taskbar_border, taskbar_text;
    uint32_t start_btn_bg, start_btn_text;
    uint32_t close_bg, close_bg_hover, max_bg, min_bg;
    uint32_t icon_text, icon_selected;
    uint32_t notif_bg, notif_border, notif_text;
    uint32_t shadow_color;
    int      shadow_offset;
    uint32_t taskbar_h, titlebar_h, border_w, resize_grip;
    uint32_t icon_size, icon_pad;
    uint32_t win_round;
} ag_theme_t;

/* ---- Common colors (0x00RRGGBB) ---- */
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
int  ag_window_get_pos(int wid, int32_t *x, int32_t *y);
int  ag_window_snap(int wid, int snap_type);
void ag_render_now(void);
void ag_set_cursor(int cursor);
int  ag_set_clipboard(const char *text);
int  ag_get_clipboard(char *buf, uint32_t size);

/* ---- Drawing ---- */
int  ag_clear(int wid, uint32_t color);
int  ag_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int  ag_draw_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
int  ag_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
int  ag_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color);
int  ag_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color);
int  ag_draw_text_centered(int wid, int32_t x, int32_t y, uint32_t w,
                           const char *s, uint32_t color);

/* ---- Events ---- */
int  ag_poll_event(int wid, ag_event_t *out);
int  ag_wait_event(int wid, ag_event_t *out);

/* ---- Theme ---- */
int  ag_theme_get(ag_theme_t *out);
int  ag_theme_set(const ag_theme_t *t);

/* ---- Desktop icons ---- */
int  ag_add_icon(int32_t x, int32_t y, const char *label, int icon_id);
int  ag_remove_icon(int icon_idx);

/* ---- Notifications ---- */
int  ag_notify(const char *text, uint32_t color, uint32_t duration_ms);

/* ---- Toolkit / widgets ---- */

enum ag_widget_kind {
    AG_W_LABEL = 1,
    AG_W_BUTTON,
    AG_W_TEXTBOX,
    AG_W_CHECKBOX,
    AG_W_SLIDER,
    AG_W_PROGRESS,
    AG_W_LISTBOX,
    AG_W_PANEL,
    AG_W_SCROLLAREA,
    AG_W_TAB,
    AG_W_CONTEXTMENU,
};

struct ag_widget;
typedef void (*ag_callback_t)(struct ag_widget *w, void *user);

#define AG_MAX_WIDGET_TEXT 256
#define AG_MAX_LIST_ITEMS  128
#define AG_MAX_MENU_ITEMS  16
#define AG_MAX_TABS        8

typedef struct ag_widget {
    int           kind;
    int           id;
    int32_t       x, y;
    uint32_t      w, h;
    uint32_t      fg, bg;
    char          text[AG_MAX_WIDGET_TEXT];
    int           state;
    int           value;
    int           value_max;
    int           cursor_pos;
    int           selected;
    const char   *items[AG_MAX_LIST_ITEMS];
    int           item_count;
    int           focused;
    int           visible;
    ag_callback_t on_click;
    ag_callback_t on_change;
    ag_callback_t on_select;
    void         *user;
    /* Scroll area. */
    int32_t       scroll_x, scroll_y;
    int           child_count;
    struct ag_widget *children;  /* pointer into view's widget array */
    /* Tab. */
    int           active_tab;
    const char   *tab_labels[AG_MAX_TABS];
    int           tab_count;
    /* Context menu. */
    struct {
        const char *label;
        int         id;
    } menu_items[AG_MAX_MENU_ITEMS];
    int           menu_count;
    int           menu_visible;
} ag_widget_t;

typedef struct {
    int           wid;
    ag_widget_t  *widgets;
    int           widget_count;
    int           widget_cap;
    int           focused_widget;
    uint32_t      bg;
} ag_view_t;

void ag_view_init(ag_view_t *v, int wid, ag_widget_t *buf, int cap, uint32_t bg);

/* Widget creation. */
ag_widget_t *ag_add_label    (ag_view_t *v, int32_t x, int32_t y, const char *text, uint32_t color);
ag_widget_t *ag_add_button   (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h,
                              const char *text, ag_callback_t cb, void *user);
ag_widget_t *ag_add_textbox  (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h,
                              const char *initial);
ag_widget_t *ag_add_checkbox (ag_view_t *v, int32_t x, int32_t y, const char *text, int checked);
ag_widget_t *ag_add_slider   (ag_view_t *v, int32_t x, int32_t y, uint32_t w, int max, int value);
ag_widget_t *ag_add_progress (ag_view_t *v, int32_t x, int32_t y, uint32_t w, int max, int value);
ag_widget_t *ag_add_listbox  (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h);
ag_widget_t *ag_add_panel    (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t bg);
ag_widget_t *ag_add_scrollarea(ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h);
ag_widget_t *ag_add_tab      (ag_view_t *v, int32_t x, int32_t y, uint32_t w, uint32_t h);
ag_widget_t *ag_add_contextmenu(ag_view_t *v);

/* Listbox helpers. */
void ag_listbox_clear(ag_widget_t *lb);
int  ag_listbox_add(ag_widget_t *lb, const char *item);

/* Tab helpers. */
int  ag_tab_add(ag_widget_t *tab, const char *label);

/* Context menu helpers. */
int  ag_contextmenu_add(ag_widget_t *cm, const char *label, int id);

/* Set textbox content. */
void ag_textbox_set(ag_widget_t *tb, const char *s);

/* Render every widget into the window back buffer. */
void ag_view_render(ag_view_t *v);

/* Dispatch one event. Returns 1 if it was a CLOSE_REQ. */
int  ag_view_dispatch(ag_view_t *v, const ag_event_t *e);

/* Convenient blocking event loop. */
typedef int (*ag_loop_cb)(ag_view_t *v, const ag_event_t *e, void *user);
void ag_view_run(ag_view_t *v, ag_loop_cb cb, void *user);

/* ---- Modal helpers ---- */
void ag_alert(const char *title, const char *message);
int  ag_confirm(const char *title, const char *message);

#endif
