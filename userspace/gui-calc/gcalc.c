/* gcalc — GUI calculator built on libauragui. */

#include "auragui.h"
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int wid;
static ag_widget_t widgets[40];
static ag_view_t view;
static ag_widget_t *display;

static char expr[64];
static long current = 0;
static long stored = 0;
static char pending_op = 0;
static int  start_fresh = 1;

static void show_value(long v) {
    char buf[32];
    int n = 0;
    long x = v;
    int neg = 0;
    if (x < 0) { neg = 1; x = -x; }
    char tmp[24];
    int t = 0;
    if (x == 0) tmp[t++] = '0';
    while (x) { tmp[t++] = '0' + x % 10; x /= 10; }
    if (neg) tmp[t++] = '-';
    while (t-- > 0) buf[n++] = tmp[t];
    buf[n] = 0;
    ag_textbox_set(display, buf);
    strncpy(expr, buf, sizeof(expr) - 1);
}

static long apply_op(long a, long b, char op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b ? a / b : 0;
        default:  return b;
    }
}

static void on_digit(ag_widget_t *w, void *u) {
    (void)u;
    int d = w->text[0] - '0';
    if (start_fresh) { current = 0; start_fresh = 0; }
    current = current * 10 + d;
    show_value(current);
}

static void on_op(ag_widget_t *w, void *u) {
    (void)u;
    if (pending_op && !start_fresh) {
        stored = apply_op(stored, current, pending_op);
    } else {
        stored = current;
    }
    pending_op = w->text[0];
    current = 0;
    start_fresh = 1;
    show_value(stored);
}

static void on_eq(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    if (pending_op) {
        stored = apply_op(stored, current, pending_op);
        current = stored;
        pending_op = 0;
        start_fresh = 1;
        show_value(stored);
    }
}

static void on_clear(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    current = 0; stored = 0; pending_op = 0; start_fresh = 1;
    show_value(0);
}

static void on_neg(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    current = -current;
    show_value(current);
}

int main(void) {
    wid = ag_window_create(140, 100, 240, 320, "Calculator", AG_WIN_DEFAULT & ~AG_WIN_RESIZABLE);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 40, 0x00F0F2F8);

    display = ag_add_textbox(&view, 12, 12, 216, 36, "0");

    /* Button layout. */
    struct { const char *t; int r, c; ag_callback_t cb; } btns[] = {
        {"C", 0, 0, on_clear}, {"+/-", 0, 1, on_neg}, {"/", 0, 3, on_op}, {"*", 0, 2, on_op},
        {"7", 1, 0, on_digit}, {"8", 1, 1, on_digit}, {"9", 1, 2, on_digit}, {"-", 1, 3, on_op},
        {"4", 2, 0, on_digit}, {"5", 2, 1, on_digit}, {"6", 2, 2, on_digit}, {"+", 2, 3, on_op},
        {"1", 3, 0, on_digit}, {"2", 3, 1, on_digit}, {"3", 3, 2, on_digit}, {"=", 3, 3, on_eq},
        {"0", 4, 0, on_digit},
    };
    int n = sizeof(btns) / sizeof(btns[0]);
    int x0 = 12, y0 = 60, bw = 50, bh = 44, gap = 6;
    for (int i = 0; i < n; i++) {
        int W = bw;
        int X = x0 + btns[i].c * (bw + gap);
        if (btns[i].r == 4 && btns[i].c == 0) W = bw * 2 + gap; /* "0" wide */
        ag_widget_t *b = ag_add_button(&view, X, y0 + btns[i].r * (bh + gap), W, bh,
                                       btns[i].t, btns[i].cb, 0);
        if (btns[i].t[0] == '=') b->bg = AG_GREEN;
        else if (btns[i].t[0] == 'C') b->bg = AG_RED;
        else if (btns[i].cb == on_op) b->bg = AG_ORANGE;
        else b->bg = AG_ACCENT;
    }
    show_value(0);
    ag_view_run(&view, 0, 0);
    return 0;
}
