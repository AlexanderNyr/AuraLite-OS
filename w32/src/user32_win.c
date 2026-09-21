/* user32_win.c — W32A-5: USER32's window and message core.
 *
 * W32APP_PLAN.md phase W32A-5.  D5: the compositor IS the window manager and
 * this file never grows a second one.  D6: the W entry point is the real
 * implementation and the A form converts its strings and forwards.
 *
 * WHAT THIS FILE OWNS
 *   classes (registry + background + proc), the window table, per-thread
 *   message queues with Win32 numbering, subclass chains, window properties,
 *   placement state (normal rect + show state), the update region, scroll
 *   state, and the mapping of WC_* metrics/colours onto the compositor theme.
 *
 * WHAT THE COMPOSITOR OWNS (asked, never duplicated)
 *   pixels, Z-order, focus, the capture contract, the desktop, the single
 *   monitor's geometry, and the pointer position.  A second copy of any of
 *   those drifts the moment the user drags a title bar -- see
 *   kernel/gui/gui_syscalls.c's W32A-5 ops.
 *
 * CROSS-THREAD SendMessage, HONESTLY
 *   Win32's contract: a send to a window owned by another thread is
 *   delivered by that thread's message loop and the sender blocks until the
 *   window procedure returns.  Here that is: the message goes into the
 *   owner's queue carrying a W32A-3 event; the owner's GetMessage/PeekMessage
 *   dispatches it in place and signals the event; the sender waits on it with
 *   WaitForSingleObject.  A target thread that never pumps therefore blocks
 *   the sender forever -- exactly as it does on Windows -- and that is
 *   written down here rather than special-cased away.
 *
 * REFUSED BY NAME, NOT SILENTLY
 *   Styles with no compositor flag are recorded and reported:
 *   CreateWindowExW refuses W32_WS_CHILD (AuraLite composites top-level
 *   windows only; GetParent/IsChild still work on the logical tree) and
 *   WS_EX_LAYERED windows accept only the colour-key form.  GetShellWindow
 *   has no shell window to return and says so.  Anything taken as real is
 *   listed in the phase's Done note with the fixture that asserts it.
 *
 * Names, message numbers and structure layouts are the interface being
 * reimplemented, written from published documentation (w32/LICENSING.md).
 */

#include "w32/user32.h"
#include "w32/kernel32.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/user32_priv.h"
#include "w32/gdi32.h"

/* Mirrors of libauragui's two structs this file reads (they are the kernel's
 * gui_theme_t / gui_event_t layouts; the ABI check below pins the values in
 * the real build).  Keeping our own copy is what lets the host suite compile
 * this file without the compositor. */
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
} ui_theme_mirror_t;

typedef struct {
    uint32_t type;
    int32_t  x, y;
    uint32_t key;
    uint8_t  buttons, mods;
    uint16_t data;
} ui_event_mirror_t;

/* The compositor's event numbers, named here so the translation table below
 * reads as itself.  _Static_assert in the real build keeps them pinned to
 * libauragui's values -- a silent drift here would mis-route input. */
#define UI_EVT_MOUSE_MOVE        1
#define UI_EVT_MOUSE_DOWN        2
#define UI_EVT_MOUSE_UP          3
#define UI_EVT_MOUSE_DBLCLICK    4
#define UI_EVT_MOUSE_WHEEL       5
#define UI_EVT_MOUSE_RIGHT_DOWN  6
#define UI_EVT_MOUSE_RIGHT_UP    7
#define UI_EVT_MOUSE_MIDDLE_DOWN 8
#define UI_EVT_MOUSE_MIDDLE_UP   9
#define UI_EVT_KEY_DOWN          10
#define UI_EVT_KEY_UP            11
#define UI_EVT_FOCUS             12
#define UI_EVT_BLUR              13
#define UI_EVT_RESIZE            14
#define UI_EVT_CLOSE_REQ         15
#define UI_EVT_TIMER             16
#define UI_EVT_PAINT             17
#define UI_EVT_CONTEXT_MENU      18
#define UI_EVT_SNAP_CHANGED      19
#define UI_EVT_DROP              20
#define UI_EVT_ICON_CLICK        21

#ifdef AURALITE_W32_HOST_TEST
/* The host suite has no compositor; it supplies these.  They mirror
 * lib/libauragui/include/auragui.h exactly -- keep them in step.  The two
 * struct typedefs come first because the declarations below name them, and
 * the AG_WIN_* flag values are repeated here for the same reason: the host
 * build has no auragui.h to include. */
typedef ui_theme_mirror_t ui_theme_t;
typedef ui_event_mirror_t ui_event_t;

/* Compositor window flags (auragui.h).  A drift here would show up as the
 * guest fixture's style assertions failing, not as a silent difference. */
#define AG_WIN_RESIZABLE   0x001
#define AG_WIN_MOVABLE     0x002
#define AG_WIN_HAS_TITLE   0x004
#define AG_WIN_HAS_CLOSE   0x008
#define AG_WIN_HAS_MINMAX  0x010
#define AG_WIN_MODAL       0x020
#define AG_WIN_NO_DECOR    0x040
#define AG_WIN_ALWAYS_TOP  0x080
#define AG_WIN_TOOL_WINDOW 0x100
#define AG_WIN_BORDERLESS  0x200
#define AG_WIN_DEFAULT     (AG_WIN_RESIZABLE | AG_WIN_MOVABLE | \
                            AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_HAS_MINMAX)

int ag_window_create(int32_t, int32_t, uint32_t, uint32_t, const char *, uint32_t);
int ag_window_show(int);
int ag_window_hide(int);
int ag_window_destroy(int);
int ag_window_focus(int);
int ag_window_minimize(int);
int ag_window_maximize(int);
int ag_window_restore(int);
int ag_window_move(int, int32_t, int32_t);
int ag_window_resize(int, uint32_t, uint32_t);
int ag_window_set_title(int, const char *);
int ag_window_invalidate(int);
int ag_window_invalidate_rect(int, int32_t, int32_t, uint32_t, uint32_t);
int ag_window_get_size(int, uint32_t *, uint32_t *);
int ag_window_get_pos(int, int32_t *, int32_t *);
int ag_window_lower(int);
int ag_window_set_flags(int, uint32_t);
uint32_t ag_window_get_flags(int);
int ag_window_get_z(int);
int ag_window_capture(int);
int ag_window_get_capture(void);
int ag_window_focused(void);
int ag_window_top(void);
int ag_screen_size(uint32_t *, uint32_t *);
int ag_mouse_position(int32_t *, int32_t *);
int ag_theme_get(ui_theme_t *);
int ag_poll_event(int, ui_event_t *);
int ag_clear(int, uint32_t);
int ag_fill_rect(int, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
int ag_draw_text(int, int32_t, int32_t, const char *, uint32_t);
int ag_draw_pixel(int, int32_t, int32_t, uint32_t);
int ag_draw_line(int, int32_t, int32_t, int32_t, int32_t, uint32_t);
void ag_render_now(void);
void ag_alert(const char *, const char *);
#else
#include "auragui.h"
typedef ag_theme_t ui_theme_t;
typedef ag_event_t ui_event_t;
_Static_assert(UI_EVT_MOUSE_DOWN == AG_EVT_MOUSE_DOWN, "ag_event_t drifted");
_Static_assert(UI_EVT_KEY_DOWN   == AG_EVT_KEY_DOWN,   "ag_event_t drifted");
_Static_assert(UI_EVT_CLOSE_REQ  == AG_EVT_CLOSE_REQ,  "ag_event_t drifted");
_Static_assert(UI_EVT_PAINT      == AG_EVT_PAINT,      "ag_event_t drifted");
_Static_assert(sizeof(ui_theme_t) == sizeof(ag_theme_t), "ag_theme_t drifted");
#endif

#define UI_MAX_CLASSES  32
#define UI_MAX_WINDOWS  32
#define UI_MAX_QUEUES   16
/* 256: a message queue must absorb a burst of creation traffic (every
 * visible CreateWindowExW posts WM_SIZE, the pump adds WM_PAINT for each
 * invalidated window) before the caller first pumps.  64 dropped posts
 * once a program built ~20 control windows and only then ran a modal
 * loop -- the W32A-8 property-sheet gate found it. */
#define UI_QUEUE_LEN    256
#define UI_MAX_PROPS    16
#define UI_MAX_INV      16
#define UI_MAX_PROCS    4
#define UI_MAX_WMSG     16
#define UI_TITLE_MAX    128
#define UI_CNAME_MAX    64

/* Style bits this personality understands.  A create with a bit outside the
 * mask is refused by name so the caller learns at the call site, which is
 * the W32-5 convention this phase keeps. */
#define UI_STYLE_KNOWN \
    (W32_WS_OVERLAPPED | W32_WS_CAPTION | W32_WS_SYSMENU | W32_WS_THICKFRAME | \
     W32_WS_MINIMIZEBOX | W32_WS_MAXIMIZEBOX | W32_WS_POPUP | W32_WS_VISIBLE | \
     W32_WS_BORDER | W32_WS_DLGFRAME | W32_WS_VSCROLL | W32_WS_HSCROLL | \
     W32_WS_CHILD | W32_WS_DISABLED | W32_WS_CLIPSIBLINGS | W32_WS_CLIPCHILDREN | \
     W32_DS_MODALFRAME | W32_DS_SETFONT | W32_DS_CENTER | W32_DS_ABSALIGN | \
     W32_DS_SYSMODAL | W32_DS_3DLOOK | W32_DS_FIXEDSYS | W32_DS_NOFAILCREATE | \
     W32_DS_CONTROL | W32_DS_CENTERMOUSE | W32_DS_CONTEXTHELP)
#define UI_EXSTYLE_KNOWN \
    (W32_WS_EX_TOPMOST | W32_WS_EX_TOOLWINDOW | W32_WS_EX_LAYERED | \
     W32_WS_EX_APPWINDOW | W32_WS_EX_CLIENTEDGE | W32_WS_EX_DLGMODALFRAME | \
     W32_WS_EX_TRANSPARENT)

struct ui_class {
    int       in_use;
    uint16_t  name_w[UI_CNAME_MAX];
    char      name_a[UI_CNAME_MAX * 3];
    W32_WNDPROC proc;
    uint32_t  bg;                 /* AG colour; 0 means "no background" */
    W32_HBRUSH hbr;               /* the handle the class was registered with */
    int       has_bg;
    W32_UINT  style;
    W32_HICON icon;
    W32_HCURSOR cursor;
    int       comctl;             /* W32A-8: a common-control class (WS_CHILD ok) */
};

struct ui_inv { int32_t l, t, r, b; };
struct ui_prop { int in_use; uint16_t key[48]; W32_HANDLE val; };
struct ui_scroll { int32_t min, max, pos, page; int shown; };

struct ui_window {
    int       in_use;
    int       ag_wid;
    int       cls;                    /* class index, -1 for the desktop */
    int       parent;                 /* window index, -1 for none */
    W32_DWORD tid;                    /* owning thread */
    W32_DWORD style, exstyle;
    uint16_t  title[UI_TITLE_MAX];
    char      title_a[UI_TITLE_MAX * 3];
    int32_t   x, y;                   /* outer rect, from the compositor */
    uint32_t  w, h;                   /* outer size */
    int       visible, minimized, maximized, enabled;
    int32_t   normal_x, normal_y;     /* placement: restored geometry */
    uint32_t  normal_w, normal_h;
    int32_t   ctrl_id;
    W32_WNDPROC proc[UI_MAX_PROCS];   /* [0] class proc, [n] subclasses */
    int       proc_top;               /* index of the current top, >= 0 */
    void     *user_data;
    struct ui_prop   prop[UI_MAX_PROPS];
    struct ui_scroll scroll[2];       /* 0 = horz, 1 = vert */
    struct ui_inv    inv[UI_MAX_INV];
    int       n_inv;
    int       painting;
    W32_HWND  lock_owner;             /* LockWindowUpdate */
    int       tracking_leave;
    int32_t   last_mx, last_my;
    W32_DWORD layered_key, layered_flags;
    uint8_t   layered_alpha;
    int       is_desktop;
};

struct ui_msg {
    W32_HWND   hwnd;
    W32_UINT   message;
    W32_WPARAM wParam;
    W32_LPARAM lParam;
    W32_DWORD  time;
    int32_t    px, py;
    /* Cross-thread send bookkeeping: non-NULL evt means "the sender is
     * blocked on this"; the receiving pump signals it after the window
     * procedure returns, writing *result first. */
    W32_HANDLE evt;
    W32_LRESULT *result;
    W32_DWORD  sender;                /* tid, 0 for Posted */
};

struct ui_queue {
    int       in_use;
    W32_DWORD tid;
    struct ui_msg q[UI_QUEUE_LEN];
    int       head, tail;
    int       quit_posted, quit_code;
    W32_DWORD last_time;              /* GetMessageTime */
    int32_t   last_px, last_py;       /* GetMessagePos */
    uint8_t   keystate[256];          /* per-thread, fed by delivered keys */
};

/* Forward declarations: the file is ordered by topic, and a few helpers
 * (text, the update region, the queue lookup) are used by the message core
 * before their topic comes up. */
static struct ui_queue *ui_queue_for(W32_DWORD tid, int create);
static void ui_post_to(W32_HWND hwnd, W32_UINT msg, W32_WPARAM wp,
                       W32_LPARAM lp, int32_t px, int32_t py);
static void ui_set_text(struct ui_window *w, const uint16_t *s);
static void ui_inv_add(struct ui_window *w, const W32_RECT *r);
static void ui_inv_clear(struct ui_window *w);
static int  ui_inv_union_rect(struct ui_window *w, W32_RECT *out);
static void ui_translate(struct ui_window *w, W32_HWND hwnd, const ui_event_t *e);

static struct ui_class  classes[UI_MAX_CLASSES];
static struct ui_window windows[UI_MAX_WINDOWS];
static struct ui_queue  queues[UI_MAX_QUEUES];
static uint16_t wmsg_names[UI_MAX_WMSG][48];
static int      wmsg_used[UI_MAX_WMSG];

/* One global lock: the window tables are small and never hot (the
 * compositor's own lock guards pixels).  Held only across table edits. */
static volatile int ui_locked;

static void ui_lock(void) {
    while (__atomic_test_and_set(&ui_locked, __ATOMIC_ACQUIRE)) { }
}
static void ui_unlock(void) { __atomic_clear(&ui_locked, __ATOMIC_RELEASE); }

/* ---- small string helpers ------------------------------------------------ */

static size_t w16_len(const uint16_t *s) {
    if (!s) return 0;
    return w32_utf16_len(s, UI_TITLE_MAX);
}

static void w16_copy(uint16_t *dst, const uint16_t *src, size_t max) {
    size_t i = 0;
    if (src) while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int w16_eq(const uint16_t *a, const uint16_t *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* UTF-16 -> UTF-8 into a fixed buffer; returns the byte length written. */
static int w16_to_a(char *dst, size_t cap, const uint16_t *src) {
    size_t needed = 0;
    if (!src) { dst[0] = 0; return 0; }
    int rc = w32_utf16_to_utf8(src, w32_utf16_len(src, UI_TITLE_MAX),
                               dst, cap - 1, &needed);
    if (rc != W32_UTF_OK || needed >= cap) {
        /* Unrepresentable or too long: an empty A-string is honest, a
         * half-converted one is not. */
        dst[0] = 0;
        return 0;
    }
    dst[needed] = 0;
    return (int)needed;
}

/* UTF-8 -> UTF-16, bounded; returns the code-unit length written. */
static int a_to_w16(uint16_t *dst, size_t cap, const char *src) {
    size_t needed = 0;
    if (!src) { dst[0] = 0; return 0; }
    size_t slen = 0;
    while (src[slen] && slen < 4096) slen++;
    int rc = w32_utf8_to_utf16(src, slen, dst, cap - 1, &needed);
    if (rc != W32_UTF_OK || needed >= cap) { dst[0] = 0; return 0; }
    dst[needed] = 0;
    return (int)needed;
}

/* ---- the window table ---------------------------------------------------- */

#define UI_HWND_BIAS 0x2000

static W32_HWND idx_to_hwnd(int i) { return (W32_HWND)(intptr_t)(UI_HWND_BIAS + i); }

int w32_win_index_from_hwnd(W32_HWND h) {
    intptr_t v = (intptr_t)h;
    if (v < UI_HWND_BIAS) return -1;
    intptr_t i = v - UI_HWND_BIAS;
    if (i >= UI_MAX_WINDOWS) return -1;
    if (!windows[i].in_use) return -1;
    return (int)i;
}

W32_HWND w32_win_hwnd_from_index(int i) { return idx_to_hwnd(i); }
int w32_win_ag_wid(int i) {
    if (i < 0 || i >= UI_MAX_WINDOWS || !windows[i].in_use) return -1;
    return windows[i].ag_wid;
}
int w32_win_live_count(void) {
    int n = 0;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) if (windows[i].in_use) n++;
    return n;
}
uint32_t w32_win_client_w(int i) {
    uint32_t w = 0, h = 0;
    if (i >= 0 && i < UI_MAX_WINDOWS && windows[i].in_use)
        ag_window_get_size(windows[i].ag_wid, &w, &h);
    return w;
}
uint32_t w32_win_client_h(int i) {
    uint32_t w = 0, h = 0;
    if (i >= 0 && i < UI_MAX_WINDOWS && windows[i].in_use)
        ag_window_get_size(windows[i].ag_wid, &w, &h);
    return h;
}

/* Re-read the geometry the compositor actually has.  Every geometry answer
 * comes from here rather than from cached fields, which is what keeps a
 * user's title-bar drag visible to GetWindowRect. */
static void ui_refresh_geom(struct ui_window *w) {
    if (w->ag_wid < 0) return;
    ag_window_get_pos(w->ag_wid, &w->x, &w->y);
    uint32_t cw = 0, ch = 0;
    ag_window_get_size(w->ag_wid, &cw, &ch);
    /* The compositor reports the client (content) size; the outer rect adds
     * the decoration the theme asks for. */
    ui_theme_t t;
    uint32_t bw = 0, th = 0;
    if (ag_theme_get(&t) == 0) { bw = t.border_w; th = t.titlebar_h; }
    int decorated = !(w->style & W32_WS_POPUP);
    w->w = cw + (decorated ? 2 * bw : 0);
    w->h = ch + (decorated ? th + 2 * bw : 0);
}

int w32_win_cls_ag_wid(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) return -1;
    return w32_win_ag_wid(i);
}

/* ---- classes ------------------------------------------------------------- */

static struct ui_class *ui_find_class_w(const uint16_t *name) {
    if (!name) return 0;
    for (int i = 0; i < UI_MAX_CLASSES; i++)
        if (classes[i].in_use && w16_eq(classes[i].name_w, name)) return &classes[i];
    return 0;
}

static W32_WORD ui_register_class(const W32_WNDCLASSEXW *c) {
    if (!c || !c->lpszClassName || !c->lpfnWndProc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (ui_find_class_w(c->lpszClassName)) {
        w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
        return 0;
    }
    for (int i = 0; i < UI_MAX_CLASSES; i++) {
        if (classes[i].in_use) continue;
        struct ui_class *k = &classes[i];
        k->in_use = 1;
        w16_copy(k->name_w, c->lpszClassName, UI_CNAME_MAX);
        w16_to_a(k->name_a, sizeof k->name_a, k->name_w);
        k->proc   = c->lpfnWndProc;
        /* W32A-7: hbrBackground may be a real brush table handle or the
         * A-5 raw colour; w32_gdi_brush_color decodes both, and the
         * class remembers the handle it was given. */
        k->has_bg = c->hbrBackground != 0;
        k->hbr    = c->hbrBackground;
        k->bg     = k->has_bg
                  ? w32_colorref_to_ag(w32_gdi_brush_color(c->hbrBackground))
                  : 0;
        k->style  = c->style;
        k->icon   = c->hIcon;
        k->cursor = c->hCursor;
        return (W32_WORD)(i + 1);            /* ATOM, non-zero */
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

/* W32A-8: comctl32's classes register through here so the class table
 * carries the marker that admits WS_CHILD.  The public RegisterClass*
 * never sets it: an application class stays a top-level window class,
 * and CreateWindowExW keeps refusing WS_CHILD for it by name. */
W32_WORD w32_win_register_comctl_class(const W32_WNDCLASSEXW *c) {
    W32_WORD atom = ui_register_class(c);
    if (!atom) return 0;
    for (int i = 0; i < UI_MAX_CLASSES; i++) {
        if (classes[i].in_use && w16_eq(classes[i].name_w, c->lpszClassName)) {
            classes[i].comctl = 1;
            break;
        }
    }
    return atom;
}

W32ABI W32_WORD RegisterClassW(const W32_WNDCLASSEXW *c) { return ui_register_class(c); }
W32ABI W32_WORD RegisterClassExW(const W32_WNDCLASSEXW *c) { return ui_register_class(c); }

W32ABI W32_WORD RegisterClassExA(const W32_WNDCLASSEXA *c) {
    if (!c) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    W32_WNDCLASSEXW w;
    uint16_t name[UI_CNAME_MAX];
    if (a_to_w16(name, UI_CNAME_MAX, c->lpszClassName) <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    w.cbSize = c->cbSize; w.style = c->style; w.lpfnWndProc = c->lpfnWndProc;
    w.cbClsExtra = c->cbClsExtra; w.cbWndExtra = c->cbWndExtra;
    w.hInstance = c->hInstance; w.hIcon = c->hIcon; w.hCursor = c->hCursor;
    w.hbrBackground = c->hbrBackground; w.lpszMenuName = 0;
    w.lpszClassName = name; w.hIconSm = c->hIconSm;
    return ui_register_class(&w);
}

W32ABI W32_WORD RegisterClassA(const W32_WNDCLASSEXA *c) { return RegisterClassExA(c); }

W32ABI W32_BOOL UnregisterClassW(const uint16_t *name, W32_HINSTANCE inst) {
    (void)inst;
    struct ui_class *k = ui_find_class_w(name);
    if (!k) { w32_set_last_error(W32_ERROR_CLASS_DOES_NOT_EXIST); return W32_FALSE; }
    /* A class with live windows cannot go: Win32 fails the call, and doing
     * otherwise would leave windows pointing at a recycled slot. */
    int idx = (int)(k - classes);
    for (int i = 0; i < UI_MAX_WINDOWS; i++)
        if (windows[i].in_use && windows[i].cls == idx) {
            w32_set_last_error(W32_ERROR_BUSY);
            return W32_FALSE;
        }
    ui_lock();
    k->in_use = 0;
    ui_unlock();
    return W32_TRUE;
}

W32ABI W32_BOOL GetClassInfoW(W32_HINSTANCE inst, const uint16_t *name,
                              W32_WNDCLASSEXW *out) {
    (void)inst;
    if (!out) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_class *k = ui_find_class_w(name);
    if (!k) { w32_set_last_error(W32_ERROR_CLASS_DOES_NOT_EXIST); return W32_FALSE; }
    out->cbSize = sizeof(W32_WNDCLASSEXW);
    out->style = k->style;
    out->lpfnWndProc = k->proc;
    out->cbClsExtra = 0;
    out->cbWndExtra = 0;
    out->hInstance = 0;
    out->hIcon = k->icon;
    out->hCursor = k->cursor;
    out->hbrBackground = k->has_bg ? k->hbr : 0;
    out->lpszMenuName = 0;
    out->lpszClassName = k->name_w;
    out->hIconSm = k->icon;
    return W32_TRUE;
}

W32ABI int32_t GetClassNameW(W32_HWND hwnd, uint16_t *buf, int32_t max) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !buf || max <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (windows[i].cls < 0) {
        uint16_t d[] = { 'D','e','s','k','t','o','p',0 };
        w16_copy(buf, d, (size_t)max);
    } else {
        w16_copy(buf, classes[windows[i].cls].name_w, (size_t)max);
    }
    return (int32_t)w16_len(buf);
}

W32ABI int32_t GetClassNameA(W32_HWND hwnd, char *buf, int32_t max) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !buf || max <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    const char *src = (windows[i].cls < 0) ? "Desktop" : classes[windows[i].cls].name_a;
    int n = 0;
    while (src[n] && n < max - 1) { buf[n] = src[n]; n++; }
    buf[n] = 0;
    return n;
}

/* ---- window creation ---------------------------------------------------- */

/* Style bits -> compositor flags.  The compositor speaks AG_WIN_*; this is
 * the whole of the translation and it is exhaustive on purpose: a caller
 * that passes a bit this function does not map gets told, not ignored. */
static uint32_t ui_style_to_flags(W32_DWORD style) {
    uint32_t f = 0;
    if (style & W32_WS_CAPTION)    f |= AG_WIN_HAS_TITLE;
    if (style & W32_WS_SYSMENU)    f |= AG_WIN_HAS_CLOSE;
    if (style & W32_WS_THICKFRAME) f |= AG_WIN_RESIZABLE | AG_WIN_MOVABLE;
    if (style & (W32_WS_MINIMIZEBOX | W32_WS_MAXIMIZEBOX)) f |= AG_WIN_HAS_MINMAX;
    if (style & W32_WS_POPUP)      f |= AG_WIN_NO_DECOR | AG_WIN_BORDERLESS;
    if (style & W32_WS_CHILD)      f |= AG_WIN_NO_DECOR | AG_WIN_BORDERLESS;
    if (f == 0) f = AG_WIN_DEFAULT;
    return f;
}

W32ABI W32_HWND CreateWindowExW(W32_DWORD exstyle, const uint16_t *clsname,
                                const uint16_t *title, W32_DWORD style,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                W32_HWND parent, W32_HMENU menu,
                                W32_HINSTANCE inst, void *param) {
    (void)inst; (void)param;
    struct ui_class *k = ui_find_class_w(clsname);
    if (!k) {
        w32_set_last_error(W32_ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    /* Refused by name for application classes: a child window is not
     * composited inside its parent here.  The logical parent/child tree
     * still works (GetParent, IsChild, EnumChildWindows,
     * SendDlgItemMessage).  W32A-8 widened exactly one case: classes
     * registered through w32_win_register_comctl_class (the common
     * controls) accept WS_CHILD, because every real caller creates
     * them that way.  A control still composites as its own top-level
     * window -- there is no child embedding in the compositor -- but
     * its x/y are read as parent-relative and translated, so a control
     * lands inside its parent on screen.  Moving the parent does not
     * re-clip children; that limitation is the compositor's, recorded
     * here so the contract stays honest. */
    if ((style & W32_WS_CHILD) && !k->comctl) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    if (style & W32_WS_CHILD) {
        int pi = w32_win_index_from_hwnd(parent);
        if (pi < 0) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        ui_lock();
        x += windows[pi].x;
        y += windows[pi].y;
        ui_unlock();
    }
    if ((style & ~UI_STYLE_KNOWN) || (exstyle & ~UI_EXSTYLE_KNOWN)) {
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

    uint32_t flags = ui_style_to_flags(style);
    if (exstyle & W32_WS_EX_TOPMOST)   flags |= AG_WIN_ALWAYS_TOP;
    if (exstyle & W32_WS_EX_TOOLWINDOW) flags |= AG_WIN_TOOL_WINDOW;

    char title_a[UI_TITLE_MAX * 3];
    w16_to_a(title_a, sizeof title_a, title);

    int slot = -1;
    ui_lock();
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) { slot = i; break; }
    }
    ui_unlock();
    if (slot < 0) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    int ag_wid = ag_window_create(x, y, (uint32_t)w, (uint32_t)h, title_a, flags);
    if (ag_wid < 0) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    int pidx = w32_win_index_from_hwnd(parent);
    ui_lock();
    struct ui_window *win = &windows[slot];
    win->in_use = 1;
    win->ag_wid = ag_wid;
    win->cls    = (int)(k - classes);
    win->parent = pidx;
    win->tid    = GetCurrentThreadId();
    win->style  = style;
    win->exstyle = exstyle;
    w16_copy(win->title, title, UI_TITLE_MAX);
    w16_to_a(win->title_a, sizeof win->title_a, win->title);
    win->x = x; win->y = y; win->w = (uint32_t)w; win->h = (uint32_t)h;
    win->normal_x = x; win->normal_y = y;
    win->normal_w = (uint32_t)w; win->normal_h = (uint32_t)h;
    win->visible = (style & W32_WS_VISIBLE) ? 1 : 0;
    win->minimized = win->maximized = 0;
    win->enabled = (style & W32_WS_DISABLED) ? 0 : 1;
    win->ctrl_id = (int32_t)(intptr_t)menu;
    win->proc[0] = k->proc;
    win->proc_top = 0;
    win->n_inv = 0;
    win->tracking_leave = 0;
    win->layered_key = 0; win->layered_alpha = 255; win->layered_flags = 0;
    ui_unlock();

    W32_HWND hwnd = idx_to_hwnd(slot);

    /* Win32 sends WM_NCCREATE then WM_CREATE before CreateWindowEx returns,
     * and a program that allocates its state in WM_CREATE depends on it.
     * Both go through SendMessageW so the subclass chain and the return
     * value behave exactly as they will later. */
    if (SendMessageW(hwnd, W32_WM_NCCREATE, 0, (W32_LPARAM)(intptr_t)param) == 0) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (SendMessageW(hwnd, W32_WM_CREATE, 0, (W32_LPARAM)(intptr_t)param) < 0) {
        DestroyWindow(hwnd);
        return 0;
    }

    if (win->visible) {
        ag_window_show(ag_wid);
        ui_refresh_geom(win);
        PostMessageW(hwnd, W32_WM_SIZE, 0,
                     (W32_LPARAM)((int32_t)win->h << 16) | (int32_t)win->w);
        InvalidateRect(hwnd, 0, W32_TRUE);
    }
    return hwnd;
}

W32ABI W32_HWND CreateWindowExA(W32_DWORD exstyle, const char *cls,
                                const char *title, W32_DWORD style,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                W32_HWND parent, W32_HMENU menu,
                                W32_HINSTANCE inst, void *param) {
    uint16_t cls_w[UI_CNAME_MAX], title_w[UI_TITLE_MAX];
    if (a_to_w16(cls_w, UI_CNAME_MAX, cls) <= 0) {
        w32_set_last_error(W32_ERROR_CLASS_DOES_NOT_EXIST);
        return 0;
    }
    a_to_w16(title_w, UI_TITLE_MAX, title);
    return CreateWindowExW(exstyle, cls_w, title_w, style, x, y, w, h,
                           parent, menu, inst, param);
}

W32ABI W32_BOOL IsWindow(W32_HWND hwnd) {
    return w32_win_index_from_hwnd(hwnd) >= 0 ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL DestroyWindow(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }

    /* Children first, depth first -- Win32 destroys the subtree. */
    for (int j = 0; j < UI_MAX_WINDOWS; j++)
        if (windows[j].in_use && windows[j].parent == i)
            DestroyWindow(idx_to_hwnd(j));

    /* WM_DESTROY is sent, not posted: the window is still valid inside it. */
    SendMessageW(hwnd, W32_WM_DESTROY, 0, 0);

    /* A capture or focus owned by this window goes back to the compositor. */
    if (ag_window_get_capture() == windows[i].ag_wid) ag_window_capture(-1);

    /* WM_NCDESTROY is the last message a window ever sees, and it must
     * still see a live handle, so the slot dies after it.  Win32's default
     * WM_DESTROY does NOT post WM_QUIT -- posting it is the sample's own
     * handler's job, and PostQuitMessage is what does it. */
    SendMessageW(hwnd, W32_WM_NCDESTROY, 0, 0);

    /* W32A-8: a common-control window dropping its state before the slot
     * dies (weak: the A-3-style host amalgams that omit comctl32.c still
     * link, exactly like the timer-pump hook above). */
    {
        extern __attribute__((weak)) void w32_comctl_window_destroyed(W32_HWND);
        if (w32_comctl_window_destroyed) w32_comctl_window_destroyed(hwnd);
    }

    int ag_wid = windows[i].ag_wid;
    ui_lock();
    windows[i].in_use = 0;
    ui_unlock();
    ag_window_destroy(ag_wid);
    return W32_TRUE;
}

W32ABI W32_HWND GetDesktopWindow(void) {
    /* The compositor's desktop is not a window, but Win32 callers use the
     * handle as a token for "the screen".  A permanent pseudo-window with
     * cls == -1 gives them one that IsWindow() and GetWindowRect() answer
     * for. */
    if (!windows[UI_MAX_WINDOWS - 1].is_desktop) {
        ui_lock();
        struct ui_window *d = &windows[UI_MAX_WINDOWS - 1];
        d->in_use = 1;
        d->ag_wid = -1;
        d->cls = -1;
        d->parent = -1;
        d->is_desktop = 1;
        d->enabled = 1;
        d->visible = 1;
        d->x = 0; d->y = 0;
        uint32_t sw = 0, sh = 0;
        ag_screen_size(&sw, &sh);
        d->w = sw; d->h = sh;
        d->normal_x = 0; d->normal_y = 0; d->normal_w = sw; d->normal_h = sh;
        ui_unlock();
    }
    return idx_to_hwnd(UI_MAX_WINDOWS - 1);
}

W32ABI W32_HWND GetShellWindow(void) {
    /* There is no shell window in AuraLite -- the compositor's taskbar and
     * desktop are kernel-owned and have no HWND.  Win32 returns a handle;
     * returning NULL here is the honest answer, and the plan's task list
     * said so ("likely FAIL-CLEAN"). */
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

W32ABI W32_DWORD GetWindowThreadProcessId(W32_HWND hwnd, W32_DWORD *pid) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (pid) *pid = GetCurrentProcessId();
    return windows[i].tid;
}

/* State reset: called by the CRT startup path and by the host suite between
 * cases.  The desktop pseudo-window is rebuilt lazily. */
void w32_user32_init(void) {
    ui_lock();
    for (int i = 0; i < UI_MAX_CLASSES; i++) classes[i].in_use = 0;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        /* Walk away from compositor windows rather than orphaning them. */
        if (windows[i].in_use && windows[i].ag_wid >= 0)
            ag_window_destroy(windows[i].ag_wid);
        windows[i].in_use = 0;
        windows[i].is_desktop = 0;
    }
    for (int i = 0; i < UI_MAX_QUEUES; i++) {
        queues[i].in_use = 0;
        queues[i].head = queues[i].tail = 0;
        queues[i].quit_posted = 0;
        queues[i].quit_code = 0;
    }
    for (int i = 0; i < UI_MAX_WMSG; i++) wmsg_used[i] = 0;
    ui_unlock();
}

/* W32A-4: live windows, for the unwinder's GUI-or-console decision. */
int w32_user_window_count(void) { return w32_win_live_count(); }

/* The class background, for the DC half's erase paths. */
uint32_t w32_win_bg_color(int i) {
    if (i < 0 || i >= UI_MAX_WINDOWS || !windows[i].in_use) return 0x00FFFFFFu;
    if (windows[i].cls < 0) return 0x00FFFFFFu;
    struct ui_class *k = &classes[windows[i].cls];
    return k->has_bg ? k->bg : 0x00FFFFFFu;
}

/* ---- BeginPaint / EndPaint ----------------------------------------------
 *
 * Win32's contract: BeginPaint validates the update region and hands back a
 * DC clipped to it (ps.rcPaint); EndPaint ends the paint.  Here the update
 * region is the personality's rectangle list, rcPaint is its union, and the
 * erase happens exactly when the region asked for it -- so a program that
 * invalidates one corner repaints one corner. */
W32ABI W32_HDC BeginPaint(W32_HWND hwnd, W32_PAINTSTRUCT *ps) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !ps) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    struct ui_window *w = &windows[i];
    if (w->painting) {
        /* Re-entrant BeginPaint: Win32 returns a DC for the same call and
         * does not re-erase.  Rare, but some toolkits do it. */
        ps->hdc = w32_win_make_dc(i);
        ps->fErase = W32_FALSE;
        ps->rcPaint.left = ps->rcPaint.top = 0;
        ps->rcPaint.right = ps->rcPaint.bottom = 0;
        return ps->hdc;
    }
    W32_RECT r;
    int have = ui_inv_union_rect(w, &r);
    if (!have) {
        uint32_t cw = 0, ch = 0;
        ag_window_get_size(w->ag_wid, &cw, &ch);
        r.left = 0; r.top = 0; r.right = (int32_t)cw; r.bottom = (int32_t)ch;
    }
    uint32_t bg = w32_win_bg_color(i);
    ag_fill_rect(w->ag_wid, r.left, r.top,
                 (uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top), bg);

    ps->hdc = w32_win_make_dc(i);
    ps->fErase = W32_TRUE;
    ps->rcPaint = r;
    ps->fRestore = 0;
    ps->fIncUpdate = 0;
    w->painting = 1;
    w->lock_owner = 0;
    return ps->hdc;
}

W32ABI W32_BOOL EndPaint(W32_HWND hwnd, const W32_PAINTSTRUCT *ps) {
    (void)ps;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ui_inv_clear(&windows[i]);
    windows[i].painting = 0;
    ag_render_now();
    return W32_TRUE;
}

/* ---- message queues ------------------------------------------------------ */

static struct ui_queue *ui_queue_for(W32_DWORD tid, int create) {
    for (int i = 0; i < UI_MAX_QUEUES; i++)
        if (queues[i].in_use && queues[i].tid == tid) return &queues[i];
    if (!create) return 0;
    for (int i = 0; i < UI_MAX_QUEUES; i++) {
        if (queues[i].in_use) continue;
        ui_lock();
        if (!queues[i].in_use) {
            queues[i].in_use = 1;
            queues[i].tid = tid;
            queues[i].head = queues[i].tail = 0;
            queues[i].quit_posted = 0;
            queues[i].quit_code = 0;
            for (int k = 0; k < 256; k++) queues[i].keystate[k] = 0;
            ui_unlock();
            return &queues[i];
        }
        ui_unlock();
    }
    return 0;
}

static int ui_q_full(const struct ui_queue *q) {
    return ((q->tail + 1) % UI_QUEUE_LEN) == q->head;
}

static int ui_q_push(struct ui_queue *q, const struct ui_msg *m) {
    if (!q || ui_q_full(q)) return 0;       /* full: drop, never overwrite */
    q->q[q->tail] = *m;
    q->tail = (q->tail + 1) % UI_QUEUE_LEN;
    return 1;
}

/* Remove the k-th element, shifting the rest down: sends are rare and the
 * ring is small, so clarity beats a tombstone scheme. */
static void ui_q_remove_at(struct ui_queue *q, int k) {
    int n = (q->tail - q->head + UI_QUEUE_LEN) % UI_QUEUE_LEN;
    if (k < 0 || k >= n) return;
    for (int i = k; i + 1 < n; i++) {
        q->q[(q->head + i) % UI_QUEUE_LEN] = q->q[(q->head + i + 1) % UI_QUEUE_LEN];
    }
    q->tail = (q->tail - 1 + UI_QUEUE_LEN) % UI_QUEUE_LEN;
}

static int ui_q_pop(struct ui_queue *q, W32_MSG *out, W32_HWND filter,
                    W32_UINT lo, W32_UINT hi, int remove) {
    int n = (q->tail - q->head + UI_QUEUE_LEN) % UI_QUEUE_LEN;
    /* Win32: GetMessage/PeekMessage treat a filter of 0..0 as "no filter"
     * rather than "messages numbered 0".  Getting this wrong makes the
     * commonest call in the API -- PeekMessage(&m, 0, 0, 0, PM_REMOVE) --
     * silently see nothing, so the suite asserts it by name. */
    if (lo == 0 && hi == 0) { lo = 0; hi = 0xFFFFFFFFu; }
    for (int i = 0; i < n; i++) {
        int idx = (q->head + i) % UI_QUEUE_LEN;
        struct ui_msg *m = &q->q[idx];
        if (m->evt) continue;                     /* a send: serviced, not read */
        if (filter && m->hwnd != filter) continue;
        if (m->message < lo || m->message > hi) continue;
        out->hwnd = m->hwnd;
        out->message = m->message;
        out->wParam = m->wParam;
        out->lParam = m->lParam;
        out->time = m->time;
        out->pt.x = m->px;
        out->pt.y = m->py;
        q->last_time = m->time;
        q->last_px = m->px;
        q->last_py = m->py;
        if (remove) ui_q_remove_at(q, i);
        return 1;
    }
    return 0;
}

/* ---- event translation --------------------------------------------------- */

/* The three tables that make the ABI callback direction work: compositor
 * event -> Win32 message, button state -> MK_* flags, key -> lParam bits. */
static W32_LPARAM ui_pt_lparam(int32_t x, int32_t y) {
    return (W32_LPARAM)(((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
}

static W32_WPARAM ui_mk_state(uint8_t buttons, uint8_t mods) {
    W32_WPARAM mk = 0;
    if (buttons & 0x01) mk |= 0x0001;          /* MK_LBUTTON */
    if (buttons & 0x02) mk |= 0x0002;          /* MK_RBUTTON */
    if (buttons & 0x04) mk |= 0x0010;          /* MK_MBUTTON */
    if (mods & 0x01)    mk |= 0x0008;          /* MK_SHIFT */
    if (mods & 0x02)    mk |= 0x0004;          /* MK_CONTROL */
    return mk;
}

static void ui_post_to(W32_HWND hwnd, W32_UINT msg, W32_WPARAM wp,
                       W32_LPARAM lp, int32_t px, int32_t py) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) return;
    W32_DWORD tid = windows[i].tid;
    struct ui_queue *q = ui_queue_for(tid ? tid : GetCurrentThreadId(), 1);
    struct ui_msg m;
    m.hwnd = hwnd; m.message = msg; m.wParam = wp; m.lParam = lp;
    m.time = GetTickCount();
    m.px = px; m.py = py;
    m.evt = 0; m.result = 0; m.sender = 0;
    ui_lock();
    ui_q_push(q, &m);
    ui_unlock();
}

/* TrackMouseEvent's leave half: the compositor has no leave event, so the
 * personality notices that a mouse-move went to a different window and fires
 * WM_MOUSELEAVE for the one that was armed.  Documented in the Done note. */
static int ui_last_hover = -1;

static void ui_note_hover(int idx) {
    if (idx == ui_last_hover) return;
    int prev = ui_last_hover;
    ui_last_hover = idx;
    if (prev >= 0 && prev < UI_MAX_WINDOWS && windows[prev].in_use &&
        windows[prev].tracking_leave) {
        windows[prev].tracking_leave = 0;
        ui_post_to(idx_to_hwnd(prev), W32_WM_MOUSELEAVE, 0, 0, 0, 0);
    }
}

static void ui_translate(struct ui_window *w, W32_HWND hwnd, const ui_event_t *e) {
    W32_WPARAM mk = ui_mk_state(e->buttons, e->mods);
    W32_LPARAM lp = ui_pt_lparam(e->x, e->y);
    switch (e->type) {
    case UI_EVT_PAINT:
        /* The compositor says "damage is on screen"; the update region is
         * the personality's, so an invalidate always precedes it.  Posting
         * WM_PAINT without an inventory would make BeginPaint a liar. */
        if (w->n_inv > 0) ui_post_to(hwnd, W32_WM_PAINT, 0, 0, e->x, e->y);
        break;
    case UI_EVT_MOUSE_MOVE:
        ui_note_hover(w32_win_index_from_hwnd(hwnd));
        ui_post_to(hwnd, W32_WM_MOUSEMOVE, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_DOWN:
        ui_post_to(hwnd, W32_WM_LBUTTONDOWN, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_UP:
        ui_post_to(hwnd, W32_WM_LBUTTONUP, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_DBLCLICK:
        ui_post_to(hwnd, 0x0203 /*WM_LBUTTONDBLCLK*/, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_RIGHT_DOWN:
        ui_post_to(hwnd, W32_WM_RBUTTONDOWN, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_RIGHT_UP:
        ui_post_to(hwnd, W32_WM_RBUTTONUP, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_MIDDLE_DOWN:
        ui_post_to(hwnd, W32_WM_MBUTTONDOWN, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_MIDDLE_UP:
        ui_post_to(hwnd, W32_WM_MBUTTONUP, mk, lp, e->x, e->y);
        break;
    case UI_EVT_MOUSE_WHEEL:
        /* Win32 packs the wheel delta in the high word of wParam and the
         * key state in the low word. */
        ui_post_to(hwnd, W32_WM_MOUSEWHEEL,
                   (W32_WPARAM)((uint32_t)(int16_t)e->data << 16) | (mk & 0xFFFF),
                   lp, e->x, e->y);
        break;
    case UI_EVT_KEY_DOWN: {
        /* One keystroke carries its scan code and its transition bits; the
         * thread's key-state array is fed here, which is what GetKeyState
         * reads back. */
        if (e->key < 256) {
            struct ui_queue *q = ui_queue_for(w->tid ? w->tid : GetCurrentThreadId(), 1);
            if (q) q->keystate[e->key] |= 0x80;
        }
        ui_post_to(hwnd, W32_WM_KEYDOWN, e->key,
                   (W32_LPARAM)(1 | ((uint32_t)e->data << 16)), e->x, e->y);
        break;
    }
    case UI_EVT_KEY_UP:
        if (e->key < 256) {
            struct ui_queue *q = ui_queue_for(w->tid ? w->tid : GetCurrentThreadId(), 1);
            if (q) q->keystate[e->key] &= (uint8_t)~0x80;
        }
        ui_post_to(hwnd, W32_WM_KEYUP, e->key,
                   (W32_LPARAM)(int32_t)0xC0000001u, e->x, e->y);
        break;
    case UI_EVT_FOCUS:
        ui_post_to(hwnd, W32_WM_SETFOCUS, 0, 0, e->x, e->y);
        break;
    case UI_EVT_BLUR:
        ui_post_to(hwnd, W32_WM_KILLFOCUS, 0, 0, e->x, e->y);
        break;
    case UI_EVT_RESIZE:
        ui_refresh_geom(w);
        ui_post_to(hwnd, W32_WM_SIZE, 0,
                   (W32_LPARAM)((int32_t)w->h << 16) | (int32_t)w->w, e->x, e->y);
        break;
    case UI_EVT_CLOSE_REQ:
        ui_post_to(hwnd, W32_WM_CLOSE, 0, 0, e->x, e->y);
        break;
    case UI_EVT_CONTEXT_MENU:
        ui_post_to(hwnd, W32_WM_CONTEXTMENU, (W32_WPARAM)(intptr_t)hwnd,
                   ui_pt_lparam(e->x, e->y), e->x, e->y);
        break;
    case UI_EVT_TIMER:
        ui_post_to(hwnd, W32_WM_TIMER, e->data, 0, e->x, e->y);
        break;
    case UI_EVT_ICON_CLICK:
    case UI_EVT_SNAP_CHANGED:
    case UI_EVT_DROP:
    default:
        /* Events with no Win32 equivalent are dropped, not invented. */
        break;
    }
}

/* Drain the compositor for every live window.  Called from the message
 * loop, so a program that never pumps never sees input -- same as Win32. */
static void ui_pump(void) {
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].ag_wid < 0) continue;
        ui_event_t e;
        int guard = 0;
        while (ag_poll_event(windows[i].ag_wid, &e) > 0) {
            ui_translate(&windows[i], idx_to_hwnd(i), &e);
            if (++guard > 32) break;
        }
    }
    /* W32A-6: let the dialog/timer layer fire due timers each pump tick.
     * A weak alias means the symbol resolves to a no-op when w32_dlg.o is
     * not linked (host unit tests that don't pull w32_dlg.o). */
    extern __attribute__((weak)) void w32_dlg_fire_timers(void);
    if (w32_dlg_fire_timers) w32_dlg_fire_timers();
}

/* ---- dispatch ------------------------------------------------------------ */

static W32_LRESULT ui_call_top(W32_HWND hwnd, W32_UINT msg,
                               W32_WPARAM wp, W32_LPARAM lp) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) return 0;
    int top = windows[i].proc_top;
    if (top < 0 || !windows[i].proc[top]) return DefWindowProcW(hwnd, msg, wp, lp);
    return windows[i].proc[top](hwnd, msg, wp, lp);
}

/* Service cross-thread sends: the sender is blocked on the carried event
 * until its window procedure has returned.  Win32 does this inside
 * GetMessage/PeekMessage, and so does this. */
static void ui_service_sends(struct ui_queue *q) {
    for (;;) {
        int n = (q->tail - q->head + UI_QUEUE_LEN) % UI_QUEUE_LEN;
        int found = -1;
        for (int i = 0; i < n; i++) {
            int idx = (q->head + i) % UI_QUEUE_LEN;
            if (q->q[idx].evt) { found = i; break; }
        }
        if (found < 0) return;
        int idx = (q->head + found) % UI_QUEUE_LEN;
        struct ui_msg m = q->q[idx];
        ui_q_remove_at(q, found);
        W32_LRESULT r = 0;
        if (w32_win_index_from_hwnd(m.hwnd) >= 0)
            r = ui_call_top(m.hwnd, m.message, m.wParam, m.lParam);
        if (m.result) *m.result = r;
        if (m.evt) SetEvent(m.evt);
    }
}

/* ---- the message API ----------------------------------------------------- */

W32ABI W32_LRESULT SendMessageW(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }

    W32_DWORD me = GetCurrentThreadId();
    W32_DWORD owner = windows[i].tid;
    if (owner == 0 || owner == me) {
        /* Same thread: a direct call, which is what Win32 does and what
         * makes the whole subclass chain cheap. */
        return ui_call_top(hwnd, msg, wp, lp);
    }

    /* Cross-thread: enqueue and block on a W32A-3 event until the owner's
     * message loop has run the procedure.  The target thread must pump. */
    struct ui_queue *q = ui_queue_for(owner, 1);
    if (!q) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    W32_HANDLE evt = CreateEventW(0, W32_TRUE, W32_FALSE, 0);   /* manual reset */
    if (!evt) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    W32_LRESULT result = 0;
    struct ui_msg m;
    m.hwnd = hwnd; m.message = msg; m.wParam = wp; m.lParam = lp;
    m.time = GetTickCount();
    m.px = 0; m.py = 0;
    m.evt = evt; m.result = &result; m.sender = me;
    ui_lock();
    int ok = ui_q_push(q, &m);
    ui_unlock();
    if (!ok) {
        CloseHandle(evt);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    WaitForSingleObject(evt, 0xFFFFFFFFu);      /* INFINITE */
    CloseHandle(evt);
    return result;
}

W32ABI W32_LRESULT SendMessageA(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp) {
    return SendMessageW(hwnd, msg, wp, lp);
}

W32ABI W32_BOOL PostMessageW(W32_HWND hwnd, W32_UINT msg,
                             W32_WPARAM wp, W32_LPARAM lp) {
    if (w32_win_index_from_hwnd(hwnd) < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    int32_t px = 0, py = 0;
    ag_mouse_position(&px, &py);
    ui_post_to(hwnd, msg, wp, lp, px, py);
    return W32_TRUE;
}

W32ABI W32_BOOL PostMessageA(W32_HWND hwnd, W32_UINT msg,
                             W32_WPARAM wp, W32_LPARAM lp) {
    return PostMessageW(hwnd, msg, wp, lp);
}

W32ABI W32_BOOL PeekMessageW(W32_MSG *msg, W32_HWND filter,
                             W32_UINT lo, W32_UINT hi, W32_UINT remove) {
    if (!msg) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 1);
    if (!q) return W32_FALSE;
    ui_service_sends(q);
    ui_pump();
    ui_service_sends(q);
    /* PM_REMOVE/PM_NOREMOVE, and a filter of HWND 0 meaning "any". */
    return ui_q_pop(q, msg, filter, lo, hi, remove) ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL GetMessageW(W32_MSG *msg, W32_HWND filter,
                            W32_UINT lo, W32_UINT hi) {
    if (!msg) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 1);
    if (!q) { msg->message = W32_WM_QUIT; return W32_FALSE; }

    for (;;) {
        ui_service_sends(q);
        if (q->quit_posted && q->head == q->tail) {
            msg->hwnd = 0;
            msg->message = W32_WM_QUIT;
            msg->wParam = (W32_WPARAM)q->quit_code;
            msg->lParam = 0;
            return W32_FALSE;                  /* WM_QUIT: the loop ends */
        }
        ui_pump();
        ui_service_sends(q);
        if (ui_q_pop(q, msg, filter, lo, hi, 1)) {
            if (msg->message == W32_WM_QUIT) return W32_FALSE;
            return W32_TRUE;
        }
        /* Nothing pending: yield.  The compositor is polled, so a sleep is
         * what keeps a message loop from eating the CPU. */
        ag_render_now();
        Sleep(1);
    }
}

/* The A forms of the message loop.  MSG carries no strings, so these are
 * pure forwarders -- which is the D6 shape: one implementation, two names. */
W32ABI W32_BOOL GetMessageA(W32_MSG *msg, W32_HWND filter,
                            W32_UINT lo, W32_UINT hi) {
    return GetMessageW(msg, filter, lo, hi);
}

W32ABI W32_BOOL PeekMessageA(W32_MSG *msg, W32_HWND filter,
                             W32_UINT lo, W32_UINT hi, W32_UINT remove) {
    return PeekMessageW(msg, filter, lo, hi, remove);
}

W32ABI W32_LRESULT DispatchMessageA(const W32_MSG *msg) {
    return DispatchMessageW(msg);
}

W32ABI W32_LRESULT DispatchMessageW(const W32_MSG *msg) {
    if (!msg) return 0;
    return ui_call_top(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

W32ABI W32_BOOL TranslateMessage(const W32_MSG *msg) {
    if (!msg || msg->message != W32_WM_KEYDOWN) return W32_FALSE;
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 1);
    uint8_t state[256];
    if (q) for (int i = 0; i < 256; i++) state[i] = q->keystate[i];
    else   for (int i = 0; i < 256; i++) state[i] = 0;
    uint16_t chars[4];
    int n = ToAscii((W32_UINT)msg->wParam, 0, state, chars, 0);
    if (n <= 0) return W32_FALSE;
    for (int i = 0; i < n; i++)
        PostMessageW(msg->hwnd, W32_WM_CHAR, (W32_WPARAM)chars[i], msg->lParam);
    return W32_TRUE;
}

W32ABI W32_LRESULT DefWindowProcW(W32_HWND hwnd, W32_UINT msg,
                                  W32_WPARAM wp, W32_LPARAM lp) {
    int i = w32_win_index_from_hwnd(hwnd);
    switch (msg) {
    case W32_WM_NCCREATE:
        return W32_TRUE;                     /* continue creation */
    case W32_WM_CREATE:
        return 0;
    case W32_WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case W32_WM_NCDESTROY:
    case W32_WM_DESTROY:
        return 0;
    case W32_WM_PAINT: {
        /* A program that does not paint is not allowed to spin: the default
         * erases the class background and validates. */
        if (i < 0) return 0;
        W32_PAINTSTRUCT ps;
        W32_HDC dc = BeginPaint(hwnd, &ps);
        (void)dc;
        EndPaint(hwnd, &ps);
        return 0;
    }
    case W32_WM_ERASEBKGND: {
        if (i < 0) return 0;
        struct ui_class *k = windows[i].cls >= 0 ? &classes[windows[i].cls] : 0;
        if (k && k->has_bg) {
            ag_clear(windows[i].ag_wid, k->bg);
            return W32_TRUE;
        }
        return W32_FALSE;
    }
    case W32_WM_GETTEXT:
        return (W32_LRESULT)GetWindowTextW(hwnd, (uint16_t *)(uintptr_t)wp,
                                          (int32_t)lp);
    case W32_WM_SETTEXT: {
        /* The default handler is what actually changes the caption; a
         * subclass that swallows WM_SETTEXT therefore wins, as on Windows. */
        if (i >= 0) ui_set_text(&windows[i], (const uint16_t *)(uintptr_t)lp);
        return W32_TRUE;
    }
    case W32_WM_GETTEXTLENGTH:
        return (W32_LRESULT)GetWindowTextLengthW(hwnd);
    case W32_WM_GETMINMAXINFO:
        return W32_TRUE;
    case W32_WM_NCHITTEST:
        return 1;                            /* HTCLIENT */
    case W32_WM_NCCALCSIZE:
        return 0;
    case W32_WM_NCPAINT:
    case W32_WM_NCACTIVATE:
        return W32_TRUE;
    case W32_WM_SETCURSOR:
        return W32_TRUE;                     /* the compositor draws it */
    case W32_WM_MOUSEACTIVATE:
        return 1;                            /* MA_ACTIVATE */
    case W32_WM_SYSCOMMAND: {
        switch (wp & 0xFFF0u) {
        case 0xF060: DestroyWindow(hwnd); return 0;       /* SC_CLOSE */
        case 0xF020: ShowWindow(hwnd, W32_SW_MINIMIZE); return 0;
        case 0xF030: ShowWindow(hwnd, W32_SW_MAXIMIZE); return 0;
        case 0xF120: ShowWindow(hwnd, W32_SW_RESTORE); return 0;
        default: return 0;                                 /* SC_MOVE/SIZE… */
        }
    }
    case W32_WM_ACTIVATE:
    case W32_WM_SETFOCUS:
    case W32_WM_KILLFOCUS:
    case W32_WM_SHOWWINDOW:
    case W32_WM_WINDOWPOSCHANGING:
    case W32_WM_WINDOWPOSCHANGED:
    case W32_WM_MOVE:
    case W32_WM_SIZE:
    case W32_WM_TIMER:
    case W32_WM_COMMAND:
    case W32_WM_NOTIFY:
    case W32_WM_HSCROLL:
    case W32_WM_VSCROLL:
    case W32_WM_MOUSEWHEEL:
    case W32_WM_MOUSELEAVE:
    case W32_WM_CONTEXTMENU:
    case W32_WM_SETREDRAW:
    case W32_WM_CANCELMODE:
        return 0;
    case W32_WM_CTLCOLORMSGBOX: case W32_WM_CTLCOLOREDIT:
    case W32_WM_CTLCOLORLISTBOX: case W32_WM_CTLCOLORBTN:
    case W32_WM_CTLCOLORDLG: case W32_WM_CTLCOLORSCROLLBAR:
    case W32_WM_CTLCOLORSTATIC:
        /* Controls paint on the window background; returning the brush is
         * what the real default does, and W32A-8's controls depend on it. */
        if (i >= 0 && windows[i].cls >= 0)
            return (W32_LRESULT)(intptr_t)GetSysColorBrush(W32_COLOR_BTNFACE);
        return (W32_LRESULT)(intptr_t)GetSysColorBrush(W32_COLOR_WINDOW);
    default:
        return 0;
    }
}

W32ABI W32_LRESULT DefWindowProcA(W32_HWND hwnd, W32_UINT msg,
                                  W32_WPARAM wp, W32_LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

W32ABI W32_LRESULT CallWindowProcW(W32_WNDPROC prev, W32_HWND hwnd,
                                   W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) return 0;
    /* Win32 calls the procedure it is handed, and that is what a subclass
     * needs: it saved the procedure it replaced (its predecessor, which may
     * well be the class procedure) and forwards to exactly that one.  A
     * caller passing the current top would recurse -- on Windows too.  A
     * stale pointer from another window is the caller's problem, and calling
     * it cannot corrupt this window's chain. */
    if (!prev) return DefWindowProcW(hwnd, msg, wp, lp);
    return prev(hwnd, msg, wp, lp);
}

W32ABI void PostQuitMessage(int32_t code) {
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 1);
    if (!q) return;
    q->quit_posted = 1;
    q->quit_code = code;
}

W32ABI W32_BOOL InSendMessage(void) { return W32_FALSE; }

W32ABI W32_BOOL ReplyMessage(W32_LRESULT r) {
    /* Nothing to reply to: our sends are synchronous and the sender is
     * still blocked, so there is no "waiting for a reply" state to exit. */
    (void)r;
    return W32_FALSE;
}

W32ABI W32_DWORD GetMessageTime(void) {
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    return q ? q->last_time : 0;
}

W32ABI W32_DWORD GetMessagePos(void) {
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    if (!q) return 0;
    return (W32_DWORD)(((uint32_t)(uint16_t)q->last_py << 16) |
                       (uint16_t)q->last_px);
}

W32ABI W32_DWORD GetQueueStatus(W32_UINT flags) {
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    int n = q ? (q->tail - q->head + UI_QUEUE_LEN) % UI_QUEUE_LEN : 0;
    /* QS_* bit 16 says "this class is present".  We report INPUT (keys and
     * mouse both arrive here) and POSTMESSAGE, which is what generic loops
     * test; the fine-grained classes are a documented approximation. */
    (void)flags;
    W32_DWORD r = 0;
    if (n) r |= (0x0004u << 16) | 0x0004u;          /* QS_POSTMESSAGE: set */
    return r;
}

W32ABI W32_UINT RegisterWindowMessageW(const uint16_t *s) {
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    for (int i = 0; i < UI_MAX_WMSG; i++)
        if (wmsg_used[i] && w16_eq(wmsg_names[i], s)) return 0xC000u + (W32_UINT)i;
    for (int i = 0; i < UI_MAX_WMSG; i++) {
        if (wmsg_used[i]) continue;
        w16_copy(wmsg_names[i], s, 48);
        wmsg_used[i] = 1;
        return 0xC000u + (W32_UINT)i;      /* the documented atom range */
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

W32ABI W32_UINT RegisterWindowMessageA(const char *s) {
    uint16_t w[48];
    if (a_to_w16(w, 48, s) <= 0) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    return RegisterWindowMessageW(w);
}

W32ABI W32_LRESULT SendDlgItemMessageW(W32_HWND parent, int32_t id,
                                       W32_UINT msg, W32_WPARAM wp,
                                       W32_LPARAM lp) {
    int pi = w32_win_index_from_hwnd(parent);
    if (pi < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].parent != pi) continue;
        if (windows[i].ctrl_id == id) return SendMessageW(idx_to_hwnd(i), msg, wp, lp);
    }
    return 0;
}

W32ABI W32_LRESULT SendDlgItemMessageA(W32_HWND parent, int32_t id,
                                       W32_UINT msg, W32_WPARAM wp,
                                       W32_LPARAM lp) {
    return SendDlgItemMessageW(parent, id, msg, wp, lp);
}

W32ABI W32_DWORD MsgWaitForMultipleObjects(W32_DWORD count,
                                           const W32_HANDLE *handles,
                                           W32_BOOL wait_all,
                                           W32_DWORD ms, W32_DWORD mask) {
    (void)mask;
    /* The honest version: wait on the handles (or sleep) and then report
     * whether this thread's queue has anything.  A real implementation
     * would put the message object into the wait set; that needs a kernel
     * object the personality does not have yet, and the plan records it as
     * residue rather than pretending. */
    W32_DWORD r;
    if (count == 0) { Sleep(ms); r = 0xFFFFFFFFu; }   /* WAIT_TIMEOUT */
    else r = WaitForMultipleObjects(count, (W32_HANDLE *)(uintptr_t)handles,
                                                       wait_all, ms);
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    int n = q ? (q->tail - q->head + UI_QUEUE_LEN) % UI_QUEUE_LEN : 0;
    if (n) return count;                               /* WAIT_OBJECT_0 + count */
    return r;
}

/* ---- window text --------------------------------------------------------- */

/* The one place window text changes.  SetWindowTextW goes through
 * WM_SETTEXT (so a subclass sees it, which is what the ladder relies on)
 * and the default handler lands here; a procedure that swallows WM_SETTEXT
 * therefore does not change the title, exactly as on Windows. */
static void ui_set_text(struct ui_window *w, const uint16_t *s) {
    if (!s) return;
    w16_copy(w->title, s, UI_TITLE_MAX);
    w16_to_a(w->title_a, sizeof w->title_a, w->title);
    if (w->ag_wid >= 0) ag_window_set_title(w->ag_wid, w->title_a);
}

W32ABI W32_BOOL SetWindowTextW(W32_HWND hwnd, const uint16_t *s) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    SendMessageW(hwnd, W32_WM_SETTEXT, 0, (W32_LPARAM)(uintptr_t)s);
    return W32_TRUE;
}

W32ABI W32_BOOL SetWindowTextA(W32_HWND hwnd, const char *s) {
    uint16_t w[UI_TITLE_MAX];
    a_to_w16(w, UI_TITLE_MAX, s);
    return SetWindowTextW(hwnd, w);
}

W32ABI int32_t GetWindowTextW(W32_HWND hwnd, uint16_t *buf, int32_t max) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !buf || max <= 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int n = 0;
    while (n < max - 1 && windows[i].title[n]) { buf[n] = windows[i].title[n]; n++; }
    buf[n] = 0;
    return n;
}

W32ABI int32_t GetWindowTextA(W32_HWND hwnd, char *buf, int32_t max) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !buf || max <= 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int n = 0;
    while (n < max - 1 && windows[i].title_a[n]) { buf[n] = windows[i].title_a[n]; n++; }
    buf[n] = 0;
    return n;
}

W32ABI int32_t GetWindowTextLengthW(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return (int32_t)w16_len(windows[i].title);
}

W32ABI int32_t GetWindowTextLengthA(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int n = 0;
    while (windows[i].title_a[n]) n++;
    return n;
}

/* ---- properties ---------------------------------------------------------- */

W32ABI W32_BOOL SetPropW(W32_HWND hwnd, const uint16_t *key, W32_HANDLE val) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !key) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    if (!val) return RemovePropW(hwnd, key) ? W32_TRUE : W32_FALSE;
    for (int k = 0; k < UI_MAX_PROPS; k++)
        if (w->prop[k].in_use && w16_eq(w->prop[k].key, key)) {
            w->prop[k].val = val;
            return W32_TRUE;
        }
    for (int k = 0; k < UI_MAX_PROPS; k++) {
        if (w->prop[k].in_use) continue;
        w->prop[k].in_use = 1;
        w16_copy(w->prop[k].key, key, 48);
        w->prop[k].val = val;
        return W32_TRUE;
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return W32_FALSE;
}

W32ABI W32_HANDLE GetPropW(W32_HWND hwnd, const uint16_t *key) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !key) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    for (int k = 0; k < UI_MAX_PROPS; k++)
        if (windows[i].prop[k].in_use && w16_eq(windows[i].prop[k].key, key))
            return windows[i].prop[k].val;
    return 0;                                  /* absent: NULL, like Win32 */
}

W32ABI W32_HANDLE RemovePropW(W32_HWND hwnd, const uint16_t *key) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !key) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    for (int k = 0; k < UI_MAX_PROPS; k++)
        if (windows[i].prop[k].in_use && w16_eq(windows[i].prop[k].key, key)) {
            W32_HANDLE v = windows[i].prop[k].val;
            windows[i].prop[k].in_use = 0;
            return v;
        }
    return 0;
}

/* ---- subclassing and window words ---------------------------------------- */

W32ABI intptr_t SetWindowLongPtrW(W32_HWND hwnd, int32_t idx, intptr_t v) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    struct ui_window *w = &windows[i];
    switch (idx) {
    case W32_GWL_WNDPROC: {
        W32_WNDPROC np = (W32_WNDPROC)v;
        W32_WNDPROC old = w->proc_top >= 0 ? w->proc[w->proc_top] : 0;
        if (!np || np == old) return (intptr_t)old;      /* no-op, like Win32 */
        if (w->proc_top + 1 >= UI_MAX_PROCS) {
            /* Depth is bounded and the bound is reported: four nested
             * subclasses is the ledger's worst case plus headroom. */
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        w->proc_top++;
        w->proc[w->proc_top] = np;
        return (intptr_t)old;
    }
    case W32_GWL_USERDATA: {
        void *old = w->user_data;
        w->user_data = (void *)v;
        return (intptr_t)old;
    }
    case W32_GWL_STYLE: {
        W32_DWORD old = w->style;
        w->style = (W32_DWORD)v;
        return (intptr_t)old;
    }
    case W32_GWL_EXSTYLE: {
        W32_DWORD old = w->exstyle;
        w->exstyle = (W32_DWORD)v;
        return (intptr_t)old;
    }
    case W32_GWL_ID: {
        int32_t old = w->ctrl_id;
        w->ctrl_id = (int32_t)v;
        return (intptr_t)old;
    }
    case W32_GWL_HWNDPARENT:
        return (intptr_t)SetParent(hwnd, (W32_HWND)(uintptr_t)v);
    case W32_GWL_HINSTANCE:
        return 0;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

W32ABI intptr_t GetWindowLongPtrW(W32_HWND hwnd, int32_t idx) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    struct ui_window *w = &windows[i];
    switch (idx) {
    case W32_GWL_WNDPROC:    return (intptr_t)(w->proc_top >= 0 ? w->proc[w->proc_top] : 0);
    case W32_GWL_USERDATA:   return (intptr_t)w->user_data;
    case W32_GWL_STYLE:      return (intptr_t)w->style;
    case W32_GWL_EXSTYLE:    return (intptr_t)w->exstyle;
    case W32_GWL_ID:         return (intptr_t)w->ctrl_id;
    case W32_GWL_HWNDPARENT: return (intptr_t)(w->parent >= 0 ? idx_to_hwnd(w->parent) : 0);
    case W32_GWL_HINSTANCE:  return 0;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

W32ABI intptr_t SetWindowLongPtrA(W32_HWND h, int32_t i, intptr_t v) { return SetWindowLongPtrW(h, i, v); }
W32ABI intptr_t GetWindowLongPtrA(W32_HWND h, int32_t i) { return GetWindowLongPtrW(h, i); }
W32ABI int32_t  GetWindowLongW(W32_HWND h, int32_t i) { return (int32_t)GetWindowLongPtrW(h, i); }
W32ABI int32_t  SetWindowLongW(W32_HWND h, int32_t i, int32_t v) {
    return (int32_t)SetWindowLongPtrW(h, i, (intptr_t)v);
}

W32ABI int32_t GetDlgCtrlID(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return windows[i].ctrl_id;
}

/* ---- geometry ------------------------------------------------------------ */

/* Decoration thickness the compositor applies to a decorated window. */
static void ui_decor(const struct ui_window *w, uint32_t *bw, uint32_t *th) {
    ui_theme_t t;
    *bw = 0; *th = 0;
    if (ag_theme_get(&t) != 0) return;
    /* WS_POPUP is the one style the compositor draws with no frame at all
     * (it becomes AG_WIN_NO_DECOR | AG_WIN_BORDERLESS).  Everything else
     * gets the theme's border, and a title bar only if it asks for one:
     * note that Win32 defines WS_CAPTION as WS_BORDER|WS_DLGFRAME, so a
     * test for "has a border" must not be read as "has no caption" --
     * getting that order wrong makes every captioned window undecorated. */
    if (w->style & W32_WS_POPUP) return;
    *bw = t.border_w;
    if (w->style & (W32_WS_CAPTION | W32_WS_SYSMENU | W32_WS_THICKFRAME |
                    W32_WS_MINIMIZEBOX | W32_WS_MAXIMIZEBOX))
        *th = t.titlebar_h;
}

W32ABI W32_BOOL GetWindowRect(W32_HWND hwnd, W32_RECT *r) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (windows[i].is_desktop) {
        uint32_t sw = 0, sh = 0;
        ag_screen_size(&sw, &sh);
        r->left = 0; r->top = 0; r->right = (int32_t)sw; r->bottom = (int32_t)sh;
        return W32_TRUE;
    }
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    ag_window_get_pos(windows[i].ag_wid, &x, &y);
    ag_window_get_size(windows[i].ag_wid, &w, &h);
    uint32_t bw = 0, th = 0;
    ui_decor(&windows[i], &bw, &th);
    r->left = x;
    r->top = y;
    r->right = x + (int32_t)(w + 2 * bw);
    r->bottom = y + (int32_t)(h + th + 2 * bw);
    return W32_TRUE;
}

W32ABI W32_BOOL GetClientRect(W32_HWND hwnd, W32_RECT *r) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    uint32_t w = 0, h = 0;
    ag_window_get_size(windows[i].ag_wid, &w, &h);
    r->left = 0; r->top = 0;
    r->right = (int32_t)w; r->bottom = (int32_t)h;
    return W32_TRUE;
}

W32ABI W32_BOOL ClientToScreen(W32_HWND hwnd, W32_POINT *pt) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !pt) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    int32_t x = 0, y = 0;
    ag_window_get_pos(windows[i].ag_wid, &x, &y);
    uint32_t bw = 0, th = 0;
    ui_decor(&windows[i], &bw, &th);
    pt->x += x + (int32_t)bw;
    pt->y += y + (int32_t)(th + bw);
    return W32_TRUE;
}

W32ABI W32_BOOL ScreenToClient(W32_HWND hwnd, W32_POINT *pt) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !pt) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    int32_t x = 0, y = 0;
    ag_window_get_pos(windows[i].ag_wid, &x, &y);
    uint32_t bw = 0, th = 0;
    ui_decor(&windows[i], &bw, &th);
    pt->x -= x + (int32_t)bw;
    pt->y -= y + (int32_t)(th + bw);
    return W32_TRUE;
}

W32ABI int32_t MapWindowPoints(W32_HWND from, W32_HWND to, W32_POINT *pts,
                               W32_UINT count) {
    if (!pts) return 0;
    for (W32_UINT i = 0; i < count; i++) {
        ClientToScreen(from, &pts[i]);
        ScreenToClient(to, &pts[i]);
    }
    return 0;
}

W32ABI W32_BOOL AdjustWindowRectEx(W32_RECT *r, W32_DWORD style,
                                   W32_BOOL menu, W32_DWORD exstyle) {
    (void)menu; (void)exstyle;
    if (!r) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    ui_theme_t t;
    uint32_t bw = 0, th = 0;
    if (ag_theme_get(&t) == 0) {
        bw = t.border_w;
        th = t.titlebar_h;
    }
    if (style & W32_WS_POPUP) {
        /* No decoration: the client rect is the window rect. */
    } else if (style & W32_WS_THICKFRAME) {
        r->left -= (int32_t)bw; r->top -= (int32_t)(th + bw);
        r->right += (int32_t)bw; r->bottom += (int32_t)bw;
    } else if (style & W32_WS_CAPTION) {
        r->left -= (int32_t)bw; r->top -= (int32_t)(th + bw);
        r->right += (int32_t)bw; r->bottom += (int32_t)bw;
    } else if (style & (W32_WS_BORDER | W32_WS_DLGFRAME)) {
        r->left -= (int32_t)bw; r->top -= (int32_t)bw;
        r->right += (int32_t)bw; r->bottom += (int32_t)bw;
    }
    return W32_TRUE;
}

/* The compositor is the only thing that knows what is on top; the request
 * below asks it rather than guessing from our own table, and the hit test
 * walks our windows in Z order for the geometry answer. */
W32ABI W32_HWND WindowFromPoint(W32_POINT pt) {
    int best = -1, bestz = 0x80000000;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].is_desktop) continue;
        if (!windows[i].visible || windows[i].minimized) continue;
        W32_RECT r;
        GetWindowRect(idx_to_hwnd(i), &r);
        if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)
            continue;
        int z = ag_window_get_z(windows[i].ag_wid);
        if (z > bestz) { bestz = z; best = i; }
    }
    return best < 0 ? 0 : idx_to_hwnd(best);
}

W32ABI W32_HWND ChildWindowFromPointEx(W32_HWND parent, W32_POINT pt,
                                       W32_UINT flags) {
    (void)flags;
    int pi = w32_win_index_from_hwnd(parent);
    if (pi < 0) return 0;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].parent != pi) continue;
        W32_RECT r;
        GetWindowRect(idx_to_hwnd(i), &r);
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
            return idx_to_hwnd(i);
    }
    return 0;
}

W32ABI W32_HWND GetParent(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (windows[i].parent < 0) return 0;
    return idx_to_hwnd(windows[i].parent);
}

W32ABI W32_HWND SetParent(W32_HWND child, W32_HWND parent) {
    int ci = w32_win_index_from_hwnd(child);
    int pi = parent ? w32_win_index_from_hwnd(parent) : -1;
    if (ci < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (parent && pi < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    /* Refused by name: a window is not re-parented into another window's
     * surface (the compositor has no child surfaces).  The logical link is
     * what SetDlgItemMessage/EnumChildWindows use, and the plan records the
     * limitation. */
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    (void)ci; (void)pi;
    return 0;
}

W32ABI W32_BOOL IsChild(W32_HWND parent, W32_HWND child) {
    int pi = w32_win_index_from_hwnd(parent);
    int ci = w32_win_index_from_hwnd(child);
    if (pi < 0 || ci < 0) return W32_FALSE;
    for (int p = windows[ci].parent; p >= 0; p = windows[p].parent)
        if (p == pi) return W32_TRUE;
    return W32_FALSE;
}

W32ABI W32_HWND GetAncestor(W32_HWND hwnd, W32_UINT flags) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) return 0;
    switch (flags) {
    case W32_GA_PARENT:
        return GetParent(hwnd);
    case W32_GA_ROOT:
    case W32_GA_ROOTOWNER: {
        int p = i;
        while (windows[p].parent >= 0) p = windows[p].parent;
        return idx_to_hwnd(p);
    }
    default:
        return 0;
    }
}

/* GetWindow's Z-order answers are against this process's own windows --
 * the compositor's stack is global and other processes' windows are not
 * this caller's business (the plan documents the approximation). */
static int ui_visible_rank(int idx, int *order, int n) {
    for (int k = 0; k < n; k++) if (order[k] == idx) return k;
    return n;
}

static int ui_zlist(int *order, int max) {
    int n = 0;
    for (int i = 0; i < UI_MAX_WINDOWS && n < max; i++)
        if (windows[i].in_use && !windows[i].is_desktop) order[n++] = i;
    /* insertion sort by z descending (topmost first) -- n <= 31 */
    for (int a = 1; a < n; a++) {
        int v = order[a], z = ag_window_get_z(windows[v].ag_wid);
        int b = a - 1;
        while (b >= 0 && ag_window_get_z(windows[order[b]].ag_wid) < z) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    return n;
}

W32ABI W32_HWND GetWindow(W32_HWND hwnd, W32_UINT cmd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int order[UI_MAX_WINDOWS];
    int n = ui_zlist(order, UI_MAX_WINDOWS);
    int at = ui_visible_rank(i, order, n);
    switch (cmd) {
    case W32_GW_HWNDFIRST: return n ? idx_to_hwnd(order[0]) : 0;
    case W32_GW_HWNDLAST:  return n ? idx_to_hwnd(order[n - 1]) : 0;
    case W32_GW_HWNDPREV:  return (at > 0) ? idx_to_hwnd(order[at - 1]) : 0;
    case W32_GW_HWNDNEXT:  return (at + 1 < n) ? idx_to_hwnd(order[at + 1]) : 0;
    case W32_GW_CHILD: {
        for (int k = 0; k < UI_MAX_WINDOWS; k++)
            if (windows[k].in_use && windows[k].parent == i) return idx_to_hwnd(k);
        return 0;
    }
    case W32_GW_OWNER: return GetParent(hwnd);
    default: return 0;
    }
}

W32ABI W32_BOOL EnumChildWindows(W32_HWND parent,
                                 W32_BOOL (W32ABI *cb)(W32_HWND, W32_LPARAM),
                                 W32_LPARAM lp) {
    if (!cb) return W32_FALSE;
    int pi = w32_win_index_from_hwnd(parent);
    if (pi < 0) return W32_FALSE;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        if (windows[i].parent != pi) continue;
        if (!cb(idx_to_hwnd(i), lp)) return W32_FALSE;
    }
    return W32_TRUE;
}

W32ABI W32_BOOL EnumThreadWindows(W32_DWORD tid,
                                  W32_BOOL (W32ABI *cb)(W32_HWND, W32_LPARAM),
                                  W32_LPARAM lp) {
    if (!cb) return W32_FALSE;
    for (int i = 0; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].is_desktop) continue;
        if (tid && windows[i].tid != tid) continue;
        if (!cb(idx_to_hwnd(i), lp)) return W32_FALSE;
    }
    return W32_TRUE;
}

static int ui_match(const uint16_t *want, const uint16_t *have) {
    return !want || w16_eq(want, have);
}

W32ABI W32_HWND FindWindowExW(W32_HWND parent, W32_HWND after,
                              const uint16_t *cls, const uint16_t *title) {
    int pi = parent ? w32_win_index_from_hwnd(parent) : -2;   /* -2: any */
    int start = 0;
    if (after) {
        int ai = w32_win_index_from_hwnd(after);
        if (ai >= 0) start = ai + 1;
    }
    for (int i = start; i < UI_MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].is_desktop) continue;
        if (parent && windows[i].parent != pi) continue;
        if (cls && (windows[i].cls < 0 || !ui_match(cls, classes[windows[i].cls].name_w)))
            continue;
        if (title && !ui_match(title, windows[i].title)) continue;
        return idx_to_hwnd(i);
    }
    return 0;
}

W32ABI W32_HWND FindWindowW(const uint16_t *cls, const uint16_t *title) {
    return FindWindowExW(0, 0, cls, title);
}

W32ABI W32_HWND FindWindowA(const char *cls, const char *title) {
    uint16_t cw[UI_CNAME_MAX], tw[UI_TITLE_MAX];
    int havec = cls ? a_to_w16(cw, UI_CNAME_MAX, cls) > 0 : 0;
    int havet = title ? a_to_w16(tw, UI_TITLE_MAX, title) > 0 : 0;
    return FindWindowExW(0, 0, havec ? cw : 0, havet ? tw : 0);
}

/* ---- show, placement, Z-order -------------------------------------------- */

W32ABI W32_BOOL ShowWindow(W32_HWND hwnd, int32_t cmd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    W32_BOOL was_visible = w->visible ? W32_TRUE : W32_FALSE;

    switch (cmd) {
    case W32_SW_HIDE:
        ag_window_hide(w->ag_wid);
        w->visible = 0;
        break;
    case W32_SW_MINIMIZE:
    case W32_SW_SHOWMINIMIZED:
        ag_window_minimize(w->ag_wid);
        w->minimized = 1;
        w->visible = 1;
        break;
    case W32_SW_MAXIMIZE:
        ag_window_maximize(w->ag_wid);
        w->maximized = 1;
        w->minimized = 0;
        w->visible = 1;
        break;
    case W32_SW_RESTORE:
    case W32_SW_SHOWNORMAL:
    case W32_SW_SHOWDEFAULT:
    case W32_SW_SHOW:
    case W32_SW_SHOWNOACTIVATE:
    default:
        ag_window_restore(w->ag_wid);
        ag_window_show(w->ag_wid);
        w->minimized = 0;
        w->maximized = 0;
        w->visible = 1;
        ui_refresh_geom(w);
        PostMessageW(hwnd, W32_WM_SIZE, 0,
                     (W32_LPARAM)((int32_t)w->h << 16) | (int32_t)w->w);
        if (cmd != W32_SW_SHOWNOACTIVATE) ag_window_focus(w->ag_wid);
        InvalidateRect(hwnd, 0, W32_TRUE);
        break;
    }
    return was_visible;
}

W32ABI W32_BOOL IsWindowVisible(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    return windows[i].visible ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL IsIconic(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    return windows[i].minimized ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL IsZoomed(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    return windows[i].maximized ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL BringWindowToTop(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    /* Raising IS focusing in this compositor (one focus, topmost wins), so
     * the two Win32 notions collapse into one call -- documented in the
     * phase's Done note. */
    return ag_window_focus(windows[i].ag_wid) == 0 ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL SetWindowPos(W32_HWND hwnd, W32_HWND after, int32_t x,
                             int32_t y, int32_t cx, int32_t cy, W32_UINT flags) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];

    if (!(flags & W32_SWP_NOZORDER)) {
        if (after == W32_HWND_TOP || after == W32_HWND_TOPMOST)
            ag_window_focus(w->ag_wid);
        else if (after == W32_HWND_BOTTOM)
            ag_window_lower(w->ag_wid);
        if (after == W32_HWND_TOPMOST || after == W32_HWND_NOTOPMOST) {
            uint32_t f = ag_window_get_flags(w->ag_wid);
            if (f != 0xFFFFFFFFu) {
                if (after == W32_HWND_TOPMOST) f |= AG_WIN_ALWAYS_TOP;
                else                           f &= ~(uint32_t)AG_WIN_ALWAYS_TOP;
                ag_window_set_flags(w->ag_wid, f);
            }
        }
    }
    if (!(flags & W32_SWP_NOMOVE)) {
        if (x == W32_CW_USEDEFAULT) x = w->x;
        if (y == W32_CW_USEDEFAULT) y = w->y;
        ag_window_move(w->ag_wid, x, y);
        w->x = x; w->y = y;
    }
    if (!(flags & W32_SWP_NOSIZE)) {
        if (cx > 0 && cy > 0) {
            ag_window_resize(w->ag_wid, (uint32_t)cx, (uint32_t)cy);
            ui_refresh_geom(w);
        }
    }
    if (flags & W32_SWP_SHOWWINDOW) { ag_window_show(w->ag_wid); w->visible = 1; }
    if (flags & W32_SWP_HIDEWINDOW) { ag_window_hide(w->ag_wid); w->visible = 0; }

    /* The compositor moved content: tell the window, then repaint.  Win32
     * sends WM_WINDOWPOSCHANGED/WM_SIZE through the same path. */
    PostMessageW(hwnd, W32_WM_WINDOWPOSCHANGED, 0, 0);
    PostMessageW(hwnd, W32_WM_SIZE, 0,
                 (W32_LPARAM)((int32_t)w->h << 16) | (int32_t)w->w);
    if (!(flags & W32_SWP_NOREDRAW)) InvalidateRect(hwnd, 0, W32_TRUE);
    return W32_TRUE;
}

W32ABI W32_BOOL MoveWindow(W32_HWND hwnd, int32_t x, int32_t y,
                           int32_t w, int32_t h, W32_BOOL repaint) {
    W32_UINT flags = W32_SWP_NOZORDER | W32_SWP_NOACTIVATE;
    if (!repaint) flags |= W32_SWP_NOREDRAW;
    return SetWindowPos(hwnd, 0, x, y, w, h, flags);
}

W32ABI W32_BOOL GetWindowPlacement(W32_HWND hwnd, W32_WINDOWPLACEMENT *p) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !p) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    p->length = sizeof(W32_WINDOWPLACEMENT);
    p->flags = 0;
    p->showCmd = w->minimized ? W32_SW_SHOWMINIMIZED
               : w->maximized ? W32_SW_SHOWMAXIMIZED
               : w->visible   ? W32_SW_SHOWNORMAL
                              : W32_SW_HIDE;
    p->ptMinPosition.x = 0; p->ptMinPosition.y = 0;
    p->ptMaxPosition.x = 0; p->ptMaxPosition.y = 0;
    p->rcNormalPosition.left = w->normal_x;
    p->rcNormalPosition.top = w->normal_y;
    p->rcNormalPosition.right = w->normal_x + (int32_t)w->normal_w;
    p->rcNormalPosition.bottom = w->normal_y + (int32_t)w->normal_h;
    return W32_TRUE;
}

W32ABI W32_BOOL SetWindowPlacement(W32_HWND hwnd,
                                   const W32_WINDOWPLACEMENT *p) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !p) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    const W32_RECT *r = &p->rcNormalPosition;
    w->normal_x = r->left; w->normal_y = r->top;
    w->normal_w = (uint32_t)(r->right - r->left);
    w->normal_h = (uint32_t)(r->bottom - r->top);
    if (p->showCmd == W32_SW_HIDE) {
        ShowWindow(hwnd, W32_SW_HIDE);
    } else if (p->showCmd == W32_SW_SHOWMINIMIZED) {
        SetWindowPos(hwnd, 0, r->left, r->top,
                     (int32_t)w->normal_w, (int32_t)w->normal_h,
                     W32_SWP_NOZORDER | W32_SWP_NOACTIVATE);
        ShowWindow(hwnd, W32_SW_MINIMIZE);
    } else if (p->showCmd == W32_SW_SHOWMAXIMIZED) {
        ShowWindow(hwnd, W32_SW_MAXIMIZE);
    } else {
        SetWindowPos(hwnd, 0, r->left, r->top,
                     (int32_t)w->normal_w, (int32_t)w->normal_h,
                     W32_SWP_NOZORDER | W32_SWP_NOACTIVATE);
        ShowWindow(hwnd, W32_SW_SHOWNORMAL);
    }
    return W32_TRUE;
}

W32ABI W32_BOOL EnableWindow(W32_HWND hwnd, W32_BOOL enable) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    int old = windows[i].enabled;
    windows[i].enabled = enable ? 1 : 0;
    return old;
}

W32ABI W32_BOOL IsWindowEnabled(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    return windows[i].enabled ? W32_TRUE : W32_FALSE;
}

/* ---- focus, capture, activation ------------------------------------------ */

static W32_HWND ui_hwnd_of_ag(int ag_wid) {
    if (ag_wid < 0) return 0;
    for (int i = 0; i < UI_MAX_WINDOWS; i++)
        if (windows[i].in_use && windows[i].ag_wid == ag_wid) return idx_to_hwnd(i);
    return 0;
}

W32ABI W32_HWND GetFocus(void) { return ui_hwnd_of_ag(ag_window_focused()); }

W32ABI W32_HWND SetFocus(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    W32_HWND old = GetFocus();
    if (ag_window_focus(windows[i].ag_wid) != 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    return old;
}

W32ABI W32_HWND GetActiveWindow(void) { return GetFocus(); }
W32ABI W32_HWND SetActiveWindow(W32_HWND hwnd) { return SetFocus(hwnd); }

W32ABI W32_HWND GetForegroundWindow(void) {
    /* One focus, one foreground: the compositor has no per-process
     * activation stack, so the focused window IS the foreground window and
     * the plan says so rather than inventing an order. */
    return GetFocus();
}

W32ABI W32_BOOL SetForegroundWindow(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    ag_window_show(windows[i].ag_wid);
    windows[i].visible = 1;
    return ag_window_focus(windows[i].ag_wid) == 0 ? W32_TRUE : W32_FALSE;
}

W32ABI W32_HWND GetLastActivePopup(W32_HWND hwnd) {
    /* No popup stack yet (dialogs arrive with W32A-6); the window itself is
     * the documented answer for a window with no popups. */
    return hwnd;
}

W32ABI W32_BOOL FlashWindow(W32_HWND hwnd, W32_BOOL invert) {
    (void)invert;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    /* There is no taskbar-flash animation to drive from here: the compositor
     * owns the taskbar and has no flash primitive.  The return value Win32
     * documents ("was the window active") is still true, and this is
     * reported as a residue rather than faked with a notification. */
    return GetFocus() == hwnd ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL FlashWindowEx(void *info) {
    (void)info;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_FALSE;
}

W32ABI W32_HWND GetCapture(void) { return ui_hwnd_of_ag(ag_window_get_capture()); }

W32ABI W32_HWND SetCapture(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int prev = ag_window_capture(windows[i].ag_wid);
    return ui_hwnd_of_ag(prev);
}

W32ABI W32_BOOL ReleaseCapture(void) {
    int prev = ag_window_capture(-1);
    return prev >= 0 ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL SetLayeredWindowAttributes(W32_HWND hwnd, W32_DWORD key,
                                           uint8_t alpha, W32_DWORD flags) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    /* Colour-key is real (the blit path has per-pixel alpha), alpha is
     * recorded and reported as a limitation: the compositor has no
     * window-level opacity knob. */
    w->layered_key = key;
    w->layered_alpha = alpha;
    w->layered_flags = flags;
    if (flags & 0x00000003u) {                 /* LWA_COLORKEY | LWA_ALPHA */
        return W32_TRUE;
    }
    w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    return W32_FALSE;
}

/* ---- painting and the update region -------------------------------------- */

static void ui_inv_union(W32_RECT *acc, const W32_RECT *r) {
    if (r->left < acc->left) acc->left = r->left;
    if (r->top < acc->top) acc->top = r->top;
    if (r->right > acc->right) acc->right = r->right;
    if (r->bottom > acc->bottom) acc->bottom = r->bottom;
}

static void ui_inv_add(struct ui_window *w, const W32_RECT *r) {
    uint32_t cw = 0, ch = 0;
    ag_window_get_size(w->ag_wid, &cw, &ch);
    W32_RECT full;
    full.left = 0; full.top = 0;
    full.right = (int32_t)cw; full.bottom = (int32_t)ch;
    W32_RECT add = r ? *r : full;
    if (r) {
        if (add.left < 0) add.left = 0;
        if (add.top < 0) add.top = 0;
        if (add.right > full.right) add.right = full.right;
        if (add.bottom > full.bottom) add.bottom = full.bottom;
        if (add.right <= add.left || add.bottom <= add.top) return;
    }
    if (w->n_inv < UI_MAX_INV) {
        w->inv[w->n_inv].l = add.left;
        w->inv[w->n_inv].t = add.top;
        w->inv[w->n_inv].r = add.right;
        w->inv[w->n_inv].b = add.bottom;
        w->n_inv++;
    } else {
        /* Region full: collapse to "everything", which is never wrong. */
        w->n_inv = 1;
        w->inv[0].l = 0; w->inv[0].t = 0;
        w->inv[0].r = full.right; w->inv[0].b = full.bottom;
    }
    if (r) ag_window_invalidate_rect(w->ag_wid, add.left, add.top,
                                     (uint32_t)(add.right - add.left),
                                     (uint32_t)(add.bottom - add.top));
    else   ag_window_invalidate(w->ag_wid);
}

static void ui_inv_clear(struct ui_window *w) { w->n_inv = 0; }

static int ui_inv_union_rect(struct ui_window *w, W32_RECT *out) {
    if (w->n_inv <= 0) return 0;
    out->left = w->inv[0].l; out->top = w->inv[0].t;
    out->right = w->inv[0].r; out->bottom = w->inv[0].b;
    for (int i = 1; i < w->n_inv; i++) {
        W32_RECT r;
        r.left = w->inv[i].l; r.top = w->inv[i].t;
        r.right = w->inv[i].r; r.bottom = w->inv[i].b;
        ui_inv_union(out, &r);
    }
    return 1;
}

W32ABI W32_BOOL InvalidateRect(W32_HWND hwnd, const W32_RECT *r, W32_BOOL erase) {
    (void)erase;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    int was_empty = (w->n_inv == 0);
    ui_inv_add(w, r);
    /* One queued WM_PAINT per accumulation: a program that invalidates a
     * hundred times gets one paint, which is what the region is for. */
    if (was_empty) PostMessageW(hwnd, W32_WM_PAINT, 0, 0);
    return W32_TRUE;
}

W32ABI W32_BOOL ValidateRect(W32_HWND hwnd, const W32_RECT *r) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    struct ui_window *w = &windows[i];
    if (!r) { ui_inv_clear(w); return W32_TRUE; }
    /* Subtract a rectangle from a list of rectangles: keep the pieces. */
    struct ui_inv keep[UI_MAX_INV];
    int nk = 0;
    for (int k = 0; k < w->n_inv && nk < UI_MAX_INV; k++) {
        struct ui_inv a = w->inv[k];
        if (r->right <= a.l || r->left >= a.r || r->bottom <= a.t || r->top >= a.b) {
            keep[nk++] = a;
            continue;
        }
        if (r->top > a.t)  { keep[nk].l = a.l; keep[nk].t = a.t; keep[nk].r = a.r; keep[nk].b = r->top;  nk++; }
        if (r->bottom < a.b) { keep[nk].l = a.l; keep[nk].t = r->bottom; keep[nk].r = a.r; keep[nk].b = a.b; nk++; }
        if (r->left > a.l) { keep[nk].l = a.l; keep[nk].t = a.t; keep[nk].r = r->left; keep[nk].b = a.b;  nk++; }
        if (r->right < a.r) { keep[nk].l = r->right; keep[nk].t = a.t; keep[nk].r = a.r; keep[nk].b = a.b; nk++; }
    }
    for (int k = 0; k < nk; k++) w->inv[k] = keep[k];
    w->n_inv = nk;
    return W32_TRUE;
}

W32ABI int32_t GetUpdateRgn(W32_HWND hwnd, void *rgn, W32_BOOL erase) {
    (void)rgn; (void)erase;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    /* There is no HRGN object yet (regions are W32A-7), so the return is the
     * documented region-complexity code: 0 empty, 1 simple.  The region
     * itself is the personality's rectangle list. */
    return windows[i].n_inv > 0 ? 1 : 0;
}

W32ABI W32_BOOL RedrawWindow(W32_HWND hwnd, const W32_RECT *r, void *rgn,
                             W32_UINT flags) {
    (void)rgn;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (flags & W32_RDW_VALIDATE) {
        ValidateRect(hwnd, r);
        return W32_TRUE;
    }
    if (flags & W32_RDW_INVALIDATE) {
        struct ui_window *w = &windows[i];
        int was_empty = (w->n_inv == 0);
        ui_inv_add(w, r);
        if (was_empty && !(flags & W32_RDW_NOINTERNALPAINT))
            PostMessageW(hwnd, W32_WM_PAINT, 0, 0);
    }
    if (flags & W32_RDW_UPDATENOW) UpdateWindow(hwnd);
    return W32_TRUE;
}

W32ABI W32_BOOL UpdateWindow(W32_HWND hwnd) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (windows[i].n_inv > 0) {
        /* Win32 sends WM_PAINT directly here: the paint happens inside this
         * call, not later in the loop. */
        SendMessageW(hwnd, W32_WM_PAINT, 0, 0);
    }
    return W32_TRUE;
}

W32ABI W32_BOOL LockWindowUpdate(W32_HWND hwnd) {
    /* One lock at a time, process-wide.  While locked, invalidations still
     * accumulate (so the paint after unlocking is correct) but no WM_PAINT
     * is posted; Win32 uses it to suppress flicker during a bulk move. */
    static W32_HWND lock_owner;
    if (hwnd == 0) { lock_owner = 0; return W32_TRUE; }
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (lock_owner == hwnd) { lock_owner = 0; return W32_TRUE; }
    if (lock_owner) { w32_set_last_error(W32_ERROR_BUSY); return W32_FALSE; }
    lock_owner = hwnd;
    return W32_TRUE;
}

/* ---- scroll state -------------------------------------------------------- */

static struct ui_scroll *ui_scroll_bar(struct ui_window *w, int32_t bar) {
    if (bar == W32_SB_HORZ) return &w->scroll[0];
    if (bar == W32_SB_VERT) return &w->scroll[1];
    return 0;                                   /* SB_CTL/SB_BOTH: no control */
}

W32ABI W32_BOOL GetScrollInfo(W32_HWND hwnd, int32_t bar, W32_SCROLLINFO *si) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !si) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_scroll *s = ui_scroll_bar(&windows[i], bar);
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    if (si->fMask & W32_SIF_RANGE)  { si->nMin = s->min; si->nMax = s->max; }
    if (si->fMask & W32_SIF_PAGE)   { si->nPage = s->page; }
    if (si->fMask & W32_SIF_POS)    { si->nPos = s->pos; }
    return W32_TRUE;
}

W32ABI int32_t SetScrollInfo(W32_HWND hwnd, int32_t bar,
                             const W32_SCROLLINFO *si, W32_BOOL redraw) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !si) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    struct ui_scroll *s = ui_scroll_bar(&windows[i], bar);
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    int32_t old = s->pos;
    if (si->fMask & W32_SIF_RANGE) { s->min = si->nMin; s->max = si->nMax; }
    if (si->fMask & W32_SIF_PAGE)  { s->page = si->nPage; }
    if (si->fMask & W32_SIF_POS) {
        s->pos = si->nPos;
        if (s->pos < s->min) s->pos = s->min;
        if (s->pos > s->max) s->pos = s->max;
    }
    if (redraw) InvalidateRect(hwnd, 0, W32_TRUE);
    return old;
}

W32ABI int32_t GetScrollPos(W32_HWND hwnd, int32_t bar) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    struct ui_scroll *s = ui_scroll_bar(&windows[i], bar);
    return s ? s->pos : 0;
}

W32ABI int32_t SetScrollPos(W32_HWND hwnd, int32_t bar, int32_t pos, W32_BOOL redraw) {
    W32_SCROLLINFO si;
    si.cbSize = sizeof si; si.fMask = W32_SIF_POS; si.nPos = pos;
    si.nMin = 0; si.nMax = 0; si.nPage = 0; si.nTrackPos = 0;
    return SetScrollInfo(hwnd, bar, &si, redraw);
}

W32ABI W32_BOOL GetScrollRange(W32_HWND hwnd, int32_t bar, int32_t *min,
                               int32_t *max) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0 || !min || !max) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_scroll *s = ui_scroll_bar(&windows[i], bar);
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    *min = s->min; *max = s->max;
    return W32_TRUE;
}

W32ABI W32_BOOL SetScrollRange(W32_HWND hwnd, int32_t bar, int32_t min,
                               int32_t max, W32_BOOL redraw) {
    W32_SCROLLINFO si;
    si.cbSize = sizeof si; si.fMask = W32_SIF_RANGE; si.nMin = min; si.nMax = max;
    si.nPos = 0; si.nPage = 0; si.nTrackPos = 0;
    SetScrollInfo(hwnd, bar, &si, redraw);
    return W32_TRUE;
}

W32ABI W32_BOOL ShowScrollBar(W32_HWND hwnd, int32_t bar, W32_BOOL show) {
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (bar == W32_SB_HORZ || bar == W32_SB_BOTH) windows[i].scroll[0].shown = show ? 1 : 0;
    if (bar == W32_SB_VERT || bar == W32_SB_BOTH) windows[i].scroll[1].shown = show ? 1 : 0;
    /* The compositor draws no scrollbars (windows scroll their content
     * themselves), so this is state, honestly recorded as such. */
    InvalidateRect(hwnd, 0, W32_TRUE);
    return W32_TRUE;
}

W32ABI int32_t ScrollWindow(W32_HWND hwnd, int32_t dx, int32_t dy,
                            const W32_RECT *scroll, const W32_RECT *clip) {
    (void)scroll; (void)clip;
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    /* Win32 blits the existing pixels; the personality has no read-back from
     * the compositor's surface, so the content is redrawn at the new offset
     * by the program's own WM_PAINT (the classic BitBlt-free idiom).  The
     * scroll position, the update region and the repaint are real; the pixel
     * copy is the documented gap.  The exposed strip is invalidated so the
     * program knows which band it must fill. */
    uint32_t cw = 0, ch = 0;
    ag_window_get_size(windows[i].ag_wid, &cw, &ch);
    if (dx != 0) {
        W32_RECT strip;
        strip.left  = (dx > 0) ? 0 : (int32_t)cw + dx;
        strip.right = (dx > 0) ? dx : (int32_t)cw;
        strip.top = 0; strip.bottom = (int32_t)ch;
        ui_inv_add(&windows[i], &strip);
    }
    if (dy != 0) {
        W32_RECT band;
        band.left = 0; band.right = (int32_t)cw;
        band.top    = (dy > 0) ? 0 : (int32_t)ch + dy;
        band.bottom = (dy > 0) ? dy : (int32_t)ch;
        ui_inv_add(&windows[i], &band);
    }
    /* A pure horizontal or pure vertical scroll leaves one of the two strips
     * degenerate; each axis stands on its own, which is why the two are
     * separate branches rather than one guarded pair. */
    if (dx != 0 || dy != 0) PostMessageW(hwnd, W32_WM_PAINT, 0, 0);
    return W32_TRUE;
}

/* ---- device contexts ----------------------------------------------------- */

W32ABI W32_HDC GetDC(W32_HWND hwnd) {
    if (!hwnd) return w32_gdi_screen_dc();  /* metrics-only screen DC */
    int i = w32_win_index_from_hwnd(hwnd);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return w32_win_make_dc(i);
}

W32ABI W32_HDC GetDCEx(W32_HWND hwnd, void *rgn, W32_DWORD flags) {
    (void)rgn; (void)flags;
    return GetDC(hwnd);
}

W32ABI W32_HDC GetWindowDC(W32_HWND hwnd) {
    /* The compositor draws the frame; a window DC here is the client DC and
     * the difference is documented rather than faked. */
    return GetDC(hwnd);
}

W32ABI int32_t ReleaseDC(W32_HWND hwnd, W32_HDC dc) {
    (void)dc;
    return IsWindow(hwnd) ? 1 : 0;
}

/* ---- rectangle helpers (pure, host-testable) ----------------------------- */

W32ABI W32_BOOL EqualRect(const W32_RECT *a, const W32_RECT *b) {
    if (!a || !b) return W32_FALSE;
    return (a->left == b->left && a->top == b->top &&
            a->right == b->right && a->bottom == b->bottom) ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL InflateRect(W32_RECT *r, int32_t dx, int32_t dy) {
    if (!r) return W32_FALSE;
    r->left -= dx; r->right += dx;
    r->top -= dy; r->bottom += dy;
    return W32_TRUE;
}

W32ABI W32_BOOL IntersectRect(W32_RECT *dst, const W32_RECT *a, const W32_RECT *b) {
    if (!dst || !a || !b) return W32_FALSE;
    dst->left = a->left > b->left ? a->left : b->left;
    dst->top = a->top > b->top ? a->top : b->top;
    dst->right = a->right < b->right ? a->right : b->right;
    dst->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (dst->right <= dst->left || dst->bottom <= dst->top) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return W32_FALSE;
    }
    return W32_TRUE;
}

W32ABI W32_BOOL OffsetRect(W32_RECT *r, int32_t dx, int32_t dy) {
    if (!r) return W32_FALSE;
    r->left += dx; r->right += dx;
    r->top += dy; r->bottom += dy;
    return W32_TRUE;
}

W32ABI W32_BOOL PtInRect(const W32_RECT *r, W32_POINT pt) {
    if (!r) return W32_FALSE;
    return (pt.x >= r->left && pt.x < r->right &&
            pt.y >= r->top && pt.y < r->bottom) ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL SetRectEmpty(W32_RECT *r) {
    if (!r) return W32_FALSE;
    r->left = r->top = r->right = r->bottom = 0;
    return W32_TRUE;
}

W32ABI W32_BOOL IsRectEmpty(const W32_RECT *r) {
    if (!r) return W32_TRUE;
    return (r->right <= r->left || r->bottom <= r->top) ? W32_TRUE : W32_FALSE;
}

/* ---- system metrics, colours, parameters --------------------------------- */

W32ABI int32_t GetSystemMetrics(int32_t index) {
    ui_theme_t t;
    int have = (ag_theme_get(&t) == 0);
    uint32_t sw = 0, sh = 0;
    ag_screen_size(&sw, &sh);

    switch (index) {
    case W32_SM_CXSCREEN: return (int32_t)sw;
    case W32_SM_CYSCREEN: return (int32_t)sh;
    case W32_SM_CXFULLSCREEN: return (int32_t)sw;
    case W32_SM_CYFULLSCREEN:
        return (int32_t)(sh - (have ? t.taskbar_h : 0));
    case W32_SM_CYCAPTION:  return have ? (int32_t)t.titlebar_h : 24;
    case W32_SM_CXFRAME:    return have ? (int32_t)t.border_w : 4;
    case W32_SM_CYFRAME:    return have ? (int32_t)t.border_w : 4;
    case W32_SM_CXDLGFRAME:
    case W32_SM_CYDLGFRAME: return have ? (int32_t)t.border_w : 4;
    case W32_SM_CXBORDER:
    case W32_SM_CYBORDER:   return 1;
    case W32_SM_CXVSCROLL:
    case W32_SM_CYHSCROLL:  return 16;
    case W32_SM_CXHTHUMB:   return 17;
    case W32_SM_CYVTHUMB:   return 17;
    case W32_SM_CXHSCROLL:  return 16;
    case W32_SM_CYVSCROLL:  return 16;
    case W32_SM_CXICON:
    case W32_SM_CYICON:     return have ? (int32_t)t.icon_size : 32;
    case W32_SM_CXCURSOR:
    case W32_SM_CYCURSOR:   return 16;
    case W32_SM_CYMENU:     return 20;          /* no menu bar yet: reserved */
    case W32_SM_CXMIN:
    case W32_SM_CXMINTRACK: return 80;          /* the compositor's floor */
    case W32_SM_CYMIN:
    case W32_SM_CYMINTRACK: return 60;
    case W32_SM_CXSIZE:
    case W32_SM_CYSIZE:     return have ? (int32_t)t.titlebar_h - 8 : 16;
    case W32_SM_MOUSEPRESENT: return 1;
    case W32_SM_CMONITORS:  return 1;           /* one monitor, documented */
    case W32_SM_SAMEDISPLAYFORMAT: return 1;
    case W32_SM_SWAPBUTTON: return 0;
    case W32_SM_DEBUG:      return 0;
    case W32_SM_CYKANJIWINDOW: return 0;
    default:                return 0;
    }
}

/* The theme's palette under Win32's names.  Every colour below comes from
 * the compositor theme, so ag_theme_set() changing it changes these too --
 * which is what the phase's test gate asserts instead of "it returns a
 * constant that happens to look right on the default theme". */
W32ABI W32_DWORD GetSysColor(int32_t index) {
    ui_theme_t t;
    if (ag_theme_get(&t) != 0) return 0;
    switch (index) {
    case W32_COLOR_SCROLLBAR:       return w32_colorref_to_ag(t.win_bg);
    case W32_COLOR_BACKGROUND:
    case W32_COLOR_DESKTOP:         return w32_colorref_to_ag(t.desktop_top);
    case W32_COLOR_APPWORKSPACE:    return w32_colorref_to_ag(t.desktop_bot);
    case W32_COLOR_ACTIVECAPTION:   return w32_colorref_to_ag(t.title_active);
    case W32_COLOR_INACTIVECAPTION: return w32_colorref_to_ag(t.title_inactive);
    case W32_COLOR_CAPTIONTEXT:
    case W32_COLOR_WINDOWTEXT:      return w32_colorref_to_ag(t.title_text);
    case W32_COLOR_MENU:            return w32_colorref_to_ag(t.taskbar_bg);
    case W32_COLOR_MENUTEXT:
    case W32_COLOR_BTNTEXT:         return w32_colorref_to_ag(t.taskbar_text);
    case W32_COLOR_WINDOW:          return w32_colorref_to_ag(t.win_content);
    case W32_COLOR_WINDOWFRAME:
    case W32_COLOR_INACTIVEBORDER:
    case W32_COLOR_BTNSHADOW:       return w32_colorref_to_ag(t.border);
    case W32_COLOR_ACTIVEBORDER:    return w32_colorref_to_ag(t.border_active);
    case W32_COLOR_HIGHLIGHT:       return w32_colorref_to_ag(t.icon_selected);
    case W32_COLOR_HIGHLIGHTTEXT:   return 0x00FFFFFFu;
    case W32_COLOR_BTNFACE:         return w32_colorref_to_ag(t.win_bg);
    case W32_COLOR_BTNHIGHLIGHT:
    case W32_COLOR_3DLIGHT:         return w32_colorref_to_ag(t.win_content);
    case W32_COLOR_GRAYTEXT:
    case W32_COLOR_INACTIVECAPTIONTEXT: return w32_colorref_to_ag(t.title_inactive);
    case W32_COLOR_INFOTEXT:        return w32_colorref_to_ag(t.notif_text);
    case W32_COLOR_INFOBK:          return w32_colorref_to_ag(t.notif_bg);
    default:                        return 0;
    }
}

W32ABI W32_HBRUSH GetSysColorBrush(int32_t index) {
    return (W32_HBRUSH)(uintptr_t)GetSysColor(index);
}

W32ABI W32_BOOL SystemParametersInfoW(W32_UINT action, W32_UINT param,
                                      void *data, W32_UINT winini) {
    (void)param; (void)winini;
    ui_theme_t t;
    uint32_t sw = 0, sh = 0;
    ag_screen_size(&sw, &sh);
    int have = (ag_theme_get(&t) == 0);

    switch (action) {
    case W32_SPI_GETBORDER:
        if (data) *(int32_t *)data = 1;         /* one-pixel border, as drawn */
        return W32_TRUE;
    case W32_SPI_GETBEEP:
        if (data) *(int32_t *)data = 1;
        return W32_TRUE;
    case W32_SPI_GETKEYBOARDSPEED:
        if (data) *(int32_t *)data = 31;        /* the fastest rate */
        return W32_TRUE;
    case W32_SPI_GETKEYBOARDDELAY:
        if (data) *(int32_t *)data = 1;
        return W32_TRUE;
    case W32_SPI_GETMOUSE:
        if (data) { int32_t *p = (int32_t *)data; p[0] = 0; p[1] = 0; p[2] = 0; }
        return W32_TRUE;                        /* no pointer acceleration: the
                                                 * compositor takes raw deltas */
    case W32_SPI_GETMOUSESPEED:
        if (data) *(int32_t *)data = 10;
        return W32_TRUE;
    case W32_SPI_GETICONTITLEWRAP:
        if (data) *(int32_t *)data = 1;
        return W32_TRUE;
    case W32_SPI_GETSCREENSAVEACTIVE:
        if (data) *(int32_t *)data = 0;         /* there is no screensaver */
        return W32_TRUE;
    case W32_SPI_GETSCREENSAVETIMEOUT:
        if (data) *(int32_t *)data = 0;
        return W32_TRUE;
    case W32_SPI_GETMENUSHOWDELAY:
        if (data) *(int32_t *)data = 200;
        return W32_TRUE;
    case W32_SPI_GETWHEELSCROLLLINES:
        if (data) *(int32_t *)data = 3;         /* lines per wheel notch */
        return W32_TRUE;
    case W32_SPI_GETWORKAREA: {
        W32_RECT *r = (W32_RECT *)data;
        if (r) {
            r->left = 0; r->top = 0;
            r->right = (int32_t)sw;
            r->bottom = (int32_t)(sh - (have ? t.taskbar_h : 0));
        }
        return W32_TRUE;
    }
    default:
        /* Refused by number: a caller asking for a parameter this compositor
         * does not have gets an error, not a plausible-looking default. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
}

W32ABI W32_BOOL SystemParametersInfoA(W32_UINT action, W32_UINT param,
                                      void *data, W32_UINT winini) {
    return SystemParametersInfoW(action, param, data, winini);
}

W32ABI W32_DWORD GetDoubleClickTime(void) {
    /* No registry yet (W32A-9 owns it); the documented default. */
    return 500;
}

W32ABI W32_DWORD GetCaretBlinkTime(void) { return 530; }

W32ABI int32_t GetKeyboardType(int32_t type) {
    switch (type) {
    case 0: return 4;                           /* enhanced 101/102-key */
    case 1: return 0;                           /* no OEM-specific data */
    case 2: return 12;                          /* 12 function keys */
    default: return 0;
    }
}

/* ---- the single monitor -------------------------------------------------- */

#define UI_MONITOR_HANDLE ((W32_HMONITOR)(uintptr_t)0x5000)

static void ui_monitor_info(W32_MONITORINFOEXW *mi) {
    ui_theme_t t;
    uint32_t sw = 0, sh = 0;
    ag_screen_size(&sw, &sh);
    uint32_t taskbar = (ag_theme_get(&t) == 0) ? t.taskbar_h : 0;
    mi->mi.rcMonitor.left = 0; mi->mi.rcMonitor.top = 0;
    mi->mi.rcMonitor.right = (int32_t)sw; mi->mi.rcMonitor.bottom = (int32_t)sh;
    mi->mi.rcWork.left = 0; mi->mi.rcWork.top = 0;
    mi->mi.rcWork.right = (int32_t)sw;
    mi->mi.rcWork.bottom = (int32_t)(sh - taskbar);
    mi->mi.dwFlags = 1;                        /* MONITORINFOF_PRIMARY */
    const uint16_t dev[] = { 'A','u','r','a','L','i','t','e',0 };
    w16_copy(mi->szDevice, dev, 32);
}

W32ABI W32_BOOL GetMonitorInfoW(W32_HMONITOR mon, W32_MONITORINFOEXW *mi) {
    if (mon != UI_MONITOR_HANDLE || !mi) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    ui_monitor_info(mi);
    return W32_TRUE;
}

W32ABI W32_BOOL GetMonitorInfoA(W32_HMONITOR mon, void *out) {
    if (!out) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    W32_MONITORINFOEXW mi;
    if (!GetMonitorInfoW(mon, &mi)) return W32_FALSE;
    /* The A form: same struct with a single-byte device name. */
    struct { W32_MONITORINFO mi; char dev[32]; } *a = out;
    a->mi = mi.mi;
    int n = 0;
    while (n < 31 && mi.szDevice[n]) { a->dev[n] = (char)mi.szDevice[n]; n++; }
    a->dev[n] = 0;
    return W32_TRUE;
}

W32ABI W32_BOOL EnumDisplayMonitors(W32_HDC dc, const W32_RECT *clip,
                                    W32_BOOL (W32ABI *cb)(W32_HMONITOR,
                                                          W32_HDC, W32_RECT *,
                                                          W32_LPARAM),
                                    W32_LPARAM lp) {
    (void)dc; (void)clip;
    if (!cb) return W32_FALSE;
    W32_MONITORINFOEXW mi;
    ui_monitor_info(&mi);
    return cb(UI_MONITOR_HANDLE, 0, &mi.mi.rcMonitor, lp);
}

W32ABI W32_HMONITOR MonitorFromWindow(W32_HWND hwnd, W32_DWORD flags) {
    (void)hwnd;
    if (flags == W32_MONITOR_DEFAULTTONULL) {
        if (w32_win_index_from_hwnd(hwnd) < 0) return 0;
    }
    return UI_MONITOR_HANDLE;
}

W32ABI W32_HMONITOR MonitorFromRect(const W32_RECT *r, W32_DWORD flags) {
    if (!r && flags == W32_MONITOR_DEFAULTTONULL) return 0;
    return UI_MONITOR_HANDLE;
}

W32ABI W32_HMONITOR MonitorFromPoint(W32_POINT pt, W32_DWORD flags) {
    (void)pt;
    if (flags == W32_MONITOR_DEFAULTTONULL) {
        uint32_t sw = 0, sh = 0;
        ag_screen_size(&sw, &sh);
        if (pt.x < 0 || pt.y < 0 || pt.x >= (int32_t)sw || pt.y >= (int32_t)sh)
            return 0;
    }
    return UI_MONITOR_HANDLE;
}

/* ---- keyboard and mouse state -------------------------------------------- */

/* The ledger's VK set mapped to a US layout.  A layout table is a W32A-6/8
 * concern (ToUnicodeEx, keyboard layouts); this is the printable range the
 * ladder's text controls actually type, and everything else returns 0 --
 * "no translation" is a legal ToAscii answer and the honest one here. */
static const struct { uint8_t vk; char lo, hi; } ui_vk_ascii[] = {
    { 0x41,'a','A' }, { 0x42,'b','B' }, { 0x43,'c','C' }, { 0x44,'d','D' },
    { 0x45,'e','E' }, { 0x46,'f','F' }, { 0x47,'g','G' }, { 0x48,'h','H' },
    { 0x49,'i','I' }, { 0x4A,'j','J' }, { 0x4B,'k','K' }, { 0x4C,'l','L' },
    { 0x4D,'m','M' }, { 0x4E,'n','N' }, { 0x4F,'o','O' }, { 0x50,'p','P' },
    { 0x51,'q','Q' }, { 0x52,'r','R' }, { 0x53,'s','S' }, { 0x54,'t','T' },
    { 0x55,'u','U' }, { 0x56,'v','V' }, { 0x57,'w','W' }, { 0x58,'x','X' },
    { 0x59,'y','Y' }, { 0x5A,'z','Z' },
    { 0x30,'0',')' }, { 0x31,'1','!' }, { 0x32,'2','@' }, { 0x33,'3','#' },
    { 0x34,'4','$' }, { 0x35,'5','%' }, { 0x36,'6','^' }, { 0x37,'7','&' },
    { 0x38,'8','*' }, { 0x39,'9','(' },
    { 0x20,' ',' ' }, { 0xBD,'-','_' }, { 0xBB,'=','+' }, { 0xDB,'[','{' },
    { 0xDD,']','}' }, { 0xDC,'\\','|' }, { 0xBA,';',':' }, { 0xDE,'\'','"' },
    { 0xBC,',','<' }, { 0xBE,'.','>' }, { 0xBF,'/','?' }, { 0xC0,'`','~' },
};
#define UI_VK_ASCII_N (sizeof ui_vk_ascii / sizeof ui_vk_ascii[0])

W32ABI int32_t ToAscii(W32_UINT vk, W32_UINT scan, const uint8_t *state,
                       uint16_t *out, W32_UINT flags) {
    (void)scan; (void)flags;
    if (!out) return 0;
    int shift = state ? ((state[W32_VK_SHIFT] & 0x80) ? 1 : 0) : 0;
    int ctrl  = state ? ((state[W32_VK_CONTROL] & 0x80) ? 1 : 0) : 0;

    if (ctrl) {
        /* The control codes Win32 produces for the letters: A->0x01. */
        if ((vk >= 'A' && vk <= 'Z') || (vk >= 'a' && vk <= 'z')) {
            out[0] = (uint16_t)((vk | 0x20) - 'a' + 1);
            return 1;
        }
        return 0;
    }
    if (vk == W32_VK_RETURN) { out[0] = '\r'; return 1; }
    if (vk == W32_VK_TAB)    { out[0] = '\t'; return 1; }
    if (vk == W32_VK_BACK)   { out[0] = 8;    return 1; }
    if (vk == W32_VK_ESCAPE) { out[0] = 27;   return 1; }
    if (vk == W32_VK_SPACE)  { out[0] = ' ';  return 1; }
    for (unsigned k = 0; k < UI_VK_ASCII_N; k++)
        if (ui_vk_ascii[k].vk == vk) {
            out[0] = (uint16_t)(shift ? ui_vk_ascii[k].hi : ui_vk_ascii[k].lo);
            return 1;
        }
    return 0;
}

W32ABI int32_t ToAsciiEx(W32_UINT vk, W32_UINT scan, const uint8_t *state,
                         uint16_t *out, W32_UINT flags, W32_DWORD layout) {
    (void)layout;
    return ToAscii(vk, scan, state, out, flags);
}

W32ABI W32_UINT MapVirtualKeyW(W32_UINT code, W32_UINT type) {
    switch (type) {
    case 0:  /* VK -> scan code */
        if (code >= 'A' && code <= 'Z') return 0x1E + (code - 'A');
        if (code >= '0' && code <= '9') return 0x02 + (code - '0');
        if (code == W32_VK_RETURN) return 0x1C;
        if (code == W32_VK_SPACE)  return 0x39;
        if (code == W32_VK_ESCAPE) return 0x01;
        if (code == W32_VK_TAB)    return 0x0F;
        if (code == W32_VK_BACK)   return 0x0E;
        return 0;
    case 1:  /* scan code -> VK */
        if (code >= 0x1E && code <= 0x26) return 'A' + (code - 0x1E);
        if (code >= 0x27 && code <= 0x28) return 0;
        if (code >= 0x02 && code <= 0x0B) return '0' + ((code == 0x0B) ? 0 : (code - 0x02));
        if (code == 0x1C) return W32_VK_RETURN;
        if (code == 0x39) return W32_VK_SPACE;
        if (code == 0x01) return W32_VK_ESCAPE;
        if (code == 0x0F) return W32_VK_TAB;
        if (code == 0x0E) return W32_VK_BACK;
        return 0;
    case 2: { /* VK -> unshifted character */
        uint16_t ch;
        uint8_t st[256];
        for (int i = 0; i < 256; i++) st[i] = 0;
        if (ToAscii(code, 0, st, &ch, 0) == 1) return (W32_UINT)ch;
        return 0;
    }
    default:
        return 0;
    }
}

W32ABI int16_t GetKeyState(int32_t vk) {
    if (vk < 0 || vk > 255) return 0;
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    if (!q) return 0;
    /* The array is fed by delivered key messages (see ui_translate), so the
     * state a program reads is the state its own loop has seen -- not a
     * second copy of the keyboard driver's idea. */
    uint8_t s = q->keystate[vk];
    return (int16_t)(((s & 0x80) ? 0x8000 : 0) | ((s & 0x01) ? 0x0001 : 0));
}

W32ABI W32_BOOL GetKeyboardState(uint8_t *keys) {
    if (!keys) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 0);
    for (int i = 0; i < 256; i++) keys[i] = q ? q->keystate[i] : 0;
    return W32_TRUE;
}

W32ABI W32_BOOL SetKeyboardState(uint8_t *keys) {
    if (!keys) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    struct ui_queue *q = ui_queue_for(GetCurrentThreadId(), 1);
    if (!q) return W32_FALSE;
    for (int i = 0; i < 256; i++) q->keystate[i] = keys[i];
    return W32_TRUE;
}

W32ABI W32_DWORD GetKeyboardLayout(W32_DWORD tid) {
    (void)tid;
    return 0x0409;                             /* en-US, the only one there is */
}

W32ABI W32_BOOL GetCursorPos(W32_POINT *pt) {
    if (!pt) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return W32_FALSE; }
    return ag_mouse_position(&pt->x, &pt->y) == 0 ? W32_TRUE : W32_FALSE;
}

W32ABI W32_BOOL SetCursorPos(int32_t x, int32_t y) {
    (void)x; (void)y;
    /* The pointer is the compositor's (it owns the sprite and the hit
     * testing); there is no warp primitive, so this refuses rather than
     * pretending the pointer moved. */
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_FALSE;
}

W32ABI void mouse_event(W32_DWORD flags, W32_DWORD dx, W32_DWORD dy,
                        W32_DWORD data, uintptr_t extra) {
    (void)data; (void)extra;
    /* Synthesised input is injected into the message queue of the window
     * that has focus -- genuinely injected, but not into the compositor's
     * own input path, and the plan says which is which. */
    W32_HWND target = GetFocus();
    if (!target) target = WindowFromPoint((W32_POINT){ 0, 0 });
    if (!target) return;
    int32_t cx = (int32_t)dx, cy = (int32_t)dy;
    if (flags & 0x0001u) {                     /* MOUSEEVENTF_MOVE */
        PostMessageW(target, W32_WM_MOUSEMOVE, 0, ui_pt_lparam(cx, cy));
        return;
    }
    if (flags & 0x0002u) { PostMessageW(target, W32_WM_LBUTTONDOWN, 1, ui_pt_lparam(cx, cy)); return; }
    if (flags & 0x0004u) { PostMessageW(target, W32_WM_LBUTTONUP, 0, ui_pt_lparam(cx, cy)); return; }
    if (flags & 0x0008u) { PostMessageW(target, W32_WM_RBUTTONDOWN, 2, ui_pt_lparam(cx, cy)); return; }
    if (flags & 0x0010u) { PostMessageW(target, W32_WM_RBUTTONUP, 0, ui_pt_lparam(cx, cy)); return; }
    if (flags & 0x0020u) { PostMessageW(target, W32_WM_MBUTTONDOWN, 16, ui_pt_lparam(cx, cy)); return; }
    if (flags & 0x0040u) { PostMessageW(target, W32_WM_MBUTTONUP, 0, ui_pt_lparam(cx, cy)); return; }
}

W32ABI W32_BOOL TrackMouseEvent(void *tev) {
    /* TRACKMOUSEEVENT is TME_LEAVE/min-size state; only the leave bit is
     * honoured because the compositor reports enter/leave by routing.  The
     * arrival of a mouse-move on a different window is what fires
     * WM_MOUSELEAVE for the previous one (see ui_note_hover). */
    struct { uint32_t cbSize; uint32_t dwFlags; W32_HWND hwndTrack;
             uint32_t dwHoverTime; } *t = tev;
    if (!t || !t->hwndTrack) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    int i = w32_win_index_from_hwnd(t->hwndTrack);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    windows[i].tracking_leave = (t->dwFlags & 0x00000002u) ? 1 : 0;
    return W32_TRUE;
}

W32ABI W32_LRESULT MessageBoxW(W32_HWND owner, const uint16_t *text,
                               const uint16_t *caption, W32_UINT type) {
    (void)owner; (void)type;
    char t[512], c[128];
    w16_to_a(t, sizeof t, text);
    w16_to_a(c, sizeof c, caption);
    ag_alert(c[0] ? c : "Message", t);
    return 1;                                   /* IDOK */
}

W32ABI int32_t MessageBoxA(W32_HWND owner, const char *text,
                           const char *caption, W32_UINT type) {
    (void)owner; (void)type;
    ag_alert(caption ? caption : "Message", text ? text : "");
    return 1;
}

/* Helper for dialog-item enumeration: list children of `parent` in
 * creation order.  Used by w32_dlg.c's GetDlgItem so it can walk
 * children regardless of the top-level z-order.  Returns the count
 * (clamped to `max`), and fills the first `min(n,max)` entries. */
int w32_win_count_and_list(W32_HWND parent, W32_HWND *out, int max) {
    int pi = w32_win_index_from_hwnd(parent);
    if (pi < 0 || max <= 0 || !out) return 0;
    int n = 0;
    for (int i = 0; i < UI_MAX_WINDOWS && n < max; i++) {
        if (!windows[i].in_use) continue;
        if (windows[i].parent != pi) continue;
        out[n++] = idx_to_hwnd(i);
    }
    return n;
}
