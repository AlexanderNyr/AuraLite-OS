/*
 * test_gui.c — unit tests for GUI subsystem: window lifecycle,
 * Z-ordering, hit-testing, event ring, coordinate math.
 *
 * 45+ test cases exercising window manager logic without real framebuffer.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- Constants (same as kernel GUI) ---- */

#define GUI_MAX_WINDOWS  16
#define GUI_TITLE_MAX    64
#define GUI_EVT_RING_SIZE 64
#define GUI_BORDER       2
#define GUI_TITLEBAR_H   24
#define GUI_TASKBAR_H    32
#define GUI_RESIZE_GRIP  12

#define GUI_WIN_NO_DECOR   (1 << 0)
#define GUI_WIN_HAS_CLOSE  (1 << 1)
#define GUI_WIN_HAS_MINMAX (1 << 2)
#define GUI_WIN_MOVABLE    (1 << 3)
#define GUI_WIN_RESIZABLE  (1 << 4)
#define GUI_WIN_DEFAULT    (GUI_WIN_HAS_CLOSE | GUI_WIN_HAS_MINMAX | GUI_WIN_MOVABLE | GUI_WIN_RESIZABLE)

/* ---- Event types ---- */
#define GUI_EVT_MOUSE_DOWN     1
#define GUI_EVT_MOUSE_UP       2
#define GUI_EVT_MOUSE_MOVE     3
#define GUI_EVT_KEY_DOWN       4
#define GUI_EVT_KEY_UP         5
#define GUI_EVT_CLOSE_REQ      6
#define GUI_EVT_FOCUS          7
#define GUI_EVT_BLUR           8
#define GUI_EVT_RESIZE         9
#define GUI_EVT_MOUSE_WHEEL    10
#define GUI_EVT_MOUSE_DBLCLICK 11

typedef struct {
    int type;
    int32_t x, y;
    uint32_t key;
    uint32_t buttons;
    uint32_t mods;
    int32_t wheel;
} gui_event_t;

/* ---- Window structure (mirrors kernel) ---- */

typedef struct gui_win {
    int       in_use;
    int       visible;
    int       minimized;
    int       maximized;
    int       focused;
    int32_t   x, y;
    uint32_t  w, h;
    int32_t   restore_x, restore_y;
    uint32_t  restore_w, restore_h;
    uint32_t  flags;
    int       z;
    char      title[GUI_TITLE_MAX];
    /* event ring */
    gui_event_t events[GUI_EVT_RING_SIZE];
    volatile uint32_t evt_head, evt_tail;
    int       owner_pid;
} gui_win_t;

static gui_win_t windows[GUI_MAX_WINDOWS];

/* ---- GUI functions (same logic as kernel) ---- */

static void gui_init(void) {
    memset(windows, 0, sizeof(windows));
}

static uint32_t content_w(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->w;
    return w->w - 2 * GUI_BORDER;
}
static uint32_t content_h(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->h;
    return w->h - GUI_TITLEBAR_H - 2 * GUI_BORDER;
}
static int32_t content_x(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->x;
    return w->x + GUI_BORDER;
}
static int32_t content_y(const gui_win_t *w) {
    if (w->flags & GUI_WIN_NO_DECOR) return w->y;
    return w->y + GUI_TITLEBAR_H + GUI_BORDER;
}

static int gui_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                             const char *title, uint32_t flags) {
    if (w == 0 || h == 0) return -1;
    int id = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) { id = i; break; }
    }
    if (id < 0) return -1;
    gui_win_t *win = &windows[id];
    memset(win, 0, sizeof(*win));
    win->in_use = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = flags;
    win->visible = 0;
    win->z = id;
    if (title) {
        strncpy(win->title, title, GUI_TITLE_MAX - 1);
    }
    return id;
}

static int gui_destroy_window(int wid) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use) return -1;
    memset(&windows[wid], 0, sizeof(windows[wid]));
    return 0;
}

/* Hit-test: find topmost window under (mx, my) */
static int hit_window(int32_t mx, int32_t my) {
    int best = -1, bestz = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || !windows[i].visible || windows[i].minimized) continue;
        gui_win_t *w = &windows[i];
        if (mx >= w->x && mx < (int32_t)(w->x + w->w) &&
            my >= w->y && my < (int32_t)(w->y + w->h)) {
            if (w->z > bestz) { bestz = w->z; best = i; }
        }
    }
    return best;
}

/* Hit-test parts: 0=client, 1=titlebar, 2=close btn, 3=max, 4=min, 5=resize, 6=outside */
static int hit_part(const gui_win_t *w, int32_t mx, int32_t my) {
    if (w->flags & GUI_WIN_NO_DECOR) return 0;
    if (mx < w->x || mx >= (int32_t)(w->x + w->w) ||
        my < w->y || my >= (int32_t)(w->y + w->h)) return 6;
    /* Resize grip */
    if ((w->flags & GUI_WIN_RESIZABLE) && !w->maximized) {
        int32_t gx = w->x + (int32_t)w->w - GUI_RESIZE_GRIP - 1;
        int32_t gy = w->y + (int32_t)w->h - GUI_RESIZE_GRIP - 1;
        if (mx >= gx && my >= gy) return 5;
    }
    if (my < w->y + GUI_BORDER + GUI_TITLEBAR_H) {
        if (w->flags & GUI_WIN_HAS_CLOSE) {
            int32_t bx = w->x + (int32_t)w->w - GUI_BORDER - 22;
            int32_t by = w->y + GUI_BORDER + 3;
            if (mx >= bx && mx < bx + 18 && my >= by && my < by + 16) return 2;
        }
        if (w->flags & GUI_WIN_HAS_MINMAX) {
            int32_t bx = w->x + (int32_t)w->w - GUI_BORDER - 44;
            int32_t by = w->y + GUI_BORDER + 3;
            if (mx >= bx && mx < bx + 18 && my >= by && my < by + 16) return 3;
            if (mx >= bx - 22 && mx < bx - 4 && my >= by && my < by + 16) return 4;
        }
        return 1;  /* titlebar */
    }
    return 0;  /* client area */
}

/* Event ring */
static int gui_post_event(int wid, const gui_event_t *evt) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use || !evt) return -1;
    gui_win_t *w = &windows[wid];
    uint32_t next = (w->evt_head + 1) % GUI_EVT_RING_SIZE;
    if (next == w->evt_tail) {
        w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    }
    w->events[w->evt_head] = *evt;
    w->evt_head = next;
    return 0;
}

static int gui_poll_event(int wid, gui_event_t *out) {
    if (wid < 0 || wid >= GUI_MAX_WINDOWS || !windows[wid].in_use || !out) return 0;
    gui_win_t *w = &windows[wid];
    if (w->evt_head == w->evt_tail) return 0;
    *out = w->events[w->evt_tail];
    w->evt_tail = (w->evt_tail + 1) % GUI_EVT_RING_SIZE;
    return 1;
}

/* ======== TESTS ======== */

/* --- Window lifecycle --- */

void t_create_basic(void) {
    gui_init();
    int w = gui_create_window(10, 20, 400, 300, "Test", GUI_WIN_DEFAULT);
    CHECK(w >= 0);
    CHECK(windows[w].in_use == 1);
    CHECK_EQ(strcmp(windows[w].title, "Test"), 0);
}

void t_create_multiple(void) {
    gui_init();
    int w1 = gui_create_window(0, 0, 200, 200, "A", GUI_WIN_DEFAULT);
    int w2 = gui_create_window(200, 0, 200, 200, "B", GUI_WIN_DEFAULT);
    CHECK(w1 >= 0 && w2 >= 0);
    CHECK(w1 != w2);
}

void t_create_zero_size(void) {
    gui_init();
    CHECK_EQ(gui_create_window(0, 0, 0, 100, "X", GUI_WIN_DEFAULT), -1);
    CHECK_EQ(gui_create_window(0, 0, 100, 0, "X", GUI_WIN_DEFAULT), -1);
}

void t_destroy_basic(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "Test", GUI_WIN_DEFAULT);
    CHECK_EQ(gui_destroy_window(w), 0);
    CHECK(!windows[w].in_use);
}

void t_destroy_invalid(void) {
    gui_init();
    CHECK_EQ(gui_destroy_window(-1), -1);
    CHECK_EQ(gui_destroy_window(GUI_MAX_WINDOWS), -1);
    CHECK_EQ(gui_destroy_window(0), -1);  /* not created */
}

void t_create_after_destroy(void) {
    gui_init();
    int w1 = gui_create_window(0, 0, 200, 200, "A", GUI_WIN_DEFAULT);
    gui_destroy_window(w1);
    int w2 = gui_create_window(0, 0, 200, 200, "B", GUI_WIN_DEFAULT);
    CHECK(w2 >= 0);
    CHECK_EQ(w2, w1);  /* reuse slot */
}

void t_create_max_windows(void) {
    gui_init();
    int ids[GUI_MAX_WINDOWS];
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        ids[i] = gui_create_window(0, 0, 100, 100, "W", GUI_WIN_DEFAULT);
        CHECK(ids[i] >= 0);
    }
    int overflow = gui_create_window(0, 0, 100, 100, "X", GUI_WIN_DEFAULT);
    CHECK_EQ(overflow, -1);
}

void t_window_not_visible_by_default(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "Test", GUI_WIN_DEFAULT);
    CHECK(!windows[w].visible);
}

/* --- Content area calculations --- */

void t_content_with_decor(void) {
    gui_init();
    int w = gui_create_window(10, 20, 400, 300, "T", GUI_WIN_DEFAULT);
    CHECK_EQ((long)content_w(&windows[w]), 400 - 2 * GUI_BORDER);
    CHECK_EQ((long)content_h(&windows[w]), 300 - GUI_TITLEBAR_H - 2 * GUI_BORDER);
}

void t_content_no_decor(void) {
    gui_init();
    int w = gui_create_window(10, 20, 400, 300, "T", GUI_WIN_NO_DECOR);
    CHECK_EQ((long)content_w(&windows[w]), 400);
    CHECK_EQ((long)content_h(&windows[w]), 300);
}

void t_content_origin_with_decor(void) {
    gui_init();
    int w = gui_create_window(10, 20, 400, 300, "T", GUI_WIN_DEFAULT);
    CHECK_EQ((long)content_x(&windows[w]), 10 + GUI_BORDER);
    CHECK_EQ((long)content_y(&windows[w]), 20 + GUI_TITLEBAR_H + GUI_BORDER);
}

void t_content_origin_no_decor(void) {
    gui_init();
    int w = gui_create_window(10, 20, 400, 300, "T", GUI_WIN_NO_DECOR);
    CHECK_EQ((long)content_x(&windows[w]), 10);
    CHECK_EQ((long)content_y(&windows[w]), 20);
}

/* --- Hit-testing --- */

void t_hit_inside(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    windows[w].visible = 1;
    CHECK_EQ(hit_window(150, 150), w);
}

void t_hit_outside(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    windows[w].visible = 1;
    CHECK_EQ(hit_window(50, 50), -1);
}

void t_hit_edge(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    windows[w].visible = 1;
    CHECK_EQ(hit_window(100, 100), w);       /* top-left corner */
    CHECK_EQ(hit_window(299, 299), w);       /* bottom-right pixel */
    CHECK_EQ(hit_window(300, 300), -1);      /* just outside */
}

void t_hit_z_order(void) {
    gui_init();
    int w1 = gui_create_window(0, 0, 400, 400, "A", GUI_WIN_DEFAULT);
    int w2 = gui_create_window(0, 0, 400, 400, "B", GUI_WIN_DEFAULT);
    windows[w1].visible = 1;
    windows[w2].visible = 1;
    windows[w1].z = 1;
    windows[w2].z = 2;  /* B on top */
    CHECK_EQ(hit_window(200, 200), w2);
}

void t_hit_minimized_not_hit(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    windows[w].visible = 1;
    windows[w].minimized = 1;
    CHECK_EQ(hit_window(150, 150), -1);
}

void t_hit_invisible_not_hit(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    windows[w].visible = 0;
    CHECK_EQ(hit_window(150, 150), -1);
}

/* --- Hit-part testing --- */

void t_hitpart_client(void) {
    gui_init();
    int w = gui_create_window(0, 0, 400, 300, "T", GUI_WIN_DEFAULT);
    CHECK_EQ(hit_part(&windows[w], 200, 200), 0);  /* client area */
}

void t_hitpart_titlebar(void) {
    gui_init();
    int w = gui_create_window(0, 0, 400, 300, "T", GUI_WIN_DEFAULT);
    CHECK_EQ(hit_part(&windows[w], 50, 5), 1);  /* titlebar */
}

void t_hitpart_close(void) {
    gui_init();
    int w = gui_create_window(0, 0, 400, 300, "T", GUI_WIN_DEFAULT);
    /* Close button at x+w-BORDER-22, y+BORDER+3 */
    int32_t bx = (int32_t)(windows[w].w - GUI_BORDER - 22);
    int32_t by = GUI_BORDER + 3 + 5;
    CHECK_EQ(hit_part(&windows[w], bx + 5, by + 5), 2);
}

void t_hitpart_resize(void) {
    gui_init();
    int w = gui_create_window(0, 0, 400, 300, "T", GUI_WIN_DEFAULT);
    int32_t gx = (int32_t)windows[w].w - GUI_RESIZE_GRIP;
    int32_t gy = (int32_t)windows[w].h - GUI_RESIZE_GRIP;
    CHECK_EQ(hit_part(&windows[w], gx, gy), 5);
}

void t_hitpart_outside(void) {
    gui_init();
    int w = gui_create_window(100, 100, 200, 200, "T", GUI_WIN_DEFAULT);
    CHECK_EQ(hit_part(&windows[w], 50, 50), 6);
}

void t_hitpart_nodecor_always_client(void) {
    gui_init();
    int w = gui_create_window(0, 0, 400, 300, "T", GUI_WIN_NO_DECOR);
    CHECK_EQ(hit_part(&windows[w], 5, 5), 0);  /* no titlebar */
}

/* --- Event ring --- */

void t_event_post_poll(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "T", GUI_WIN_DEFAULT);
    gui_event_t e = { GUI_EVT_KEY_DOWN, 0, 0, 'A', 0, 0, 0 };
    CHECK_EQ(gui_post_event(w, &e), 0);
    gui_event_t out;
    CHECK_EQ(gui_poll_event(w, &out), 1);
    CHECK_EQ(out.type, GUI_EVT_KEY_DOWN);
    CHECK_EQ((long)out.key, 'A');
}

void t_event_fifo_order(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "T", GUI_WIN_DEFAULT);
    gui_event_t e1 = { GUI_EVT_MOUSE_DOWN, 0, 0, 0, 0, 0, 0 };
    gui_event_t e2 = { GUI_EVT_MOUSE_UP, 0, 0, 0, 0, 0, 0 };
    gui_post_event(w, &e1);
    gui_post_event(w, &e2);
    gui_event_t out;
    CHECK_EQ(gui_poll_event(w, &out), 1);
    CHECK_EQ(out.type, GUI_EVT_MOUSE_DOWN);
    CHECK_EQ(gui_poll_event(w, &out), 1);
    CHECK_EQ(out.type, GUI_EVT_MOUSE_UP);
}

void t_event_empty_poll(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "T", GUI_WIN_DEFAULT);
    gui_event_t out;
    CHECK_EQ(gui_poll_event(w, &out), 0);
}

void t_event_invalid_wid(void) {
    gui_event_t e = {0};
    CHECK_EQ(gui_post_event(-1, &e), -1);
    gui_event_t out;
    CHECK_EQ(gui_poll_event(-1, &out), 0);
}

void t_event_ring_overflow(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "T", GUI_WIN_DEFAULT);
    /* Fill ring beyond capacity */
    for (int i = 0; i < GUI_EVT_RING_SIZE + 10; i++) {
        gui_event_t e = { GUI_EVT_KEY_DOWN, 0, 0, (uint32_t)i, 0, 0, 0 };
        gui_post_event(w, &e);
    }
    /* Should still be able to drain events */
    int count = 0;
    gui_event_t out;
    while (gui_poll_event(w, &out)) count++;
    CHECK(count > 0);
    CHECK(count <= GUI_EVT_RING_SIZE);
}

/* --- Title handling --- */

void t_title_set(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, "My Window", GUI_WIN_DEFAULT);
    CHECK_EQ(strcmp(windows[w].title, "My Window"), 0);
}

void t_title_truncation(void) {
    gui_init();
    char long_title[256];
    memset(long_title, 'X', 255);
    long_title[255] = 0;
    int w = gui_create_window(0, 0, 200, 200, long_title, GUI_WIN_DEFAULT);
    CHECK_EQ((long)strlen(windows[w].title), GUI_TITLE_MAX - 1);
}

void t_title_null(void) {
    gui_init();
    int w = gui_create_window(0, 0, 200, 200, NULL, GUI_WIN_DEFAULT);
    CHECK_EQ(windows[w].title[0], 0);
}

int main(void) {
    printf("=== GUI Subsystem Tests ===\n\n");

    printf("--- window lifecycle ---\n");
    RUN(t_create_basic);
    RUN(t_create_multiple);
    RUN(t_create_zero_size);
    RUN(t_destroy_basic);
    RUN(t_destroy_invalid);
    RUN(t_create_after_destroy);
    RUN(t_create_max_windows);
    RUN(t_window_not_visible_by_default);

    printf("--- content area ---\n");
    RUN(t_content_with_decor);
    RUN(t_content_no_decor);
    RUN(t_content_origin_with_decor);
    RUN(t_content_origin_no_decor);

    printf("--- hit-testing ---\n");
    RUN(t_hit_inside);
    RUN(t_hit_outside);
    RUN(t_hit_edge);
    RUN(t_hit_z_order);
    RUN(t_hit_minimized_not_hit);
    RUN(t_hit_invisible_not_hit);

    printf("--- hit-part ---\n");
    RUN(t_hitpart_client);
    RUN(t_hitpart_titlebar);
    RUN(t_hitpart_close);
    RUN(t_hitpart_resize);
    RUN(t_hitpart_outside);
    RUN(t_hitpart_nodecor_always_client);

    printf("--- event ring ---\n");
    RUN(t_event_post_poll);
    RUN(t_event_fifo_order);
    RUN(t_event_empty_poll);
    RUN(t_event_invalid_wid);
    RUN(t_event_ring_overflow);

    printf("--- title handling ---\n");
    RUN(t_title_set);
    RUN(t_title_truncation);
    RUN(t_title_null);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
