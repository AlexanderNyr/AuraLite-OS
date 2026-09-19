/* test_w32_a5.c — W32APP_PLAN.md phase W32A-5: the USER32 window/message core.
 *
 * What is real in here:
 *   - the personality's own code: w32/src/user32_win.c (classes, windows,
 *     queues, geometry, paint, scroll, metrics, input) and w32/src/user32.c
 *     (the DC half), included directly, exactly as the W32-1/W32-2/W32A-3
 *     host gates do;
 *   - real pthreads, so cross-thread SendMessageW runs through a production
 *     scheduler rather than a simulation.  That is the phase's headline
 *     claim and it is asserted, not described;
 *   - a fake compositor whose theme is mutable, which is what makes
 *     "GetSystemMetrics/GetSysColor answer from the compositor theme"
 *     testable instead of a comment.
 *
 * What is not: the compositor itself.  Every ag_* call lands in the table in
 * this file; the guest fixture (w32/tests/w32a5_win.asm, run by
 * tests/integration/cases/test_w32a5_user32win.sh) asserts the same
 * behaviours against the real one, pixels included.
 *
 * Run: build/test_w32_a5
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "w32/w32_abi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"
#include "w32/w32_teb.h"

/* The suite #includes the implementation rather than an archive, so the
 * kernel32 primitives it calls are the ones below. */
static __thread W32_DWORD host_tid;

W32ABI W32_DWORD GetCurrentThreadId(void) {
    if (!host_tid) host_tid = (W32_DWORD)(uintptr_t)pthread_self();
    return host_tid;
}
W32ABI W32_DWORD GetCurrentProcessId(void) { return 4242; }

static W32_DWORD tick_ms;
W32ABI W32_DWORD GetTickCount(void) { return tick_ms; }
W32ABI W32_ULONGLONG GetTickCount64(void) { return tick_ms; }

/* The TEB: W32A-3 keeps LastError in it (+0x48) and w32_errno.c reads it
 * through this accessor.  On the host, one TEB per thread is what the suite
 * needs -- no segment register, no kernel. */
static __thread struct w32_teb host_teb;
struct w32_teb *w32_teb_self(void) { return &host_teb; }

W32ABI W32_DWORD GetLastError(void) { return w32_get_last_error_raw(); }

struct host_ev { pthread_mutex_t m; pthread_cond_t c; int set; };
#define HOST_EV_MAX 64
static struct host_ev *evs[HOST_EV_MAX];

W32ABI W32_HANDLE CreateEventW(void *sec, W32_BOOL manual, W32_BOOL initial,
                        const uint16_t *name) {
    (void)sec; (void)manual; (void)name;
    for (int i = 0; i < HOST_EV_MAX; i++) {
        if (evs[i]) continue;
        evs[i] = calloc(1, sizeof(struct host_ev));
        pthread_mutex_init(&evs[i]->m, 0);
        pthread_cond_init(&evs[i]->c, 0);
        evs[i]->set = initial ? 1 : 0;
        return (W32_HANDLE)(uintptr_t)(i + 1);
    }
    return 0;
}
W32ABI W32_BOOL SetEvent(W32_HANDLE h) {
    int i = (int)(uintptr_t)h - 1;
    if (i < 0 || i >= HOST_EV_MAX || !evs[i]) return W32_FALSE;
    pthread_mutex_lock(&evs[i]->m);
    evs[i]->set = 1;
    pthread_cond_broadcast(&evs[i]->c);
    pthread_mutex_unlock(&evs[i]->m);
    return W32_TRUE;
}
W32ABI W32_DWORD WaitForSingleObject(W32_HANDLE h, W32_DWORD ms) {
    int i = (int)(uintptr_t)h - 1;
    (void)ms;
    if (i < 0 || i >= HOST_EV_MAX || !evs[i]) return 0xFFFFFFFFu;
    pthread_mutex_lock(&evs[i]->m);
    while (!evs[i]->set) pthread_cond_wait(&evs[i]->c, &evs[i]->m);
    pthread_mutex_unlock(&evs[i]->m);
    return 0;                                   /* WAIT_OBJECT_0 */
}
W32ABI W32_DWORD WaitForMultipleObjects(W32_DWORD n, W32_HANDLE *hs,
                                 W32_BOOL all, W32_DWORD ms) {
    (void)all;
    if (n == 0) return 0xFFFFFFFFu;
    return WaitForSingleObject(hs[0], ms);
}
W32ABI W32_BOOL CloseHandle(W32_HANDLE h) {
    int i = (int)(uintptr_t)h - 1;
    if (i < 0 || i >= HOST_EV_MAX || !evs[i]) return W32_FALSE;
    pthread_mutex_destroy(&evs[i]->m);
    pthread_cond_destroy(&evs[i]->c);
    free(evs[i]);
    evs[i] = 0;
    return W32_TRUE;
}
W32ABI void Sleep(W32_DWORD ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

/* ---- the implementation under test -------------------------------------- */

#define AURALITE_W32_HOST_TEST 1
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/user32_win.c"
#include "../../w32/src/user32.c"
#include "../../w32/src/w32_gdi.c"      /* W32A-7: the DC/drawing half */

/* ---- the fake compositor ------------------------------------------------ */
/* Signatures must match the mirror declarations in user32_win.c's host block
 * (same types: ui_theme_mirror_t / ui_event_mirror_t). */

typedef struct {
    int in_use;
    int32_t x, y;
    uint32_t w, h;
    int visible;
    int z;
    uint32_t flags;
    char title[128];
    int invalidated;
    struct { uint32_t type; int32_t x, y; uint32_t key; uint8_t buttons, mods;
             uint16_t data; } evq[32];
    int evq_head, evq_tail;
} fake_win_t;

#define FAKE_WINS 64
static fake_win_t fw[FAKE_WINS];
static uint32_t fake_screen_w = 1280, fake_screen_h = 800;
static int fake_focused, fake_capture = -1;
static int32_t fake_mouse_x = 100, fake_mouse_y = 100;

static ui_theme_mirror_t theme = {
    .desktop_top = 0x00102030u, .desktop_bot = 0x00405060u,
    .win_bg = 0x00EEEEEEu, .win_content = 0x00FFFFFFu,
    .title_active = 0x000A0A0Au, .title_inactive = 0x00606060u,
    .title_text = 0x00F0F0F0u,
    .border = 0x00333333u, .border_active = 0x002F60C0u,
    .taskbar_bg = 0x00202020u, .taskbar_border = 0x00404040u,
    .taskbar_text = 0x00FFFFFFu,
    .start_btn_bg = 0x00303030u, .start_btn_text = 0x00FFFFFFu,
    .close_bg = 0x00C03030u, .close_bg_hover = 0x00E04040u,
    .max_bg = 0x00404040u, .min_bg = 0x00404040u,
    .icon_text = 0x00FFFFFFu, .icon_selected = 0x002F60C0u,
    .notif_bg = 0x00F0F0F0u, .notif_border = 0x00AAAAAAu,
    .notif_text = 0x00101010u,
    .shadow_color = 0x00000000u, .shadow_offset = 3,
    .taskbar_h = 40, .titlebar_h = 24, .border_w = 2, .resize_grip = 6,
    .icon_size = 48, .icon_pad = 8, .win_round = 6,
};

/* The draw calls the personality makes, counted so "it painted" is asserted
 * rather than assumed. */
static int clears, fills, renders;

int ag_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                     const char *title, uint32_t flags) {
    for (int i = 0; i < FAKE_WINS; i++) {
        if (fw[i].in_use) continue;
        fw[i].in_use = 1;
        fw[i].x = x; fw[i].y = y; fw[i].w = w; fw[i].h = h;
        fw[i].visible = 0; fw[i].z = i + 1; fw[i].flags = flags;
        fw[i].invalidated = 0; fw[i].evq_head = fw[i].evq_tail = 0;
        memset(fw[i].title, 0, sizeof fw[i].title);
        snprintf(fw[i].title, sizeof fw[i].title, "%s", title ? title : "");
        return i + 1;                          /* 1-based wid; 0 would be none */
    }
    return -1;
}
int ag_window_show(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].visible = 1;
    return 0;
}
int ag_window_hide(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].visible = 0;
    return 0;
}
int ag_window_destroy(int wid) {
    if (wid < 1 || wid > FAKE_WINS) return -1;
    fw[wid-1].in_use = 0;
    return 0;
}
int ag_window_focus(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fake_focused = wid;
    fw[wid-1].z = 9999;
    return 0;
}
int ag_window_minimize(int wid) { (void)wid; return 0; }
int ag_window_maximize(int wid) { (void)wid; return 0; }
int ag_window_restore(int wid) { (void)wid; return 0; }
int ag_window_move(int wid, int32_t x, int32_t y) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].x = x; fw[wid-1].y = y;
    return 0;
}
int ag_window_resize(int wid, uint32_t w, uint32_t h) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].w = w; fw[wid-1].h = h;
    return 0;
}
int ag_window_set_title(int wid, const char *t) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    memset(fw[wid-1].title, 0, sizeof fw[wid-1].title);
    snprintf(fw[wid-1].title, sizeof fw[wid-1].title, "%s", t ? t : "");
    return 0;
}
int ag_window_invalidate(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].invalidated = 1;
    return 0;
}
int ag_window_invalidate_rect(int wid, int32_t x, int32_t y,
                              uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)w; (void)h;
    return ag_window_invalidate(wid);
}
int ag_window_get_size(int wid, uint32_t *w, uint32_t *h) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    if (w) *w = fw[wid-1].w;
    if (h) *h = fw[wid-1].h;
    return 0;
}
int ag_window_get_pos(int wid, int32_t *x, int32_t *y) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    if (x) *x = fw[wid-1].x;
    if (y) *y = fw[wid-1].y;
    return 0;
}
int ag_window_lower(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].z = -1;
    return 0;
}
int ag_window_set_flags(int wid, uint32_t flags) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    fw[wid-1].flags = flags;
    return 0;
}
uint32_t ag_window_get_flags(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return 0xFFFFFFFFu;
    return fw[wid-1].flags;
}
int ag_window_get_z(int wid) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    return fw[wid-1].z;
}
int ag_window_capture(int wid) {
    if (wid > 0 && (wid > FAKE_WINS || !fw[wid-1].in_use)) return -1;
    int prev = fake_capture;
    fake_capture = wid;
    return prev;
}
int ag_window_get_capture(void) { return fake_capture; }
int ag_window_focused(void) { return fake_focused; }
int ag_window_top(void) { return fake_focused; }
int ag_screen_size(uint32_t *w, uint32_t *h) {
    if (w) *w = fake_screen_w;
    if (h) *h = fake_screen_h;
    return 0;
}
int ag_mouse_position(int32_t *x, int32_t *y) {
    if (x) *x = fake_mouse_x;
    if (y) *y = fake_mouse_y;
    return 0;
}
int ag_theme_get(ui_theme_t *out) { if (out) *out = theme; return 0; }
int ag_poll_event(int wid, ui_event_t *out) {
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use || !out) return -1;
    fake_win_t *w = &fw[wid-1];
    if (w->evq_head == w->evq_tail) return 0;
    memcpy(out, &w->evq[w->evq_head], sizeof w->evq[0]);
    w->evq_head = (w->evq_head + 1) % 32;
    return 1;
}
int ag_clear(int wid, uint32_t color) { (void)wid; (void)color; clears++; return 0; }
/* W32A-7 additions the raster engine calls (see w32_gdi.c's host block). */
int ag_blit_alpha(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                  const uint32_t *argb, uint32_t stride) {
    (void)wid; (void)x; (void)y; (void)w; (void)h; (void)argb; (void)stride;
    return 0;
}
int ag_get_pixel(int wid, int32_t x, int32_t y) {
    (void)x; (void)y;
    if (wid < 1 || wid > FAKE_WINS || !fw[wid-1].in_use) return -1;
    return 0;
}
uint32_t w32_gdi_host_dpi(void) { return 96; }
int ag_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                 uint32_t c) {
    (void)wid; (void)x; (void)y; (void)w; (void)h; (void)c;
    fills++;
    return 0;
}
int ag_draw_text(int wid, int32_t x, int32_t y, const char *s, uint32_t c) {
    (void)wid; (void)x; (void)y; (void)s; (void)c;
    return 0;
}
int ag_draw_pixel(int wid, int32_t x, int32_t y, uint32_t c) {
    (void)wid; (void)x; (void)y; (void)c;
    return 0;
}
int ag_draw_line(int wid, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 uint32_t c) {
    (void)wid; (void)x0; (void)y0; (void)x1; (void)y1; (void)c;
    return 0;
}
void ag_render_now(void) { renders++; }
void ag_alert(const char *t, const char *m) { (void)t; (void)m; }

static void fake_event(int wid, uint32_t type, int32_t x, int32_t y,
                       uint32_t key, uint8_t buttons, uint8_t mods,
                       uint16_t data) {
    fake_win_t *w = &fw[wid-1];
    int next = (w->evq_tail + 1) % 32;
    if (next == w->evq_head) return;              /* full: drop, like a driver */
    w->evq[w->evq_tail].type = type;
    w->evq[w->evq_tail].x = x;
    w->evq[w->evq_tail].y = y;
    w->evq[w->evq_tail].key = key;
    w->evq[w->evq_tail].buttons = buttons;
    w->evq[w->evq_tail].mods = mods;
    w->evq[w->evq_tail].data = data;
    w->evq_tail = next;
}

/* wid of the first live fake window whose compositor id is n (helper for the
 * "the title really changed" assertion). */
static const char *fake_title(int wid) {
    if (wid < 1 || wid > FAKE_WINS) return "";
    return fw[wid-1].title;
}

/* ---- the harness --------------------------------------------------------- */

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (cond) printf("ok - %s\n", what);
    else { failures++; printf("not ok - %s\n", what); }
}

static int seen_n;
static W32_UINT seen_msg[64];

static W32_LRESULT W32ABI wndproc_logged(W32_HWND h, W32_UINT m,
                                         W32_WPARAM wp, W32_LPARAM lp) {
    if (seen_n < 64) seen_msg[seen_n++] = m;
    return DefWindowProcW(h, m, wp, lp);
}

/* A subclass that records what it saw and hands the rest down the chain. */
static int sub_seen_text;
static W32_WNDPROC sub_prev;
static W32_LRESULT W32ABI wndproc_sub(W32_HWND h, W32_UINT m,
                                      W32_WPARAM wp, W32_LPARAM lp) {
    if (m == W32_WM_SETTEXT) sub_seen_text++;
    return CallWindowProcW(sub_prev, h, m, wp, lp);
}

static void drain(void) {
    W32_MSG m;
    while (PeekMessageW(&m, 0, 0, 0, 1)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static const uint16_t CLS[8]   = { 'T','e','s','t','W','i','n', 0 };
static const uint16_t TITLE[6] = { 'h','e','l','l','o', 0 };
static const uint16_t CLS_P[6] = { 'P','u','m','p','X', 0 };

static W32_WNDCLASSEXW mkclass(const uint16_t *name, W32_WNDPROC p) {
    W32_WNDCLASSEXW c;
    memset(&c, 0, sizeof c);
    c.cbSize = sizeof c;
    c.lpfnWndProc = p;
    c.hbrBackground = (W32_HBRUSH)(uintptr_t)0x00FFFFFFu;
    c.lpszClassName = name;
    return c;
}

/* ---- the cross-thread pump ---------------------------------------------- */

struct pump_ctx {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int ready;
    W32_HWND hwnd;
    W32_DWORD tid;
    W32_DWORD proc_tid;      /* the thread the window procedure ran on */
    W32_LRESULT send_result;
    int posted_seen;
    int extra_seen;
};
static struct pump_ctx P;

#define MSG_SEND_PROBE  0x8004u
#define MSG_POST_PROBE  0x8006u
#define MSG_POST_EXTRA  0x8007u
#define MSG_PUMP_QUIT   0x9000u

static W32_LRESULT W32ABI wndproc_pump(W32_HWND h, W32_UINT m,
                                       W32_WPARAM wp, W32_LPARAM lp) {
    if (m == MSG_SEND_PROBE) {
        /* Cross-thread send: this runs on the owner thread, and the sender is
         * blocked until we return. */
        P.proc_tid = GetCurrentThreadId();
        P.send_result = (W32_LRESULT)(wp + 1);
        return P.send_result;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void *pump_main(void *arg) {
    (void)arg;
    P.tid = GetCurrentThreadId();
    P.hwnd = CreateWindowExW(0, CLS_P, CLS_P, W32_WS_CAPTION, 5, 5, 120, 90,
                             0, 0, 0, 0);
    pthread_mutex_lock(&P.m);
    P.ready = 1;
    pthread_cond_signal(&P.c);
    pthread_mutex_unlock(&P.m);

    W32_MSG m;
    for (;;) {
        if (PeekMessageW(&m, 0, 0, 0, 1)) {
            if (m.message == MSG_PUMP_QUIT) break;
            if (m.message == MSG_POST_PROBE) P.posted_seen = (int)m.wParam;
            if (m.message == MSG_POST_EXTRA) P.extra_seen++;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        } else {
            Sleep(1);
        }
    }
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    /* Unbuffered: this suite spawns threads and blocks in a cross-thread
     * send, so a hang must not swallow the log that says where it hung. */
    setvbuf(stdout, 0, _IONBF, 0);

    printf("== W32A-5: USER32 window/message core (fake compositor, real "
           "threads) ==\n");

    /* ---- rectangle helpers ---- */
    {
        W32_RECT a = { 10, 10, 50, 50 }, b = { 30, 30, 70, 70 }, d;
        ok(EqualRect(&a, &a) == W32_TRUE, "EqualRect: identical");
        ok(EqualRect(&a, &b) == W32_FALSE, "EqualRect: different");
        W32_RECT c = a;
        InflateRect(&c, 5, 5);
        ok(c.left == 5 && c.top == 5 && c.right == 55 && c.bottom == 55,
           "InflateRect grows both axes");
        ok(IntersectRect(&d, &a, &b) == W32_TRUE && d.left == 30 && d.top == 30 &&
           d.right == 50 && d.bottom == 50,
           "IntersectRect clamps to the overlap");
        W32_RECT e = { 0, 0, 10, 10 }, f = { 40, 40, 50, 50 };
        ok(IntersectRect(&d, &e, &f) == W32_FALSE,
           "IntersectRect reports an empty overlap");
        W32_RECT g = a;
        OffsetRect(&g, -10, 5);
        ok(g.left == 0 && g.top == 15 && g.right == 40 && g.bottom == 55,
           "OffsetRect translates");
        W32_POINT in = { 10, 10 }, out = { 50, 50 };
        ok(PtInRect(&a, in) == W32_TRUE && PtInRect(&a, out) == W32_FALSE,
           "PtInRect includes the top-left and excludes the bottom-right");
        W32_RECT z;
        SetRectEmpty(&z);
        ok(IsRectEmpty(&z) == W32_TRUE && z.right == 0,
           "SetRectEmpty then IsRectEmpty");
    }

    /* ---- classes ---- */
    W32_WNDCLASSEXW cw = mkclass(CLS, wndproc_logged);
    W32_WORD atom = RegisterClassExW(&cw);
    ok(atom != 0, "RegisterClassExW returns an atom");
    {
        W32_WNDCLASSEXW dup = mkclass(CLS, wndproc_logged);
        ok(RegisterClassExW(&dup) == 0 && GetLastError() == W32_ERROR_ALREADY_EXISTS,
           "a duplicate class name is refused by name");
    }
    {
        W32_WNDCLASSEXA ca;
        memset(&ca, 0, sizeof ca);
        ca.cbSize = sizeof ca;
        ca.lpfnWndProc = wndproc_logged;
        ca.hbrBackground = (W32_HBRUSH)(uintptr_t)0x00FFFFFFu;
        ca.lpszClassName = "AOnly";
        ok(RegisterClassA(&ca) != 0, "RegisterClassA registers through the W core");
        W32_HWND h = CreateWindowExA(0, "AOnly", "a", W32_WS_POPUP, 5, 6, 100, 80,
                                     0, 0, 0, 0);
        char name[64];
        ok(h != 0 && GetClassNameA(h, name, (int32_t)sizeof name) > 0 &&
           strcmp(name, "AOnly") == 0,
           "CreateWindowExA + GetClassNameA round-trip a class name");
        DestroyWindow(h);
    }

    /* ---- creation: the NCCREATE/CREATE order, and refusals ---- */
    seen_n = 0;
    W32_HWND h1 = CreateWindowExW(0, CLS, TITLE,
                                  W32_WS_CAPTION | W32_WS_SYSMENU | W32_WS_VISIBLE,
                                  100, 100, 400, 300, 0, 0, 0, 0);
    ok(h1 != 0 && IsWindow(h1) == W32_TRUE, "CreateWindowExW creates a window");
    ok(seen_n >= 2 && seen_msg[0] == W32_WM_NCCREATE && seen_msg[1] == W32_WM_CREATE,
       "WM_NCCREATE arrives before WM_CREATE");
    {
        W32_HWND bad = CreateWindowExW(0, CLS, TITLE, W32_WS_CHILD, 0, 0, 10, 10,
                                       0, 0, 0, 0);
        ok(bad == 0 && GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "WS_CHILD is refused by name (there is no child compositing)");
    }
    {
        /* WS_MINIMIZE has no compositor meaning here -- minimize is a
         * ShowWindow command -- so it is refused, not silently dropped. */
        W32_HWND bad = CreateWindowExW(0, CLS, TITLE, 0x20000000u, 0, 0, 10, 10,
                                       0, 0, 0, 0);
        ok(bad == 0 && GetLastError() == W32_ERROR_INVALID_PARAMETER,
           "a style bit with no compositor meaning is refused");
    }
    {
        const uint16_t nope[5] = { 'n','o','p','e', 0 };
        W32_HWND bad = CreateWindowExW(0, nope, TITLE, W32_WS_POPUP, 0, 0, 10, 10,
                                       0, 0, 0, 0);
        ok(bad == 0 && GetLastError() == W32_ERROR_CLASS_DOES_NOT_EXIST,
           "an unregistered class is refused");
    }
    drain();                                   /* drop the create-time paint */

    /* ---- geometry: the compositor holds the position, the theme the frame -- */
    {
        W32_RECT r, cr;
        ok(GetWindowRect(h1, &r) == W32_TRUE && r.left == 100 && r.top == 100 &&
           r.right == 100 + 400 + 2 * 2 && r.bottom == 100 + 300 + 24 + 2 * 2,
           "GetWindowRect adds the decoration the theme asks for");
        ok(GetClientRect(h1, &cr) == W32_TRUE &&
           cr.left == 0 && cr.top == 0 && cr.right == 400 && cr.bottom == 300,
           "GetClientRect is the compositor's content size");
        W32_POINT p = { 0, 0 };
        ClientToScreen(h1, &p);
        ok(p.x == 102 && p.y == 126, "ClientToScreen adds border + titlebar");
        ScreenToClient(h1, &p);
        ok(p.x == 0 && p.y == 0, "ScreenToClient inverts it");
        W32_RECT want = { 0, 0, 400, 300 };
        ok(AdjustWindowRectEx(&want, W32_WS_CAPTION, W32_FALSE, 0) == W32_TRUE &&
           want.right - want.left == 404 && want.bottom - want.top == 328,
           "AdjustWindowRectEx uses the same theme deltas");
    }

    /* ---- subclassing ---- */
    {
        W32_WNDPROC prev = (W32_WNDPROC)GetWindowLongPtrW(h1, W32_GWL_WNDPROC);
        ok(prev == wndproc_logged,
           "GetWindowLongPtrW(GWLP_WNDPROC) returns the class procedure");
        sub_prev = prev;
        seen_n = 0;
        sub_seen_text = 0;
        ok(SetWindowLongPtrW(h1, W32_GWL_WNDPROC, (intptr_t)wndproc_sub) != 0 &&
           (W32_WNDPROC)GetWindowLongPtrW(h1, W32_GWL_WNDPROC) == wndproc_sub,
           "SetWindowLongPtrW installs a subclass");
        SendMessageW(h1, W32_WM_SETTEXT, 0, (W32_LPARAM)(uintptr_t)TITLE);
        int class_saw = 0;
        for (int i = 0; i < seen_n; i++)
            if (seen_msg[i] == W32_WM_SETTEXT) class_saw = 1;
        ok(sub_seen_text == 1 && class_saw,
           "the subclass sees the message and CallWindowProcW reaches the class");
        ok(CallWindowProcW(prev, h1, W32_WM_NULL, 0, 0) == 0,
           "CallWindowProcW can call the previous procedure directly");
        SetWindowLongPtrW(h1, W32_GWL_WNDPROC, (intptr_t)prev);
    }

    /* ---- window text ---- */
    {
        const uint16_t hi[3] = { 'h','i', 0 };
        uint16_t buf[32];
        char abuf[32];
        SetWindowTextW(h1, hi);
        ok(GetWindowTextW(h1, buf, 32) == 2 && buf[0] == 'h' && buf[1] == 'i',
           "SetWindowTextW then GetWindowTextW round-trips");
        ok(GetWindowTextA(h1, abuf, 32) == 2 && strcmp(abuf, "hi") == 0,
           "GetWindowTextA converts to UTF-8");
        ok(GetWindowTextLengthW(h1) == 2 && GetWindowTextLengthA(h1) == 2,
           "GetWindowTextLength agrees in both directions");
        ok(GetClassNameW(h1, buf, 32) == 7, "GetClassNameW reports the class");
        ok(strcmp(fake_title(1), "hi") == 0,
           "the compositor's title bar was updated too");
    }

    /* ---- properties ---- */
    {
        const uint16_t k1[3] = { 'k','1', 0 };
        W32_HANDLE v = (W32_HANDLE)(uintptr_t)0xBEEF;
        ok(SetPropW(h1, k1, v) == W32_TRUE && GetPropW(h1, k1) == v,
           "SetPropW/GetPropW round-trip a handle");
        ok(RemovePropW(h1, k1) == v && GetPropW(h1, k1) == 0,
           "RemovePropW returns the value and the key goes absent");
    }

    /* ---- same-thread send, posted messages, quit ---- */
    {
        W32_MSG m;
        seen_n = 0;
        drain();
        W32_LRESULT r = SendMessageW(h1, 0x8001, 7, 9);
        ok(seen_n == 1 && seen_msg[0] == 0x8001 && r == 0,
           "SendMessageW on the owning thread dispatches directly");
        ok(PeekMessageW(&m, 0, 0, 0, 1) == W32_FALSE,
           "a send leaves nothing in the queue");
        PostMessageW(h1, 0x8002, 1, 2);
        ok(PeekMessageW(&m, 0, 0, 0, 0) == W32_TRUE && m.message == 0x8002 &&
           m.wParam == 1 && m.lParam == 2,
           "PeekMessageW(PM_NOREMOVE) looks without removing");
        ok(PeekMessageW(&m, 0, 0, 0, 1) == W32_TRUE && m.message == 0x8002,
           "PeekMessageW(PM_REMOVE) takes it");
        seen_n = 0;
        ok(DispatchMessageW(&m) == 0 && seen_n == 1,
           "DispatchMessageW calls the window procedure");
        /* A filter that does not match must not consume. */
        PostMessageW(h1, 0x8003, 0, 0);
        ok(PeekMessageW(&m, 0, 0x9000, 0x9FFF, 1) == W32_FALSE &&
           PeekMessageW(&m, 0, 0x8003, 0x8003, 1) == W32_TRUE,
           "the message filter is honoured");
        PostQuitMessage(3);
        ok(GetMessageW(&m, 0, 0, 0) == W32_FALSE && m.message == W32_WM_QUIT &&
           (int)m.wParam == 3,
           "PostQuitMessage ends the loop with its exit code");
        ok(GetMessageW(&m, 0, 0, 0) == W32_FALSE,
           "the quit stays latched, as Win32's does");
    }

    /* ---- paint and the update region ---- */
    {
        W32_PAINTSTRUCT ps;
        clears = fills = renders = 0;
        drain();
        ok(GetUpdateRgn(h1, 0, 0) == 0, "nothing is dirty before an invalidate");
        InvalidateRect(h1, 0, W32_TRUE);
        ok(GetUpdateRgn(h1, 0, 0) == 1, "InvalidateRect creates an update region");
        ok(fw[0].invalidated == 1, "the compositor was told to redraw");
        W32_HDC dc = BeginPaint(h1, &ps);
        ok(dc != 0 && ps.rcPaint.left == 0 && ps.rcPaint.top == 0 &&
           ps.rcPaint.right == 400 && ps.rcPaint.bottom == 300,
           "BeginPaint reports the whole client when the whole client is dirty");
        ok(ps.fErase == W32_TRUE && fills > 0,
           "BeginPaint erased with the class background and says so");
        EndPaint(h1, &ps);
        ok(GetUpdateRgn(h1, 0, 0) == 0 && renders > 0,
           "EndPaint validates the region and asks for a render");
        InvalidateRect(h1, &(W32_RECT){ 10, 10, 60, 60 }, W32_TRUE);
        BeginPaint(h1, &ps);
        ok(ps.rcPaint.left == 10 && ps.rcPaint.top == 10 &&
           ps.rcPaint.right == 60 && ps.rcPaint.bottom == 60,
           "a small invalidate paints a small rcPaint");
        EndPaint(h1, &ps);
        InvalidateRect(h1, &(W32_RECT){ 0, 0, 100, 100 }, W32_TRUE);
        ValidateRect(h1, &(W32_RECT){ 0, 0, 100, 50 });
        ok(GetUpdateRgn(h1, 0, 0) == 1,
           "ValidateRect subtracts one rectangle and leaves the rest");
        ValidateRect(h1, 0);
        ok(GetUpdateRgn(h1, 0, 0) == 0, "ValidateRect(NULL) validates it all");
        seen_n = 0;
        InvalidateRect(h1, 0, W32_TRUE);
        drain();
        InvalidateRect(h1, 0, W32_TRUE);
        UpdateWindow(h1);
        int saw_paint = 0;
        for (int i = 0; i < seen_n; i++)
            if (seen_msg[i] == W32_WM_PAINT) saw_paint = 1;
        ok(saw_paint, "UpdateWindow sends WM_PAINT inside the call");
        {
            W32_RECT rr = { 0, 0, 10, 10 };
            ok(RedrawWindow(h1, &rr, 0, W32_RDW_INVALIDATE | W32_RDW_UPDATENOW) ==
               W32_TRUE,
               "RedrawWindow invalidates and updates in one call");
            ok(LockWindowUpdate(h1) == W32_TRUE, "LockWindowUpdate takes the lock");
            ok(LockWindowUpdate(0) == W32_TRUE, "and releases it");
        }
    }

    /* ---- scroll state ---- */
    {
        W32_SCROLLINFO si;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = W32_SIF_ALL;
        si.nMin = 0; si.nMax = 200; si.nPage = 20; si.nPos = 40;
        ok(SetScrollInfo(h1, W32_SB_VERT, &si, W32_TRUE) == 0,
           "SetScrollInfo returns the previous position");
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = W32_SIF_ALL;
        ok(GetScrollInfo(h1, W32_SB_VERT, &si) == W32_TRUE && si.nMax == 200 &&
           si.nPage == 20 && si.nPos == 40,
           "GetScrollInfo round-trips range, page and position");
        ok(GetScrollPos(h1, W32_SB_VERT) == 40, "GetScrollPos");
        ok(SetScrollPos(h1, W32_SB_VERT, 500, W32_FALSE) == 40 &&
           GetScrollPos(h1, W32_SB_VERT) == 200,
           "SetScrollPos returns the old position and clamps to nMax");
        int32_t lo = -1, hi = -1;
        ok(GetScrollRange(h1, W32_SB_VERT, &lo, &hi) == W32_TRUE &&
           lo == 0 && hi == 200, "GetScrollRange");
        ok(ShowScrollBar(h1, W32_SB_BOTH, W32_TRUE) == W32_TRUE,
           "ShowScrollBar records the request");
        drain();
        ok(ScrollWindow(h1, 0, -20, 0, 0) == W32_TRUE &&
           GetUpdateRgn(h1, 0, 0) == 1,
           "ScrollWindow invalidates the bared strip");
        drain();
    }

    /* ---- focus, capture, Z-order ---- */
    {
        W32_HWND h2 = CreateWindowExW(0, CLS, TITLE, W32_WS_POPUP, 10, 10, 200, 150,
                                      0, 0, 0, 0);
        ok(h2 != 0, "a second window for the focus/capture checks");
        drain();
        SetFocus(h2);
        ok(GetFocus() == h2, "SetFocus reaches the compositor and GetFocus reads it");
        ok(GetForegroundWindow() == h2 && GetActiveWindow() == h2,
           "foreground/active follow the one focused window (documented)");
        ok(SetCapture(h1) == 0,
           "SetCapture returns the previous owner (there was none, so NULL)");
        ok(GetCapture() == h1, "GetCapture reads the capture owner back");
        ok(SetCapture(h1) == h1, "a second SetCapture returns the same window");
        ok(ReleaseCapture() == W32_TRUE && GetCapture() == 0,
           "ReleaseCapture clears it");
        drain();
        ok(SetWindowPos(h1, W32_HWND_BOTTOM, 0, 0, 0, 0,
                        W32_SWP_NOSIZE | W32_SWP_NOMOVE | W32_SWP_NOREDRAW) ==
           W32_TRUE && fw[0].z == -1,
           "SetWindowPos(HWND_BOTTOM) lowers through the compositor");
        ok(SetWindowPos(h1, W32_HWND_TOPMOST, 0, 0, 0, 0,
                        W32_SWP_NOSIZE | W32_SWP_NOMOVE | W32_SWP_NOREDRAW) ==
           W32_TRUE && (ag_window_get_flags(1) & 0x080u) != 0,
           "SetWindowPos(HWND_TOPMOST) sets the compositor's ALWAYS_TOP flag");
        ok(SetWindowPos(h1, W32_HWND_NOTOPMOST, 0, 0, 0, 0,
                        W32_SWP_NOSIZE | W32_SWP_NOMOVE | W32_SWP_NOREDRAW) ==
           W32_TRUE && (ag_window_get_flags(1) & 0x080u) == 0,
           "and NOTOPMOST clears it");
        ok(BringWindowToTop(h1) == W32_TRUE && GetFocus() == h1,
           "BringWindowToTop focuses the window");
        ok(GetWindow(h1, W32_GW_HWNDFIRST) == h2 ||
           GetWindow(h1, W32_GW_HWNDFIRST) == h1,
           "GetWindow(GW_HWNDFIRST) answers from the process's own list");
        ok(GetWindowThreadProcessId(h1, 0) == GetCurrentThreadId(),
           "GetWindowThreadProcessId is the creating thread");
        drain();
        DestroyWindow(h2);
    }

    /* ---- metrics and colours follow the theme ---- */
    {
        ok(GetSystemMetrics(W32_SM_CXSCREEN) == 1280 &&
           GetSystemMetrics(W32_SM_CYSCREEN) == 800,
           "SM_CXSCREEN/SM_CYSCREEN come from the framebuffer");
        ok(GetSystemMetrics(W32_SM_CYCAPTION) == 24,
           "SM_CYCAPTION is the theme's titlebar height");
        ok(GetSystemMetrics(W32_SM_CXFRAME) == 2, "SM_CXFRAME is the theme's border");
        ok(GetSystemMetrics(W32_SM_CXICON) == 48, "SM_CXICON is the theme's icon size");
        ok(GetSystemMetrics(W32_SM_CMONITORS) == 1, "one monitor, and it says one");
        ok(GetSystemMetrics(0x7FFE) == 0, "an unknown metric answers zero");
        uint32_t before = GetSysColor(W32_COLOR_WINDOW);
        theme.win_content = 0x00123456u;           /* the theme changes... */
        ok(GetSysColor(W32_COLOR_WINDOW) == w32_colorref_to_ag(0x00123456u) &&
           before != GetSysColor(W32_COLOR_WINDOW),
           "...and GetSysColor moves with it, so it is not a constant table");
        theme.win_content = 0x00FFFFFFu;
        ok(GetSysColor(W32_COLOR_DESKTOP) == w32_colorref_to_ag(theme.desktop_top),
           "COLOR_DESKTOP maps to the theme's desktop colour");
        ok(GetSysColorBrush(W32_COLOR_WINDOW) ==
           (W32_HBRUSH)(uintptr_t)GetSysColor(W32_COLOR_WINDOW),
           "GetSysColorBrush is the colour: brushes are colours here");
        int32_t beep = -1;
        ok(SystemParametersInfoW(W32_SPI_GETBEEP, 0, &beep, 0) == W32_TRUE &&
           beep == 1, "SPI_GETBEEP answers");
        W32_RECT work;
        ok(SystemParametersInfoW(W32_SPI_GETWORKAREA, 0, &work, 0) == W32_TRUE &&
           work.right == 1280 && work.bottom == 800 - 40,
           "SPI_GETWORKAREA honours the taskbar height");
        ok(SystemParametersInfoW(0x999u, 0, 0, 0) == W32_FALSE &&
           GetLastError() == W32_ERROR_INVALID_PARAMETER,
           "an unlisted SPI is refused by number, not guessed");
        ok(GetDoubleClickTime() == 500, "GetDoubleClickTime is the documented value");
    }

    /* ---- one monitor, described honestly ---- */
    {
        W32_MONITORINFOEXW mi;
        memset(&mi, 0, sizeof mi);
        W32_HMONITOR mon = MonitorFromWindow(h1, W32_MONITOR_DEFAULTTONEAREST);
        ok(mon != 0 && GetMonitorInfoW(mon, &mi) == W32_TRUE &&
           mi.mi.rcMonitor.right == 1280 && mi.mi.rcWork.bottom == 760,
           "GetMonitorInfoW describes the framebuffer minus the taskbar");
        ok(mi.mi.dwFlags == 1u, "the only monitor is the primary one");
        ok(EnumDisplayMonitors(0, 0, 0, 0) == W32_FALSE,
           "EnumDisplayMonitors refuses a null callback");
    }

    /* ---- input state ---- */
    {
        uint16_t ch = 0;
        uint8_t st[256];
        memset(st, 0, sizeof st);
        st[W32_VK_SHIFT] = 0x80;
        ok(ToAscii('A', 0, st, &ch, 0) == 1 && ch == 'A', "ToAscii honours shift");
        st[W32_VK_SHIFT] = 0;
        ok(ToAscii('A', 0, st, &ch, 0) == 1 && ch == 'a',
           "ToAscii lower-cases without it");
        st[W32_VK_CONTROL] = 0x80;
        ok(ToAscii('A', 0, st, &ch, 0) == 1 && ch == 0x01,
           "ToAscii maps Ctrl+A to 0x01");
        st[W32_VK_CONTROL] = 0;
        ok(ToAscii(W32_VK_RETURN, 0, st, &ch, 0) == 1 && ch == '\r',
           "ToAscii maps Enter");
        ok(MapVirtualKeyW('A', 0) == 0x1E && MapVirtualKeyW(0x1E, 1) == 'A',
           "MapVirtualKeyW round-trips a letter's scan code");
        drain();
        fake_event(1, 10 /* UI_EVT_KEY_DOWN */, 0, 0, 'A', 0, 0, 0x1E);
        W32_MSG m;
        ok(PeekMessageW(&m, 0, 0, 0, 1) == W32_TRUE &&
           m.message == W32_WM_KEYDOWN && (m.lParam & 0xFFFF) == 1,
           "a compositor key event becomes WM_KEYDOWN with its scan code");
        ok((GetKeyState('A') & 0x8000) != 0,
           "GetKeyState sees the delivered key down");
        ok(TranslateMessage(&m) == W32_TRUE &&
           PeekMessageW(&m, 0, 0, 0, 1) == W32_TRUE && m.message == W32_WM_CHAR &&
           m.wParam == 'a',
           "TranslateMessage synthesises WM_CHAR from the key state");
        fake_event(1, 11 /* UI_EVT_KEY_UP */, 0, 0, 'A', 0, 0, 0x1E);
        PeekMessageW(&m, 0, 0, 0, 1);
        ok((GetKeyState('A') & 0x8000) == 0, "and the key up clears it");
        W32_POINT pt;
        ok(GetCursorPos(&pt) == W32_TRUE && pt.x == 100 && pt.y == 100,
           "GetCursorPos asks the compositor");
        ok(SetCursorPos(0, 0) == W32_FALSE &&
           GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "SetCursorPos refuses: the compositor owns the pointer");
        drain();
    }

    /* ---- DefWindowProc's documented defaults ---- */
    {
        drain();
        W32_HWND h3 = CreateWindowExW(0, CLS, TITLE, W32_WS_CAPTION, 0, 0, 100, 100,
                                      0, 0, 0, 0);
        ok(h3 != 0 && GetWindowLongPtrW(h3, W32_GWL_USERDATA) == 0,
           "a fresh window's GWL_USERDATA is null");
        SetWindowLongPtrW(h3, W32_GWL_USERDATA, (intptr_t)&checks);
        ok(GetWindowLongPtrW(h3, W32_GWL_USERDATA) == (intptr_t)&checks &&
           GetWindowLongPtrW(h3, W32_GWL_STYLE) == (intptr_t)W32_WS_CAPTION,
           "Set/GetWindowLongPtrW round-trip userdata and style");
        /* WM_NCHITTEST defaults to HTCAPTION, so the compositor drags. */
        ok(SendMessageW(h3, W32_WM_NCHITTEST, 0, 0) == 1,
           "DefWindowProc answers WM_NCHITTEST with HTCAPTION");
        ok(SendMessageW(h3, W32_WM_ERASEBKGND, 0, 0) == W32_TRUE,
           "DefWindowProc's WM_ERASEBKGND says it erased");
        ok(DestroyWindow(h3) == W32_TRUE && IsWindow(h3) == W32_FALSE,
           "DestroyWindow reports success and the handle dies");
        W32_HWND h4 = CreateWindowExW(0, CLS, TITLE, W32_WS_CAPTION, 0, 0, 100, 100,
                                      0, 0, 0, 0);
        seen_n = 0;
        SendMessageW(h4, W32_WM_CLOSE, 0, 0);
        ok(IsWindow(h4) == W32_FALSE, "the default WM_CLOSE destroys the window");
        W32_HWND h5 = CreateWindowExW(0, CLS, TITLE, W32_WS_CAPTION, 0, 0, 100, 100,
                                      0, 0, 0, 0);
        SendMessageW(h5, W32_WM_SYSCOMMAND, 0xF060 /* SC_CLOSE */, 0);
        ok(IsWindow(h5) == W32_FALSE,
           "SC_CLOSE through DefWindowProc destroys it too");
        drain();
    }

    /* ---- the desktop, and the refusals ---- */
    {
        W32_HWND desk = GetDesktopWindow();
        W32_RECT r;
        ok(IsWindow(desk) == W32_TRUE && GetWindowRect(desk, &r) == W32_TRUE &&
           r.left == 0 && r.right == 1280 && r.bottom == 800,
           "GetDesktopWindow answers with the screen rect");
        ok(GetShellWindow() == 0 && GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "GetShellWindow refuses: this system has no shell window");
        ok(SetParent(h1, desk) == 0 &&
           GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "SetParent refuses: there are no child surfaces");
        ok(GetParent(h1) == 0 && IsChild(desk, h1) == W32_FALSE,
           "a top-level window has no parent");
        ok(UnregisterClassW(CLS, 0) == W32_FALSE &&
           GetLastError() == W32_ERROR_BUSY,
           "a class with live windows refuses to unregister");
    }

    /* ---- cross-thread messages, on real threads ---- */
    {
        pthread_mutex_init(&P.m, 0);
        pthread_cond_init(&P.c, 0);
        memset(&P, 0, sizeof P);
        pthread_mutex_init(&P.m, 0);
        pthread_cond_init(&P.c, 0);
        W32_WNDCLASSEXW cp = mkclass(CLS_P, wndproc_pump);
        ok(RegisterClassExW(&cp) != 0,
           "the pump thread's class is registered");

        pthread_t th;
        ok(pthread_create(&th, 0, pump_main, 0) == 0, "pump thread started");
        pthread_mutex_lock(&P.m);
        while (!P.ready) pthread_cond_wait(&P.c, &P.m);
        pthread_mutex_unlock(&P.m);
        ok(P.hwnd != 0 && P.tid != GetCurrentThreadId(),
           "the pump thread created the window it owns");

        /* The phase's headline: a send from this thread runs the window
         * procedure on the other one and blocks until it returns. */
        W32_LRESULT r = SendMessageW(P.hwnd, MSG_SEND_PROBE, 41, 0);
        ok(r == 42 && P.send_result == 42 && P.proc_tid == P.tid,
           "cross-thread SendMessageW runs the procedure on the owner thread "
           "and returns its result");

        drain();
        ok(PeekMessageW(&(W32_MSG){ 0 }, 0, 0, 0, 1) == W32_FALSE,
           "the owner's queue is not this thread's queue");

        PostMessageW(P.hwnd, MSG_POST_PROBE, 77, 0);
        for (int i = 0; i < 3000 && P.posted_seen != 77; i++) Sleep(1);
        ok(P.posted_seen == 77, "the owner thread's loop received the posted message");

        PostMessageW(P.hwnd, MSG_POST_EXTRA, 0, 0);
        for (int i = 0; i < 3000 && P.extra_seen == 0; i++) Sleep(1);
        ok(P.extra_seen == 1, "and every posted message is delivered");

        PostMessageW(P.hwnd, MSG_PUMP_QUIT, 0, 0);
        pthread_join(th, 0);
        ok(1, "the pump thread exited on its quit message");
        pthread_mutex_destroy(&P.m);
        pthread_cond_destroy(&P.c);
    }

    printf("\n== A5: %d checks, %d failures ==\n", checks, failures);
    return failures ? 1 : 0;
}
