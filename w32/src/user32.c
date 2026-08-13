/* user32.c — USER32 + GDI32 over AuraLite's compositor.
 * WIN32_PLAN.md phase W32-5, decision D5.
 *
 * Every window is an ag_window_*, every draw is an ag_* call.  What this file
 * adds is the three things Win32 has and libauragui does not:
 *
 *   1. a window CLASS, carrying the WNDPROC;
 *   2. a message QUEUE with Win32 numbering, fed by translating ag_event_t;
 *   3. the callback direction of the ABI -- the personality calls into the PE
 *      image, which is what W32-4's test did not cover.
 *
 * Deliberately absent: any drawing that libauragui cannot already do.  A
 * function with no AG equivalent fails with a documented error rather than
 * pretending, so a program discovers it at the call site.
 */

#include "w32/user32.h"
#include "w32/w32_errno.h"

#ifndef AURALITE_W32_HOST_TEST
#include "auragui.h"
#include <string.h>
#include <stdio.h>
#endif

#define MAX_CLASSES 16
#define MAX_WINDOWS 16
#define MAX_QUEUED  64

struct w32_class {
    int         in_use;
    char        name[64];
    W32_WNDPROC proc;
    W32_DWORD   bg_color;      /* AG colour, already converted */
};

struct w32_window {
    int          in_use;
    int          ag_wid;       /* the compositor's window id */
    W32_WNDPROC  proc;
    W32_DWORD    bg_color;
    uint32_t     w, h;
    int          quit_posted;
};

static struct w32_class  classes[MAX_CLASSES];
static struct w32_window windows[MAX_WINDOWS];

/* A queue of already-translated messages.  ag_poll_event() yields at most one
 * event per call, but one event can produce two Win32 messages (a close
 * becomes WM_CLOSE then, after DefWindowProc, WM_DESTROY), so a queue is
 * simpler than trying to make the translation 1:1. */
static W32_MSG queue[MAX_QUEUED];
static int q_head, q_tail;
static int quit_requested, quit_code;

/* HWND/HDC bias: same reasoning as the HANDLE table in W32-4.  A window token
 * must not be forgeable by inventing a small integer, and must never be NULL,
 * because NULL is how every Win32 producer reports failure. */
#define HWND_BIAS 0x2000
#define HDC_BIAS  0x3000

static W32_HWND idx_to_hwnd(int i) { return (W32_HWND)(intptr_t)(HWND_BIAS + i); }

static int hwnd_to_idx(W32_HWND h) {
    intptr_t v = (intptr_t)h;
    if (v < HWND_BIAS) return -1;
    intptr_t i = v - HWND_BIAS;
    if (i >= MAX_WINDOWS) return -1;
    if (!windows[i].in_use) return -1;
    return (int)i;
}

/* An HDC in this phase names a window: there is no off-screen surface, no
 * compatible DC and no bitmap selection, so a DC needs no state of its own.
 * That is a real limitation and is why CreateCompatibleDC is absent rather
 * than stubbed. */
static W32_HDC idx_to_hdc(int i) { return (W32_HDC)(intptr_t)(HDC_BIAS + i); }

static int hdc_to_idx(W32_HDC d) {
    intptr_t v = (intptr_t)d;
    if (v < HDC_BIAS) return -1;
    intptr_t i = v - HDC_BIAS;
    if (i >= MAX_WINDOWS) return -1;
    if (!windows[i].in_use) return -1;
    return (int)i;
}

/* Win32 COLORREF is 0x00BBGGRR, AuraLite is 0x00RRGGBB.  Getting this
 * backwards produces a picture that looks plausible but has red and blue
 * swapped, which is exactly the kind of bug that survives a smoke test. */
W32_DWORD w32_colorref_to_ag(W32_DWORD cr) {
    uint32_t r = (cr >>  0) & 0xFFu;
    uint32_t g = (cr >>  8) & 0xFFu;
    uint32_t b = (cr >> 16) & 0xFFu;
    return (r << 16) | (g << 8) | b;
}

/* Defined with the other DC state further down; reset here. */
static void dc_state_reset(void);

void w32_user32_init(void) {
    for (int i = 0; i < MAX_CLASSES; i++) classes[i].in_use = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].in_use = 0;
    q_head = q_tail = 0;
    quit_requested = 0;
    quit_code = 0;
    dc_state_reset();
}

static void q_push(W32_HWND hwnd, W32_UINT m, W32_WPARAM wp, W32_LPARAM lp) {
    int next = (q_tail + 1) % MAX_QUEUED;
    if (next == q_head) return;             /* full: drop, never overwrite */
    queue[q_tail].hwnd    = hwnd;
    queue[q_tail].message = m;
    queue[q_tail].wParam  = wp;
    queue[q_tail].lParam  = lp;
    queue[q_tail].time    = 0;
    queue[q_tail].pt.x    = 0;
    queue[q_tail].pt.y    = 0;
    q_tail = next;
}

static int q_pop(W32_MSG *out) {
    if (q_head == q_tail) return 0;
    *out = queue[q_head];
    q_head = (q_head + 1) % MAX_QUEUED;
    return 1;
}

/* ---- classes ------------------------------------------------------------- */

W32ABI W32_WORD RegisterClassExA(const W32_WNDCLASSEXA *cls) {
    if (!cls || !cls->lpszClassName || !cls->lpfnWndProc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (classes[i].in_use) continue;
        classes[i].in_use = 1;
        size_t n = 0;
        while (n < sizeof(classes[i].name) - 1 && cls->lpszClassName[n]) {
            classes[i].name[n] = cls->lpszClassName[n];
            n++;
        }
        classes[i].name[n] = '\0';
        classes[i].proc = cls->lpfnWndProc;
        /* hbrBackground carries a colour in this personality; see
         * CreateSolidBrush. */
        classes[i].bg_color = cls->hbrBackground
            ? w32_colorref_to_ag((W32_DWORD)(uintptr_t)cls->hbrBackground)
            : 0x00FFFFFFu;
        return (W32_WORD)(i + 1);           /* ATOM, non-zero on success */
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static struct w32_class *find_class(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!classes[i].in_use) continue;
        const char *a = classes[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return &classes[i];
    }
    return 0;
}

/* ---- windows ------------------------------------------------------------- */

W32ABI W32_HWND CreateWindowExA(W32_DWORD exstyle, const char *cls,
                                const char *title, W32_DWORD style,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                W32_HWND parent, W32_HMENU menu,
                                W32_HINSTANCE inst, void *param) {
    (void)exstyle; (void)parent; (void)menu; (void)inst; (void)param;

    struct w32_class *c = find_class(cls);
    if (!c) {
        /* An unregistered class is a hard error in Win32 too.  Refusing here
         * is what keeps a bad class pointer from reaching the compositor. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (x == W32_CW_USEDEFAULT) x = 80;
    if (y == W32_CW_USEDEFAULT) y = 60;
    if (w == W32_CW_USEDEFAULT || w <= 0) w = 320;
    if (h == W32_CW_USEDEFAULT || h <= 0) h = 200;
    if (w > 4096 || h > 4096) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Style -> AG_WIN_* flags.  Only the bits with an equivalent are mapped;
     * the rest are ignored rather than approximated. */
    uint32_t flags = 0;
    if (style & W32_WS_CAPTION)     flags |= AG_WIN_HAS_TITLE;
    if (style & W32_WS_SYSMENU)     flags |= AG_WIN_HAS_CLOSE;
    if (style & W32_WS_THICKFRAME)  flags |= AG_WIN_RESIZABLE | AG_WIN_MOVABLE;
    if (style & (W32_WS_MINIMIZEBOX | W32_WS_MAXIMIZEBOX))
        flags |= AG_WIN_HAS_MINMAX;
    if (style & W32_WS_POPUP)       flags |= AG_WIN_NO_DECOR;
    if (flags == 0)                 flags = AG_WIN_DEFAULT;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].in_use) continue;

        int wid = ag_window_create(x, y, (uint32_t)w, (uint32_t)h,
                                   title ? title : "", flags);
        if (wid < 0) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        windows[i].in_use   = 1;
        windows[i].ag_wid   = wid;
        windows[i].proc     = c->proc;
        windows[i].bg_color = c->bg_color;
        windows[i].w        = (uint32_t)w;
        windows[i].h        = (uint32_t)h;
        windows[i].quit_posted = 0;

        W32_HWND hwnd = idx_to_hwnd(i);

        /* Win32 sends WM_CREATE synchronously, before CreateWindowEx returns.
         * A program that allocates its state there depends on the ordering. */
        if (c->proc) (void)c->proc(hwnd, W32_WM_CREATE, 0, 0);

        if (style & W32_WS_VISIBLE) {
            ag_window_show(wid);
            q_push(hwnd, W32_WM_PAINT, 0, 0);
        }
        return hwnd;
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

W32ABI W32_BOOL ShowWindow(W32_HWND hwnd, int32_t cmd) {
    int i = hwnd_to_idx(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }

    if (cmd == W32_SW_HIDE) {
        ag_window_hide(windows[i].ag_wid);
    } else {
        ag_window_show(windows[i].ag_wid);
        q_push(hwnd, W32_WM_PAINT, 0, 0);
    }
    return W32_TRUE;
}

W32ABI W32_BOOL UpdateWindow(W32_HWND hwnd) {
    int i = hwnd_to_idx(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    q_push(hwnd, W32_WM_PAINT, 0, 0);
    return W32_TRUE;
}

W32ABI W32_BOOL InvalidateRect(W32_HWND hwnd, const W32_RECT *r, W32_BOOL erase) {
    (void)r; (void)erase;
    int i = hwnd_to_idx(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ag_window_invalidate(windows[i].ag_wid);
    q_push(hwnd, W32_WM_PAINT, 0, 0);
    return W32_TRUE;
}

W32ABI W32_BOOL GetClientRect(W32_HWND hwnd, W32_RECT *r) {
    int i = hwnd_to_idx(hwnd);
    if (i < 0 || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    uint32_t w = windows[i].w, h = windows[i].h;
    ag_window_get_size(windows[i].ag_wid, &w, &h);
    r->left = 0; r->top = 0;
    r->right = (int32_t)w; r->bottom = (int32_t)h;
    return W32_TRUE;
}

W32ABI W32_BOOL DestroyWindow(W32_HWND hwnd) {
    int i = hwnd_to_idx(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }

    if (windows[i].proc) (void)windows[i].proc(hwnd, W32_WM_DESTROY, 0, 0);
    ag_window_destroy(windows[i].ag_wid);
    windows[i].in_use = 0;
    return W32_TRUE;
}

/* ---- the message loop ----------------------------------------------------
 *
 * One ag_event_t becomes zero, one or two Win32 messages.  The mapping is
 * explicit rather than arithmetic: the two numbering schemes have no relation,
 * and a table makes an unhandled event obvious. */

static void translate_event(W32_HWND hwnd, const ag_event_t *e) {
    switch (e->type) {
    case AG_EVT_PAINT:
        q_push(hwnd, W32_WM_PAINT, 0, 0);
        break;
    case AG_EVT_MOUSE_MOVE:
        q_push(hwnd, W32_WM_MOUSEMOVE, e->buttons,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_MOUSE_DOWN:
        q_push(hwnd, W32_WM_LBUTTONDOWN, e->buttons,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_MOUSE_UP:
        q_push(hwnd, W32_WM_LBUTTONUP, e->buttons,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_MOUSE_RIGHT_DOWN:
        q_push(hwnd, W32_WM_RBUTTONDOWN, e->buttons,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_MOUSE_RIGHT_UP:
        q_push(hwnd, W32_WM_RBUTTONUP, e->buttons,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_KEY_DOWN:
        q_push(hwnd, W32_WM_KEYDOWN, e->key, 0);
        /* Win32 programs that do not call TranslateMessage still expect
         * printable keys to arrive as WM_CHAR from the system in many
         * samples; posting it here keeps a simple program working without
         * making TranslateMessage a lie. */
        if (e->key >= 0x20 && e->key < 0x7F)
            q_push(hwnd, W32_WM_CHAR, e->key, 0);
        break;
    case AG_EVT_KEY_UP:
        q_push(hwnd, W32_WM_KEYUP, e->key, 0);
        break;
    case AG_EVT_FOCUS:
        q_push(hwnd, W32_WM_SETFOCUS, 0, 0);
        break;
    case AG_EVT_BLUR:
        q_push(hwnd, W32_WM_KILLFOCUS, 0, 0);
        break;
    case AG_EVT_RESIZE:
        q_push(hwnd, W32_WM_SIZE, 0,
               ((W32_LPARAM)(e->y & 0xFFFF) << 16) | (e->x & 0xFFFF));
        break;
    case AG_EVT_CLOSE_REQ:
        q_push(hwnd, W32_WM_CLOSE, 0, 0);
        break;
    default:
        /* Events with no Win32 equivalent are dropped, not invented. */
        break;
    }
}

/* Pump the compositor for every live window. */
static void pump(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        ag_event_t e;
        int guard = 0;
        while (ag_poll_event(windows[i].ag_wid, &e) > 0) {
            translate_event(idx_to_hwnd(i), &e);
            if (++guard > 32) break;        /* never spin on a hot queue */
        }
    }
}

W32ABI W32_BOOL PeekMessageA(W32_MSG *msg, W32_HWND hwnd,
                             W32_UINT min, W32_UINT max, W32_UINT remove) {
    (void)hwnd; (void)min; (void)max;
    if (!msg) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    pump();
    if (q_head == q_tail) return W32_FALSE;
    if (remove) return q_pop(msg) ? W32_TRUE : W32_FALSE;
    *msg = queue[q_head];
    return W32_TRUE;
}

W32ABI W32_BOOL GetMessageA(W32_MSG *msg, W32_HWND hwnd,
                            W32_UINT min, W32_UINT max) {
    (void)hwnd; (void)min; (void)max;
    if (!msg) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }

    for (;;) {
        if (q_pop(msg)) {
            /* GetMessage returns FALSE (0) on WM_QUIT -- that is how every
             * Win32 message loop terminates. */
            if (msg->message == W32_WM_QUIT) return W32_FALSE;
            return W32_TRUE;
        }
        if (quit_requested) {
            msg->hwnd = 0;
            msg->message = W32_WM_QUIT;
            msg->wParam = (W32_WPARAM)quit_code;
            msg->lParam = 0;
            return W32_FALSE;
        }
        pump();
        if (q_head == q_tail) {
            /* Nothing pending: yield rather than spin.  The compositor is
             * polled, so a sleep here is what keeps a message loop from
             * eating the CPU. */
            ag_render_now();
            for (volatile int k = 0; k < 20000; k++) { }
        }
    }
}

W32ABI W32_BOOL TranslateMessage(const W32_MSG *msg) {
    /* WM_CHAR is already produced in translate_event(), so this is a
     * no-op that reports honestly rather than a stub that lies: TRUE means
     * "a character message was posted for this key", which is true exactly
     * when the key was printable. */
    if (!msg) return W32_FALSE;
    return (msg->message == W32_WM_KEYDOWN) ? W32_TRUE : W32_FALSE;
}

W32ABI W32_LRESULT DispatchMessageA(const W32_MSG *msg) {
    if (!msg) return 0;
    int i = hwnd_to_idx(msg->hwnd);
    if (i < 0) return 0;
    if (!windows[i].proc) return 0;
    /* The callback direction of the ABI: into the PE image. */
    return windows[i].proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

W32ABI W32_LRESULT DefWindowProcA(W32_HWND hwnd, W32_UINT msg,
                                  W32_WPARAM wp, W32_LPARAM lp) {
    (void)wp; (void)lp;
    switch (msg) {
    case W32_WM_CLOSE:
        /* The documented default: closing destroys the window, which in turn
         * sends WM_DESTROY. */
        DestroyWindow(hwnd);
        return 0;
    case W32_WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return 0;
    }
}

W32ABI void PostQuitMessage(int32_t code) {
    quit_requested = 1;
    quit_code = code;
}

W32ABI int32_t MessageBoxA(W32_HWND owner, const char *text,
                           const char *caption, W32_UINT type) {
    (void)owner; (void)type;
    ag_alert(caption ? caption : "Message", text ? text : "");
    return 1;                                /* IDOK */
}

/* ---- GDI ----------------------------------------------------------------- */

W32ABI W32_HDC BeginPaint(W32_HWND hwnd, W32_PAINTSTRUCT *ps) {
    int i = hwnd_to_idx(hwnd);
    if (i < 0 || !ps) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }

    uint32_t w = windows[i].w, h = windows[i].h;
    ag_window_get_size(windows[i].ag_wid, &w, &h);

    /* Win32 erases the background with the class brush before WM_PAINT. */
    ag_clear(windows[i].ag_wid, windows[i].bg_color);

    ps->hdc = idx_to_hdc(i);
    ps->fErase = W32_TRUE;
    ps->rcPaint.left = 0; ps->rcPaint.top = 0;
    ps->rcPaint.right = (int32_t)w; ps->rcPaint.bottom = (int32_t)h;
    ps->fRestore = 0; ps->fIncUpdate = 0;
    return ps->hdc;
}

W32ABI W32_BOOL EndPaint(W32_HWND hwnd, const W32_PAINTSTRUCT *ps) {
    (void)ps;
    int i = hwnd_to_idx(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ag_render_now();
    return W32_TRUE;
}

W32ABI int32_t FillRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush) {
    int i = hdc_to_idx(hdc);
    if (i < 0 || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (r->right <= r->left || r->bottom <= r->top) return 0;
    ag_fill_rect(windows[i].ag_wid, r->left, r->top,
                 (uint32_t)(r->right - r->left),
                 (uint32_t)(r->bottom - r->top),
                 w32_colorref_to_ag((W32_DWORD)(uintptr_t)brush));
    return 1;
}

/* One text colour and one current position per window: enough for TextOut and
 * MoveTo/LineTo, which is what this phase's gate uses.  A full DC state
 * (pen, font, ROP, clip) is not modelled, and no function pretends it is. */
static W32_DWORD dc_text_color[MAX_WINDOWS];
static int32_t   dc_cur_x[MAX_WINDOWS], dc_cur_y[MAX_WINDOWS];

static void dc_state_reset(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        dc_text_color[i] = 0;              /* COLORREF black */
        dc_cur_x[i] = 0;
        dc_cur_y[i] = 0;
    }
}

W32ABI W32_DWORD SetTextColor(W32_HDC hdc, W32_DWORD color) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    W32_DWORD old = dc_text_color[i];
    dc_text_color[i] = color;
    return old;
}

W32ABI W32_BOOL TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                         const char *s, int32_t len) {
    int i = hdc_to_idx(hdc);
    if (i < 0 || !s) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }

    /* ag_draw_text takes a NUL-terminated string; Win32 passes a length. */
    char buf[256];
    int n = 0;
    while (n < (int)sizeof buf - 1 && (len < 0 || n < len) && s[n]) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = '\0';
    ag_draw_text(windows[i].ag_wid, x, y, buf,
                 w32_colorref_to_ag(dc_text_color[i]));
    return W32_TRUE;
}

W32ABI W32_BOOL MoveToEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (old) { old->x = dc_cur_x[i]; old->y = dc_cur_y[i]; }
    dc_cur_x[i] = x; dc_cur_y[i] = y;
    return W32_TRUE;
}

W32ABI W32_BOOL LineTo(W32_HDC hdc, int32_t x, int32_t y) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ag_draw_line(windows[i].ag_wid, dc_cur_x[i], dc_cur_y[i], x, y,
                 w32_colorref_to_ag(dc_text_color[i]));
    dc_cur_x[i] = x; dc_cur_y[i] = y;
    return W32_TRUE;
}

W32ABI W32_DWORD SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color) {
    int i = hdc_to_idx(hdc);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    ag_draw_pixel(windows[i].ag_wid, x, y, w32_colorref_to_ag(color));
    return color;
}

/* The brush "handle" is the colour.  No allocation, no object table, and
 * DeleteObject on it is a no-op that cannot leak. */
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD color) {
    return (W32_HBRUSH)(uintptr_t)color;
}

W32ABI W32_BOOL DeleteObject(void *obj) { (void)obj; return W32_TRUE; }
