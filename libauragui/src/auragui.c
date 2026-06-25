/* libauragui — user-space GUI wrappers + widget toolkit. */

#include <stdint.h>
#include "auragui.h"
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Pack two int32 into uint64. */
static uint64_t pack2(int32_t a, int32_t b) {
    return ((uint64_t)(uint32_t)b << 32) | (uint32_t)a;
}

/* GUI ops — keep in sync with kernel/gui/gui_syscalls.h enums. */
enum {
    GUI_OP_CREATE = 1, GUI_OP_DESTROY, GUI_OP_SHOW, GUI_OP_HIDE,
    GUI_OP_MOVE, GUI_OP_RESIZE, GUI_OP_TITLE, GUI_OP_FOCUS,
    GUI_OP_MINIMIZE, GUI_OP_MAXIMIZE, GUI_OP_RESTORE,
    GUI_OP_CLEAR, GUI_OP_FILL_RECT, GUI_OP_DRAW_RECT, GUI_OP_DRAW_LINE,
    GUI_OP_DRAW_TEXT, GUI_OP_DRAW_PIXEL, GUI_OP_INVALIDATE, GUI_OP_RENDER,
    GUI_OP_SET_CURSOR, GUI_OP_GET_SIZE, GUI_OP_SET_CLIPBOARD, GUI_OP_GET_CLIPBOARD,
};
#define SYS_GUI_CALL_NUM    200
#define SYS_GUI_EVENT_NUM   201

static int64_t gui_call(uint64_t op, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
    return syscall(SYS_GUI_CALL_NUM, op, a2, a3, a4, a5, 0);
}

int ag_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                     const char *title, uint32_t flags) {
    return (int)gui_call(GUI_OP_CREATE,
                         pack2(x, y),
                         pack2((int32_t)w, (int32_t)h),
                         (uint64_t)title, flags);
}
int ag_window_show(int wid)           { return (int)gui_call(GUI_OP_SHOW, wid, 0, 0, 0); }
int ag_window_hide(int wid)           { return (int)gui_call(GUI_OP_HIDE, wid, 0, 0, 0); }
int ag_window_destroy(int wid)        { return (int)gui_call(GUI_OP_DESTROY, wid, 0, 0, 0); }
int ag_window_move(int wid, int32_t x, int32_t y) {
    return (int)gui_call(GUI_OP_MOVE, wid, pack2(x, y), 0, 0);
}
int ag_window_resize(int wid, uint32_t w, uint32_t h) {
    return (int)gui_call(GUI_OP_RESIZE, wid, pack2((int32_t)w, (int32_t)h), 0, 0);
}
int ag_window_set_title(int wid, const char *t) {
    return (int)gui_call(GUI_OP_TITLE, wid, (uint64_t)t, 0, 0);
}
int ag_window_invalidate(int wid)     { return (int)gui_call(GUI_OP_INVALIDATE, wid, 0, 0, 0); }
int ag_window_get_size(int wid, uint32_t *w, uint32_t *h) {
    /* Keep the syscall output buffer out of the current user stack. AuraLite's
     * syscall entry still runs on the issuing user stack, so copy_to_user() into
     * a stack-local object can overlap the kernel's temporary syscall frames in
     * deeply nested GUI calls. */
    static uint32_t out[2];
    out[0] = out[1] = 0;
    int r = (int)gui_call(GUI_OP_GET_SIZE, wid, (uint64_t)out, 0, 0);
    if (r == 0) { if (w) *w = out[0]; if (h) *h = out[1]; }
    return r;
}
void ag_render_now(void)              { gui_call(GUI_OP_RENDER, 0, 0, 0, 0); }
void ag_set_cursor(int c)             { gui_call(GUI_OP_SET_CURSOR, c, 0, 0, 0); }
int  ag_set_clipboard(const char *text) { return (int)gui_call(GUI_OP_SET_CLIPBOARD, (uint64_t)text, 0, 0, 0); }
int  ag_get_clipboard(char *buf, uint32_t size) { return (int)gui_call(GUI_OP_GET_CLIPBOARD, (uint64_t)buf, size, 0, 0); }

int ag_clear(int wid, uint32_t color) { return (int)gui_call(GUI_OP_CLEAR, wid, color, 0, 0); }
int ag_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    return (int)gui_call(GUI_OP_FILL_RECT, wid, pack2(x, y),
                         pack2((int32_t)w, (int32_t)h), color);
}
int ag_draw_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    return (int)gui_call(GUI_OP_DRAW_RECT, wid, pack2(x, y),
                         pack2((int32_t)w, (int32_t)h), color);
}
int ag_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    return (int)gui_call(GUI_OP_DRAW_LINE, wid, pack2(x0, y0), pack2(x1, y1), color);
}
int ag_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t color) {
    return (int)gui_call(GUI_OP_DRAW_TEXT, wid, pack2(x, y), (uint64_t)s, color);
}
int ag_draw_pixel(int wid, int32_t x, int32_t y, uint32_t color) {
    return (int)gui_call(GUI_OP_DRAW_PIXEL, wid, pack2(x, y), color, 0);
}

int ag_draw_text_centered(int wid, int32_t x, int32_t y, uint32_t w,
                          const char *s, uint32_t color) {
    int32_t tw = (int32_t)(strlen(s) * 8);
    int32_t tx = x + ((int32_t)w - tw) / 2;
    if (tx < x) tx = x;
    return ag_draw_text(wid, tx, y, s, color);
}

int ag_poll_event(int wid, ag_event_t *e) {
    return (int)syscall(SYS_GUI_EVENT_NUM, (uint64_t)wid, (uint64_t)e, 0, 0, 0, 0);
}
int ag_wait_event(int wid, ag_event_t *e) {
    return (int)syscall(SYS_GUI_EVENT_NUM, (uint64_t)wid, (uint64_t)e, 1, 0, 0, 0);
}

/* ---- Toolkit / widget framework ---- */

void ag_view_init(ag_view_t *v, int wid, ag_widget_t *buf, int cap, uint32_t bg) {
    v->wid = wid;
    v->widgets = buf;
    v->widget_count = 0;
    v->widget_cap = cap;
    v->focused_widget = -1;
    v->bg = bg ? bg : AG_PANEL;
    
    int fd = open("/disk/theme.txt");
    if (fd >= 0) {
        char thm[16];
        int n = read(fd, thm, sizeof(thm)-1);
        if (n > 0) {
            thm[n] = 0;
            v->bg = (uint32_t)strtol(thm, NULL, 16);
        }
        close(fd);
    }
    
    for (int i = 0; i < cap; i++) buf[i].kind = 0;
}

static ag_widget_t *alloc_widget(ag_view_t *v) {
    if (v->widget_count >= v->widget_cap) return 0;
    ag_widget_t *w = &v->widgets[v->widget_count];
    memset(w, 0, sizeof(*w));
    w->id = v->widget_count;
    w->visible = 1;
    v->widget_count++;
    return w;
}

ag_widget_t *ag_add_label(ag_view_t *v, int32_t x, int32_t y, const char *text, uint32_t color) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_LABEL;
    w->x = x; w->y = y; w->w = strlen(text) * 8; w->h = 12;
    w->fg = color; w->bg = 0;
    strncpy(w->text, text, AG_MAX_WIDGET_TEXT - 1);
    return w;
}

ag_widget_t *ag_add_button(ag_view_t *v, int32_t x, int32_t y, uint32_t W, uint32_t H,
                           const char *text, ag_callback_t cb, void *user) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_BUTTON;
    w->x = x; w->y = y; w->w = W; w->h = H;
    w->fg = AG_WHITE; w->bg = AG_ACCENT;
    strncpy(w->text, text, AG_MAX_WIDGET_TEXT - 1);
    w->on_click = cb; w->user = user;
    return w;
}

ag_widget_t *ag_add_textbox(ag_view_t *v, int32_t x, int32_t y, uint32_t W, uint32_t H,
                            const char *initial) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_TEXTBOX;
    w->x = x; w->y = y; w->w = W; w->h = H;
    w->fg = AG_BLACK; w->bg = AG_WHITE;
    if (initial) {
        strncpy(w->text, initial, AG_MAX_WIDGET_TEXT - 1);
        w->cursor_pos = (int)strlen(w->text);
    }
    return w;
}

ag_widget_t *ag_add_checkbox(ag_view_t *v, int32_t x, int32_t y, const char *text, int checked) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_CHECKBOX;
    w->x = x; w->y = y; w->w = 16 + 4 + strlen(text) * 8; w->h = 16;
    w->fg = AG_BLACK; w->bg = AG_WHITE;
    w->state = checked ? 1 : 0;
    strncpy(w->text, text, AG_MAX_WIDGET_TEXT - 1);
    return w;
}

ag_widget_t *ag_add_slider(ag_view_t *v, int32_t x, int32_t y, uint32_t W, int max, int value) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_SLIDER;
    w->x = x; w->y = y; w->w = W; w->h = 18;
    w->fg = AG_ACCENT; w->bg = AG_GRAY;
    w->value = value; w->value_max = max;
    return w;
}

ag_widget_t *ag_add_progress(ag_view_t *v, int32_t x, int32_t y, uint32_t W, int max, int value) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_PROGRESS;
    w->x = x; w->y = y; w->w = W; w->h = 16;
    w->fg = AG_GREEN; w->bg = AG_GRAY;
    w->value = value; w->value_max = max;
    return w;
}

ag_widget_t *ag_add_listbox(ag_view_t *v, int32_t x, int32_t y, uint32_t W, uint32_t H) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_LISTBOX;
    w->x = x; w->y = y; w->w = W; w->h = H;
    w->fg = AG_BLACK; w->bg = AG_WHITE;
    w->selected = -1;
    return w;
}

ag_widget_t *ag_add_panel(ag_view_t *v, int32_t x, int32_t y, uint32_t W, uint32_t H, uint32_t bg) {
    ag_widget_t *w = alloc_widget(v);
    if (!w) return 0;
    w->kind = AG_W_PANEL;
    w->x = x; w->y = y; w->w = W; w->h = H;
    w->bg = bg;
    return w;
}

void ag_listbox_clear(ag_widget_t *lb) {
    lb->item_count = 0; lb->selected = -1;
}
int ag_listbox_add(ag_widget_t *lb, const char *item) {
    if (lb->item_count >= AG_MAX_LIST_ITEMS) return -1;
    lb->items[lb->item_count++] = item;
    return lb->item_count - 1;
}

void ag_textbox_set(ag_widget_t *tb, const char *s) {
    strncpy(tb->text, s ? s : "", AG_MAX_WIDGET_TEXT - 1);
    tb->text[AG_MAX_WIDGET_TEXT - 1] = 0;
    tb->cursor_pos = (int)strlen(tb->text);
}

/* ---- Widget rendering ---- */

static void render_button(int wid, ag_widget_t *w) {
    uint32_t bg = w->bg;
    if (w->state == 1) bg = AG_YELLOW;       /* hover (unused for now) */
    if (w->state == 2) bg = 0x00C0A020;      /* pressed */
    ag_fill_rect(wid, w->x, w->y, w->w, w->h, bg);
    ag_draw_rect(wid, w->x, w->y, w->w, w->h, AG_DARK);
    ag_draw_text_centered(wid, w->x, w->y + (int32_t)w->h / 2 - 4, w->w, w->text, w->fg);
}

static void render_label(int wid, ag_widget_t *w) {
    ag_draw_text(wid, w->x, w->y, w->text, w->fg);
}

static void render_textbox(int wid, ag_widget_t *w) {
    ag_fill_rect(wid, w->x, w->y, w->w, w->h, w->bg);
    ag_draw_rect(wid, w->x, w->y, w->w, w->h, w->focused ? AG_ACCENT : AG_DARK);
    ag_draw_text(wid, w->x + 4, w->y + (int32_t)w->h / 2 - 4, w->text, w->fg);
    if (w->focused) {
        int32_t cx = w->x + 4 + w->cursor_pos * 8;
        ag_draw_line(wid, cx, w->y + 3, cx, w->y + (int32_t)w->h - 4, AG_ACCENT);
    }
}

static void render_checkbox(int wid, ag_widget_t *w) {
    ag_fill_rect(wid, w->x, w->y, 16, 16, w->bg);
    ag_draw_rect(wid, w->x, w->y, 16, 16, AG_DARK);
    if (w->state) {
        ag_draw_line(wid, w->x + 3, w->y + 7, w->x + 7, w->y + 12, AG_GREEN);
        ag_draw_line(wid, w->x + 7, w->y + 12, w->x + 13, w->y + 3, AG_GREEN);
    }
    ag_draw_text(wid, w->x + 22, w->y + 4, w->text, w->fg);
}

static void render_slider(int wid, ag_widget_t *w) {
    /* Track. */
    ag_fill_rect(wid, w->x, w->y + 7, w->w, 4, w->bg);
    /* Thumb. */
    int32_t span = (int32_t)w->w - 12;
    int32_t thumb_x = w->x + (w->value_max ? (w->value * span) / w->value_max : 0);
    ag_fill_rect(wid, thumb_x, w->y, 12, 18, w->fg);
    ag_draw_rect(wid, thumb_x, w->y, 12, 18, AG_DARK);
}

static void render_progress(int wid, ag_widget_t *w) {
    ag_fill_rect(wid, w->x, w->y, w->w, w->h, w->bg);
    uint32_t fw = w->value_max ? (uint32_t)((w->w * (uint32_t)w->value) / (uint32_t)w->value_max) : 0;
    ag_fill_rect(wid, w->x, w->y, fw, w->h, w->fg);
    ag_draw_rect(wid, w->x, w->y, w->w, w->h, AG_DARK);
    char buf[8];
    int pct = w->value_max ? (w->value * 100) / w->value_max : 0;
    int p = 0;
    if (pct >= 100) { buf[p++] = '1'; buf[p++] = '0'; buf[p++] = '0'; }
    else if (pct >= 10) { buf[p++] = '0' + pct / 10; buf[p++] = '0' + pct % 10; }
    else { buf[p++] = '0' + pct; }
    buf[p++] = '%'; buf[p] = 0;
    ag_draw_text_centered(wid, w->x, w->y + (int32_t)w->h / 2 - 4, w->w, buf, AG_WHITE);
}

static void render_listbox(int wid, ag_widget_t *w) {
    ag_fill_rect(wid, w->x, w->y, w->w, w->h, w->bg);
    ag_draw_rect(wid, w->x, w->y, w->w, w->h, w->focused ? AG_ACCENT : AG_DARK);
    int row_h = 14;
    for (int i = 0; i < w->item_count; i++) {
        int32_t ry = w->y + 2 + i * row_h;
        if (ry + row_h > w->y + (int32_t)w->h - 2) break;
        if (i == w->selected) {
            ag_fill_rect(wid, w->x + 1, ry, w->w - 2, (uint32_t)row_h, AG_ACCENT);
            ag_draw_text(wid, w->x + 5, ry + 3, w->items[i], AG_WHITE);
        } else {
            ag_draw_text(wid, w->x + 5, ry + 3, w->items[i], w->fg);
        }
    }
}

static void render_panel(int wid, ag_widget_t *w) {
    ag_fill_rect(wid, w->x, w->y, w->w, w->h, w->bg);
    ag_draw_rect(wid, w->x, w->y, w->w, w->h, AG_DARK);
}

void ag_view_render(ag_view_t *v) {
    uint32_t W, H;
    ag_window_get_size(v->wid, &W, &H);
    ag_clear(v->wid, v->bg);
    for (int i = 0; i < v->widget_count; i++) {
        ag_widget_t *w = &v->widgets[i];
        if (!w->visible) continue;
        switch (w->kind) {
            case AG_W_LABEL:    render_label(v->wid, w);    break;
            case AG_W_BUTTON:   render_button(v->wid, w);   break;
            case AG_W_TEXTBOX:  render_textbox(v->wid, w);  break;
            case AG_W_CHECKBOX: render_checkbox(v->wid, w); break;
            case AG_W_SLIDER:   render_slider(v->wid, w);   break;
            case AG_W_PROGRESS: render_progress(v->wid, w); break;
            case AG_W_LISTBOX:  render_listbox(v->wid, w);  break;
            case AG_W_PANEL:    render_panel(v->wid, w);    break;
        }
    }
    ag_render_now();
    (void)W; (void)H;
}

/* Hit-test. */
static int widget_at(ag_view_t *v, int32_t mx, int32_t my) {
    for (int i = v->widget_count - 1; i >= 0; i--) {
        ag_widget_t *w = &v->widgets[i];
        if (!w->visible) continue;
        if (mx >= w->x && mx < w->x + (int32_t)w->w &&
            my >= w->y && my < w->y + (int32_t)w->h) {
            return i;
        }
    }
    return -1;
}

/* Set focus to widget index (or -1 to clear). */
static void set_focus(ag_view_t *v, int idx) {
    if (v->focused_widget == idx) return;
    if (v->focused_widget >= 0 && v->focused_widget < v->widget_count) {
        v->widgets[v->focused_widget].focused = 0;
    }
    v->focused_widget = idx;
    if (idx >= 0 && idx < v->widget_count) v->widgets[idx].focused = 1;
}

static int is_focusable(ag_widget_t *w) {
    if (!w || !w->visible) return 0;
    return w->kind == AG_W_TEXTBOX || w->kind == AG_W_LISTBOX
        || w->kind == AG_W_BUTTON  || w->kind == AG_W_CHECKBOX;
}

static void focus_next(ag_view_t *v, int dir) {
    if (v->widget_count == 0) return;
    int start = v->focused_widget;
    int idx = start;
    for (int i = 0; i < v->widget_count; i++) {
        idx += dir;
        if (idx < 0) idx = v->widget_count - 1;
        if (idx >= v->widget_count) idx = 0;
        if (is_focusable(&v->widgets[idx])) {
            set_focus(v, idx);
            return;
        }
    }
}

/* Insert character into textbox at cursor. */
static void tb_insert(ag_widget_t *w, char c) {
    int len = (int)strlen(w->text);
    if (len + 1 >= AG_MAX_WIDGET_TEXT) return;
    for (int i = len + 1; i > w->cursor_pos; i--) w->text[i] = w->text[i - 1];
    w->text[w->cursor_pos] = c;
    w->cursor_pos++;
}
static void tb_backspace(ag_widget_t *w) {
    if (w->cursor_pos == 0) return;
    int len = (int)strlen(w->text);
    for (int i = w->cursor_pos - 1; i < len; i++) w->text[i] = w->text[i + 1];
    w->cursor_pos--;
}

int ag_view_dispatch(ag_view_t *v, const ag_event_t *e) {
    if (e->type == AG_EVT_CLOSE_REQ) return 1;

    if (e->type == AG_EVT_MOUSE_DOWN || e->type == AG_EVT_MOUSE_DBLCLICK) {
        int idx = widget_at(v, e->x, e->y);
        if (idx >= 0) {
            ag_widget_t *w = &v->widgets[idx];
            switch (w->kind) {
                case AG_W_BUTTON:
                    w->state = 2;
                    if (w->on_click) w->on_click(w, w->user);
                    set_focus(v, idx);
                    break;
                case AG_W_CHECKBOX:
                    w->state = !w->state;
                    if (w->on_change) w->on_change(w, w->user);
                    set_focus(v, idx);
                    break;
                case AG_W_TEXTBOX:
                    set_focus(v, idx);
                    /* Position cursor by mouse X. */
                    int rel = (e->x - w->x - 4) / 8;
                    if (rel < 0) rel = 0;
                    int len = (int)strlen(w->text);
                    if (rel > len) rel = len;
                    w->cursor_pos = rel;
                    break;
                case AG_W_LISTBOX: {
                    int row_h = 14;
                    int row = (e->y - w->y - 2) / row_h;
                    if (row >= 0 && row < w->item_count) {
                        w->selected = row;
                        if (w->on_select) w->on_select(w, w->user);
                    }
                    set_focus(v, idx);
                    break;
                }
                case AG_W_SLIDER: {
                    /* Map x to value. */
                    int32_t span = (int32_t)w->w - 12;
                    int32_t rel  = e->x - w->x - 6;
                    if (rel < 0) rel = 0;
                    if (rel > span) rel = span;
                    w->value = span ? (rel * w->value_max) / span : 0;
                    if (w->on_change) w->on_change(w, w->user);
                    set_focus(v, idx);
                    break;
                }
            }
        } else {
            set_focus(v, -1);
        }
    }
    if (e->type == AG_EVT_MOUSE_UP) {
        /* Release button state. */
        for (int i = 0; i < v->widget_count; i++) {
            if (v->widgets[i].kind == AG_W_BUTTON && v->widgets[i].state == 2) {
                v->widgets[i].state = 0;
            }
        }
    }
    if (e->type == AG_EVT_KEY_DOWN) {
        if (e->key == '\t') {
            focus_next(v, (e->mods & 0x01 /* shift */) ? -1 : 1);
        } else if (v->focused_widget >= 0) {
            ag_widget_t *w = &v->widgets[v->focused_widget];
            if (w->kind == AG_W_TEXTBOX) {
                if (e->mods & 0x02 /* CTRL */) {
                    if (e->key == 'c' || e->key == 'C') {
                        ag_set_clipboard(w->text);
                    } else if (e->key == 'v' || e->key == 'V') {
                        char buf[AG_MAX_WIDGET_TEXT];
                        if (ag_get_clipboard(buf, sizeof(buf)) == 0) {
                            strncpy(w->text, buf, AG_MAX_WIDGET_TEXT - 1);
                            w->text[AG_MAX_WIDGET_TEXT - 1] = '\0';
                            w->cursor_pos = (int)strlen(w->text);
                            if (w->on_change) w->on_change(w, w->user);
                        }
                    }
                } else if (e->key >= 0x20 && e->key < 0x7F) {
                    tb_insert(w, (char)e->key);
                    if (w->on_change) w->on_change(w, w->user);
                } else if (e->key == '\b') {
                    tb_backspace(w);
                    if (w->on_change) w->on_change(w, w->user);
                } else if (e->key == 0x100 /* LEFT */) {
                    if (w->cursor_pos > 0) w->cursor_pos--;
                } else if (e->key == 0x101 /* RIGHT */) {
                    int len = (int)strlen(w->text);
                    if (w->cursor_pos < len) w->cursor_pos++;
                } else if (e->key == 0x104 /* HOME */) {
                    w->cursor_pos = 0;
                } else if (e->key == 0x105 /* END */) {
                    w->cursor_pos = (int)strlen(w->text);
                } else if (e->key == 0x109 /* DELETE */) {
                    int len = (int)strlen(w->text);
                    if (w->cursor_pos < len) {
                        for (int i = w->cursor_pos; i < len; i++) w->text[i] = w->text[i + 1];
                        if (w->on_change) w->on_change(w, w->user);
                    }
                }
            } else if (w->kind == AG_W_LISTBOX) {
                if (e->key == 0x102 /* UP */ && w->selected > 0) {
                    w->selected--;
                    if (w->on_select) w->on_select(w, w->user);
                } else if (e->key == 0x103 /* DOWN */ &&
                           w->selected < w->item_count - 1) {
                    w->selected++;
                    if (w->on_select) w->on_select(w, w->user);
                }
            } else if (w->kind == AG_W_BUTTON && (e->key == '\n' || e->key == ' ')) {
                if (w->on_click) w->on_click(w, w->user);
            } else if (w->kind == AG_W_CHECKBOX && (e->key == '\n' || e->key == ' ')) {
                w->state = !w->state;
                if (w->on_change) w->on_change(w, w->user);
            }
        }
    }
    return 0;
}

void ag_view_run(ag_view_t *v, ag_loop_cb cb, void *user) {
    ag_view_render(v);
    static ag_event_t e;
    for (;;) {
        /* Use non-blocking polling and a static event buffer. AuraLite syscalls
         * currently execute on the issuing user stack; blocking GUI waits and
         * copy_to_user() into stack-local event structs can overlap the kernel's
         * temporary syscall frames. */
        if (!ag_poll_event(v->wid, &e)) {
            for (volatile int i = 0; i < 200000; i++) {}
            continue;
        }
        int quit = ag_view_dispatch(v, &e);
        if (cb) {
            if (cb(v, &e, user)) break;
        }
        ag_view_render(v);
        if (quit) break;
    }
    ag_window_destroy(v->wid);
}

/* ---- Modal helpers (very small) ---- */
void ag_alert(const char *title, const char *message) {
    int W = 320, H = 120;
    int wid = ag_window_create(200, 200, W, H, title,
                               AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_MOVABLE | AG_WIN_MODAL);
    if (wid < 0) return;
    ag_window_show(wid);
    ag_widget_t buf[2];
    ag_view_t v;
    ag_view_init(&v, wid, buf, 2, AG_PANEL);
    ag_add_label(&v, 20, 24, message, AG_BLACK);
    ag_add_button(&v, W - 100, H - 60, 80, 28, "OK", 0, 0);
    for (;;) {
        ag_event_t e;
        if (!ag_wait_event(wid, &e)) continue;
        if (ag_view_dispatch(&v, &e)) break;
        if (e.type == AG_EVT_MOUSE_DOWN) {
            ag_widget_t *btn = &buf[1];
            if (e.x >= btn->x && e.x < btn->x + (int32_t)btn->w &&
                e.y >= btn->y && e.y < btn->y + (int32_t)btn->h) break;
        }
        if (e.type == AG_EVT_KEY_DOWN && (e.key == '\n' || e.key == 0x1B)) break;
        ag_view_render(&v);
    }
    ag_window_destroy(wid);
}
int ag_confirm(const char *title, const char *message) {
    int W = 360, H = 130;
    int wid = ag_window_create(220, 220, W, H, title,
                               AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_MOVABLE | AG_WIN_MODAL);
    if (wid < 0) return 0;
    ag_window_show(wid);
    ag_widget_t buf[3];
    ag_view_t v;
    ag_view_init(&v, wid, buf, 3, AG_PANEL);
    ag_add_label(&v, 20, 24, message, AG_BLACK);
    ag_add_button(&v, W - 200, H - 60, 80, 28, "Yes", 0, 0);
    ag_add_button(&v, W - 100, H - 60, 80, 28, "No",  0, 0);
    int answer = 0;
    for (;;) {
        ag_event_t e;
        if (!ag_wait_event(wid, &e)) continue;
        int quit = ag_view_dispatch(&v, &e);
        if (e.type == AG_EVT_MOUSE_DOWN) {
            ag_widget_t *yes = &buf[1];
            ag_widget_t *no  = &buf[2];
            if (e.x >= yes->x && e.x < yes->x + (int32_t)yes->w &&
                e.y >= yes->y && e.y < yes->y + (int32_t)yes->h) { answer = 1; break; }
            if (e.x >= no->x && e.x < no->x + (int32_t)no->w &&
                e.y >= no->y && e.y < no->y + (int32_t)no->h) { answer = 0; break; }
        }
        if (e.type == AG_EVT_KEY_DOWN) {
            if (e.key == 'y' || e.key == 'Y') { answer = 1; break; }
            if (e.key == 'n' || e.key == 'N' || e.key == 0x1B) { answer = 0; break; }
        }
        if (quit) break;
        ag_view_render(&v);
    }
    ag_window_destroy(wid);
    return answer;
}
