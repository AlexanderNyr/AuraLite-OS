/* comctl32.c — W32APP_PLAN.md phase W32A-8: the common controls.
 *
 * The 27 ladder-measured COMCTL32 symbols, all REAL (decision D1: the
 * 21 named imports plus ordinals 17/381/410-413; w32/ordinal_map.tsv).
 * On top of the imports sit the windowed controls, which cost no import
 * at all: applications create them with CreateWindowExW on the WC_*
 * class names InitCommonControlsEx registers and drive them through the
 * W32A-5 SendMessage path, with WM_NOTIFY coming back through an NMHDR
 * exactly as documented.
 *
 * How each control is backed (decision D5, mapped not faked):
 *   toolbar   self-drawn: GDI rectangles + text over the A-7 window DC,
 *             buttons from a per-control table, clicks raise WM_COMMAND
 *   status    self-drawn: SB_SETPARTS rectangles + per-part text
 *   listview  ag_add_listbox widget renders the rows; the control owns
 *             the LVM_* semantics (items, state, columns, selection)
 *   treeview  the new ag_add_tree widget (this phase adds it to
 *             libauragui: no tree widget existed, and mapping a tree
 *             onto a listbox is the D5 violation the plan names)
 *   tab       ag_add_tab, extended with set-active/remove helpers
 *   tooltip   self-drawn tip window; TTM_RELAYEVENT + the A-6 timer
 *             pump drive show/hide, so a scripted hover really shows it
 *   progress  ag_add_progress, extended with a range/pos setter
 *   header    self-drawn: the column strip above a report listview
 *   propsheet PropertySheetW hosts each PROPSHEETPAGE as a real window
 *             (the A-6 dialog-engine trampoline pattern), with the tab
 *             strip as a real tab control child and PSN_* notifications
 *             with the documented lParam direction (TRUE = OK/close,
 *             FALSE = Apply)
 *
 * Version behaviour (the W32A-1 manifest record): w32run records the
 * SxS dependency as comctl v6 or v5 before the image starts.  v6
 * controls paint through the live compositor theme (the palette below
 * reads ag_theme_get); v5 controls paint the classic syscolor look.
 * The integration gate asserts the two renderings differ where the
 * theme engine draws, so the selection is honoured, not ignored.  The
 * palette IS the seam W32A-11 will replace with real uxtheme.dll
 * OpenThemeData/DrawThemeBackground calls: same state (class + part),
 * mechanical swap.
 *
 * WS_CHILD: user32_win.c admits it for exactly the classes registered
 * here (w32_win_register_comctl_class marks them); every other class
 * keeps the documented refusal.  Child coordinates are parent-relative
 * and translated at creation, so a control lands inside its parent on
 * screen; the compositor composites each control as its own window
 * (moving the parent does not move children -- recorded in
 * user32_win.c, not hidden here).
 *
 * Refusals are named (D9): toolbar customisation (TB_CUSTOMIZE) and
 * the rebars/bands the ledger never showed refuse with
 * ERROR_CALL_NOT_IMPLEMENTED; bad handles refuse with
 * ERROR_INVALID_HANDLE; nothing fails silently.
 */

#include "w32/w32_abi.h"
#include "w32/user32.h"
#include "w32/gdi32.h"
#include "w32/comctl32.h"
#include "w32/kernel32.h"
#include "w32/w32_module.h"
#include "w32/w32_rsrc.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef W32_ARRAY_COUNT
#define W32_ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))
#endif

#ifndef AURALITE_W32_HOST_TEST
#include "auragui.h"
#include <stdio.h>
#else
/* The host suite has no compositor and no auragui types.  The widget
 * surface this file drives is mirrored as int-handle hooks the test
 * supplies (create/row/active/sel/progress/render/dispatch); the theme
 * palette arrives through one hook, exactly the w32_gdi host-DPI
 * pattern.  No auragui struct is ever re-declared here, so the
 * amalgamated test TU cannot grow conflicting declarations. */
extern int  w32_comctl_host_wdg_create(int kind, int32_t x, int32_t y,
                                       uint32_t w, uint32_t h);
extern void w32_comctl_host_wdg_destroy(int wdg);
extern void w32_comctl_host_wdg_clear(int wdg);
extern int  w32_comctl_host_wdg_add_row(int wdg, int parent, const char *label);
extern void w32_comctl_host_wdg_remove_row(int wdg, int idx);
extern void w32_comctl_host_wdg_set_active(int wdg, int idx);
extern int  w32_comctl_host_wdg_active(int wdg);
extern void w32_comctl_host_wdg_set_sel(int wdg, int idx);
extern int  w32_comctl_host_wdg_sel(int wdg);
extern void w32_comctl_host_wdg_set_progress(int wdg, int value, int max);
extern int  w32_comctl_host_wdg_progress(int wdg);
extern void w32_comctl_host_wdg_set_expanded(int wdg, int node, int on);
extern int  w32_comctl_host_wdg_expanded(int wdg, int node);
extern int  w32_comctl_host_wdg_visible_rows(int wdg);
extern int  w32_comctl_host_wdg_row_node(int wdg, int row);
extern int  w32_comctl_host_wdg_node_row(int wdg, int node);
extern int  w32_comctl_host_wdg_row_count(int wdg);
extern void w32_comctl_host_wdg_render(int wdg, int wid);
extern void w32_comctl_host_wdg_dispatch(int wdg, int type, int32_t x, int32_t y);
extern void w32_comctl_host_palette(w32_comctl_palette_t *out);
#endif

/* The dialog engine keeps this one local too (user32.h spells it
 * GWL_ID); the pointer-width read wants the same index. */
#define W32_GWLP_ID (-12)

/* ---- user32/gdi internals this file rides on ------------------------- */

extern W32_WORD   w32_win_register_comctl_class(const W32_WNDCLASSEXW *c);
extern W32_BOOL   W32ABI TrackMouseEvent(void *tev);
extern W32_HICON  w32_gdi_icon_from_argb(int32_t w, int32_t hgt, const uint32_t *argb);
extern W32_HICON  w32_gdi_icon_decode(const uint8_t *bytes, size_t len);
extern int        w32_win_cls_ag_wid(W32_HWND hwnd);   /* user32_win.c */
extern int        w32_win_index_from_hwnd(W32_HWND h);

/* ===================================================================== *
 * Version record + the theme palette (the W32A-11 seam)
 * ===================================================================== */

static int comctl_version = 5;

void w32_comctl_set_version(int major) { comctl_version = major; }
int  w32_comctl_version(void)          { return comctl_version; }

/* Classic syscolors (the v5 look): the documented face/text/frame
 * triples every Win32 programmer draws first.  Fixed by contract. */
static const w32_comctl_palette_t classic_palette = {
    0x00C0C0C0u,   /* face  BTNFACE      */
    0x00000000u,   /* text  BTNTEXT      */
    0x00808080u,   /* frame BTNSHADOW    */
    0x00C0C0C0u,   /* hot   (flat: same) */
    0x00000080u,   /* sel   HIGHLIGHT    */
};

static w32_comctl_palette_t live_palette;      /* rebuilt per call */
static int palette_valid;

const w32_comctl_palette_t *w32_comctl_palette(void) {
    if (comctl_version < 6) return &classic_palette;
    if (!palette_valid) {
#ifdef AURALITE_W32_HOST_TEST
        live_palette = classic_palette;        /* test overrides fields */
        w32_comctl_host_palette(&live_palette);
#else
        ag_theme_t t;
        if (!ag_theme_get(&t)) return &classic_palette;
        live_palette.face  = t.win_content;
        live_palette.text  = 0x00000000u;
        live_palette.frame = t.border;
        live_palette.hot   = t.title_active;   /* the gtheme accent   */
        live_palette.sel   = t.title_active;
#endif
        palette_valid = 1;
    }
    return &live_palette;
}

/* 0x00RRGGBB (the palette + libauragui convention) -> COLORREF. */
static uint32_t ag_to_colorref(uint32_t ag) {
    return (ag & 0xFFu) | ((ag >> 8) & 0xFFu) << 8 | ((ag >> 16) & 0xFFu) << 16;
}

/* ===================================================================== *
 * Small helpers
 * ===================================================================== */

static int32_t lp_x(W32_LPARAM lp) { return (int32_t)(int16_t)(uint16_t)(uint32_t)lp; }
static int32_t lp_y(W32_LPARAM lp) { return (int32_t)(int16_t)(uint16_t)((uint64_t)lp >> 16); }

/* Bounded W-string copy. */
static void memcpy_w16(uint16_t *dst, const uint16_t *src, size_t cap) {
    size_t n = 0;
    if (!dst || !cap) return;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

/* Bounded copy (the widget seams below never see a format string). */
static void ctl_strlcpy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || !cap) return;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

/* Copy a W string into a char buffer (labels are ASCII in this
 * personality's fonts; the walk is the standard w16_to_a). */
static void a_to_w16_buf(uint16_t *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!dst || !cap) return;
    while (src && src[n] && n + 1 < cap) { dst[n] = (uint16_t)(uint8_t)src[n]; n++; }
    dst[n] = 0;
}

/* The control id a window carries (mirrors GWLP_ID; windows created
 * with a menu handle hold it there). */
int32_t w32_comctl_ctrl_id(W32_HWND hwnd) {
    return (int32_t)GetWindowLongPtrW(hwnd, W32_GWLP_ID);
}

/* One WM_NOTIFY to the parent, synchronously, with an NMHDR the parent
 * can extend.  Returns the parent's reply. */
static W32_LRESULT notify_parent(W32_HWND ctl, uint32_t code, void *payload) {
    W32_NMHDR *nm = payload;
    W32_HWND parent = GetParent(ctl);
    if (!parent) return 0;
    if (nm) {
        nm->hwndFrom = ctl;
        nm->idFrom   = (uint64_t)(int64_t)w32_comctl_ctrl_id(ctl);
        nm->code     = code;
    }
    return SendMessageW(parent, W32_WM_NOTIFY,
                        (W32_WPARAM)(uint64_t)(int64_t)w32_comctl_ctrl_id(ctl),
                        (W32_LPARAM)(intptr_t)payload);
}

/* ===================================================================== *
 * The widget seam (guest: libauragui; host: the hooks above)
 * ===================================================================== *
 * Every widget-backed control goes through these wrappers, so the
 * host gate exercises the same call sequence the guest makes without
 * ever naming an auragui type. */

#define CTLK_TAB      0
#define CTLK_PROGRESS 1
#define CTLK_TREE     2
#define CTLK_LISTBOX  3

/* Input event types, numerically identical to auragui's AG_EVT_* so the
 * guest path passes them straight through (statically checked) and the
 * host hooks see the same numbers the fake compositor would post. */
#define CTL_EVT_DOWN     2   /* == AG_EVT_MOUSE_DOWN    */
#define CTL_EVT_DBLCLICK 4   /* == AG_EVT_MOUSE_DBLCLICK */
#define CTL_EVT_MOVE     1   /* == AG_EVT_MOUSE_MOVE    */
#ifndef AURALITE_W32_HOST_TEST
_Static_assert(CTL_EVT_DOWN == AG_EVT_MOUSE_DOWN &&
               CTL_EVT_DBLCLICK == AG_EVT_MOUSE_DBLCLICK &&
               CTL_EVT_MOVE == AG_EVT_MOUSE_MOVE,
               "event seam must mirror auragui's numbering");
#endif

typedef struct {
#ifdef AURALITE_W32_HOST_TEST
    int wdg;                       /* hook handle, -1 = none */
#else
    ag_view_t   view;
    ag_widget_t wbuf[4];
    ag_widget_t *widget;
#endif
} ctl_surface_t;

static void ctl_surface_init(ctl_surface_t *s, int wid, int kind,
                             int32_t x, int32_t y, uint32_t w, uint32_t h) {
    (void)wid;                       /* only the guest path needs it */
    memset(s, 0, sizeof *s);
#ifdef AURALITE_W32_HOST_TEST
    s->wdg = w32_comctl_host_wdg_create(kind, x, y, w, h);
#else
    ag_view_init(&s->view, wid, s->wbuf, W32_ARRAY_COUNT(s->wbuf), 0xFFFFFFFFu);
    switch (kind) {
    case CTLK_TAB:
        s->widget = ag_add_tab(&s->view, x, y, w, h);
        break;
    case CTLK_PROGRESS:
        s->widget = ag_add_progress(&s->view, x, y, w, 1, 0);
        break;
    case CTLK_TREE:
        s->widget = ag_add_tree(&s->view, x, y, w, h);
        break;
    default:
        s->widget = ag_add_listbox(&s->view, x, y, w, h);
        break;
    }
#endif
}

static void ctl_surface_free(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    if (s->wdg >= 0) w32_comctl_host_wdg_destroy(s->wdg);
    s->wdg = -1;
#else
    (void)s;
#endif
}

static void ctl_surf_clear(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_clear(s->wdg);
#else
    if (s->widget && s->widget->kind == AG_W_LISTBOX) ag_listbox_clear(s->widget);
    if (s->widget && s->widget->kind == AG_W_TREE)   ag_tree_clear(s->widget);
#endif
}

static int ctl_surf_add_row(ctl_surface_t *s, int parent, const char *label) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_add_row(s->wdg, parent, label);
#else
    if (s->widget->kind == AG_W_TREE)   return ag_tree_add(s->widget, parent, label);
    if (s->widget->kind == AG_W_TAB)    return ag_tab_add(s->widget, label);
    if (s->widget->kind == AG_W_LISTBOX) {
        int n = s->widget->item_count;
        if (ag_listbox_add(s->widget, label) < 0) return -1;
        return n;
    }
    return -1;
#endif
}


static void ctl_surf_set_active(ctl_surface_t *s, int idx) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_set_active(s->wdg, idx);
#else
    if (s->widget->kind == AG_W_TAB) ag_tab_set_active(s->widget, idx);
#endif
}

static int ctl_surf_active(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_active(s->wdg);
#else
    return s->widget && s->widget->kind == AG_W_TAB ? s->widget->active_tab : -1;
#endif
}

static void ctl_surf_set_sel(ctl_surface_t *s, int idx) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_set_sel(s->wdg, idx);
#else
    if (s->widget && s->widget->kind == AG_W_TREE) s->widget->tree_sel = idx;
    else if (s->widget) s->widget->selected = idx;
#endif
}

static int ctl_surf_sel(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_sel(s->wdg);
#else
    if (!s->widget) return -1;
    if (s->widget->kind == AG_W_TREE) return s->widget->tree_sel;
    return s->widget->selected;
#endif
}

static void ctl_surf_set_progress(ctl_surface_t *s, int value, int max) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_set_progress(s->wdg, value, max);
#else
    if (s->widget && s->widget->kind == AG_W_PROGRESS)
        ag_progress_set(s->widget, value, max);
#endif
}

static int ctl_surf_progress(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_progress(s->wdg);
#else
    return s->widget && s->widget->kind == AG_W_PROGRESS ? s->widget->value : 0;
#endif
}

static void ctl_surf_set_expanded(ctl_surface_t *s, int node, int on) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_set_expanded(s->wdg, node, on);
#else
    if (s->widget && s->widget->kind == AG_W_TREE)
        ag_tree_set_expanded(s->widget, node, on);
#endif
}

static void ctl_surf_render(ctl_surface_t *s, int wid) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_render(s->wdg, wid);
#else
    (void)wid;
    ag_view_render(&s->view);
#endif
}

/* One input event into the widget (the control translated WM_* to this
 * point already); returns 1 like ag_view_dispatch's CLOSE_REQ contract
 * (never true for these widgets, but the seam keeps the shape). */
static int ctl_surf_dispatch(ctl_surface_t *s, int type, int32_t x, int32_t y) {
#ifdef AURALITE_W32_HOST_TEST
    w32_comctl_host_wdg_dispatch(s->wdg, type, x, y);
    return 0;
#else
    ag_event_t e;
    memset(&e, 0, sizeof e);
    e.type = (uint32_t)type;
    e.x = x; e.y = y;
    return ag_view_dispatch(&s->view, &e);
#endif
}

/* ===================================================================== *
 * Control classes
 * ===================================================================== */

#define CTL_KIND_TOOLBAR  1
#define CTL_KIND_STATUS   2
#define CTL_KIND_LISTVIEW 3
#define CTL_KIND_TREEVIEW 4
#define CTL_KIND_TAB      5
#define CTL_KIND_TOOLTIP  6
#define CTL_KIND_PROGRESS 7
#define CTL_KIND_HEADER   8
#define CTL_KIND_PSPAGE   9     /* internal: PropertySheet page host */

static W32_LRESULT ctl_generic_proc(W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);

/* The property-sheet page host: an internal class (WS_CHILD-able, the
 * A-6 dialog trampoline pattern); never in a documented ICC mask. */
static const uint16_t ps_page_cls[] = { 'A','u','r','a','P','S','P','a','g','e',0 };

/* ===================================================================== *
 * Per-control state
 * ===================================================================== */

#define TB_MAX_BUTTONS  32
#define SB_MAX_PARTS    8
#define LV_MAX_ITEMS    64
#define LV_MAX_COLUMNS  8
#define TV_MAX_ITEMS    64
#define TT_MAX_TOOLS    8
#define HD_MAX_ITEMS    16
#define CTL_MAX_WINDOWS 40

typedef struct {
    int32_t  bitmap;      /* image-list index, -1 = none     */
    int32_t  cmd;         /* WM_COMMAND id                   */
    uint32_t state;       /* W32_TBSTATE_*                   */
    uint32_t style;       /* W32_TBSTYLE_*                   */
} tb_button_t;

typedef struct {
    int       used;
    W32_HWND  hwnd;
    int       kind;
    ctl_surface_t surf;         /* tab/progress/tree/listview      */
    union {
        struct {                          /* toolbar */
            tb_button_t btn[TB_MAX_BUTTONS];
            char     text[TB_MAX_BUTTONS][24];  /* copied label storage */
            int n_btn;
            W32_HIMAGELIST himl;
            int32_t btn_w, btn_h;
        } tb;
        struct {                          /* status bar */
            int32_t  edge[SB_MAX_PARTS];  /* right edge, client coords  */
            char     text[SB_MAX_PARTS][64];
            uint32_t op[SB_MAX_PARTS];    /* the SBT_* byte             */
            int n_parts;
            int simple;
        } sb;
        struct {                          /* listview */
            char     text[LV_MAX_ITEMS][48];   /* widget label storage */
            uint32_t state[LV_MAX_ITEMS];
            int64_t  lparam[LV_MAX_ITEMS];
            int n_items;
            int sel_mark;
            char     col_text[LV_MAX_COLUMNS][32];
            int32_t  col_cx[LV_MAX_COLUMNS];
            int n_cols;
        } lv;
        struct {                          /* treeview: item i mirrors
                                            widget node i, 1:1; array
                                            order == ag_tree_add call
                                            order, so a rebuild replays
                                            0..n-1 and identities hold */
            char     text[TV_MAX_ITEMS][48];   /* widget label storage */
            int      parent[TV_MAX_ITEMS];
            int64_t  lparam[TV_MAX_ITEMS];
            uint8_t  expanded[TV_MAX_ITEMS];
            int      n_items;
            int      caret;               /* selected item, -1 none */
        } tv;
        struct {                          /* tab: rows render in the
                                            widget; label/lparam/count/
                                            active mirrored here */
            char    text[8][32];
            int64_t lparam[8];
            int     n;
            int     active;
        } tc;
        struct {                          /* tooltip */
            struct {
                W32_HWND hwnd;
                uint64_t uId;
                W32_RECT rect;
                uint16_t text[64];
                int in_use;
            } tool[TT_MAX_TOOLS];
            int active;                   /* TTM_ACTIVATE flag        */
            int current;                  /* tool index shown, -1     */
            int shown;
        } tt;
        struct {                          /* header */
            char     text[HD_MAX_ITEMS][32];
            int32_t  cx[HD_MAX_ITEMS];
            int32_t  fmt[HD_MAX_ITEMS];
            int64_t  lparam[HD_MAX_ITEMS];
            int n_items;
        } hd;
        struct {                          /* progress: range/step
                                            (value lives in the widget) */
            int32_t min, max, step;
        } pb;
        struct {                          /* propsheet page host */
            int sheet;                    /* sheet table index   */
            int page;                     /* page index in sheet */
        } pg;
    } u;
} ctl_state_t;

static ctl_state_t ctls[CTL_MAX_WINDOWS];

static ctl_state_t *ctl_of(W32_HWND hwnd) {
    for (int i = 0; i < CTL_MAX_WINDOWS; i++)
        if (ctls[i].used && ctls[i].hwnd == hwnd) return &ctls[i];
    return 0;
}

static ctl_state_t *ctl_alloc(W32_HWND hwnd, int kind) {
    ctl_state_t *c = ctl_of(hwnd);
    if (c) return c;                      /* NCCREATE twice: keep one */
    for (int i = 0; i < CTL_MAX_WINDOWS; i++) {
        if (!ctls[i].used) {
            memset(&ctls[i], 0, sizeof ctls[i]);
            ctls[i].used = 1;
            ctls[i].hwnd = hwnd;
            ctls[i].kind = kind;
            if (kind == CTL_KIND_TAB) ctls[i].u.tc.active = -1;
            /* Widget-backed controls get their surface at install time:
             * the ag window exists (CreateWindowExW made it before the
             * first message) and the geometry is final. */
            if (kind == CTL_KIND_TAB || kind == CTL_KIND_PROGRESS ||
                kind == CTL_KIND_TREEVIEW || kind == CTL_KIND_LISTVIEW) {
                W32_RECT r;
                memset(&r, 0, sizeof r);
                GetClientRect(hwnd, &r);
                uint32_t w = (uint32_t)(r.right - r.left);
                uint32_t h = (uint32_t)(r.bottom - r.top);
                int wk = CTLK_LISTBOX;
                if (kind == CTL_KIND_TAB) wk = CTLK_TAB;
                if (kind == CTL_KIND_PROGRESS) wk = CTLK_PROGRESS;
                if (kind == CTL_KIND_TREEVIEW) wk = CTLK_TREE;
                ctl_surface_init(&ctls[i].surf, w32_win_cls_ag_wid(hwnd),
                                 wk, 0, 0, w, h);
            }
            return &ctls[i];
        }
    }
    return 0;
}

static void ctl_free(W32_HWND hwnd) {
    ctl_state_t *c = ctl_of(hwnd);
    if (!c) return;
    ctl_surface_free(&c->surf);
    memset(c, 0, sizeof *c);
}

void w32_comctl_window_destroyed(W32_HWND hwnd) { ctl_free(hwnd); }

/* ===================================================================== *
 * The per-class procs: identity by proc, dispatch by state
 * ===================================================================== */
/* One registered proc per class: the proc a message arrives through IS
 * the control's identity (no name matching, no lookup races).  The
 * per-kind handlers below are plain internal functions. */
#define CTL_CLASS_PROC(id)                                                     \
    static W32_LRESULT W32ABI ctl_proc_##id(W32_HWND h, W32_UINT m,           \
                                            W32_WPARAM w, W32_LPARAM l) {     \
        if (m == W32_WM_NCCREATE) ctl_alloc(h, CTL_KIND_##id);                 \
        return ctl_generic_proc(h, m, w, l);                                   \
    }
CTL_CLASS_PROC(TOOLBAR)
CTL_CLASS_PROC(STATUS)
CTL_CLASS_PROC(LISTVIEW)
CTL_CLASS_PROC(TREEVIEW)
CTL_CLASS_PROC(TAB)
CTL_CLASS_PROC(TOOLTIP)
CTL_CLASS_PROC(PROGRESS)
CTL_CLASS_PROC(HEADER)
CTL_CLASS_PROC(PSPAGE)

/* The documented class names, as UTF-16 (see the header note: L""
 * literals are 4-byte wchar here and would truncate). */
const uint16_t W32_WCN_TOOLBAR[]   = {'T','o','o','l','b','a','r','W','i','n','d','o','w','3','2',0};
const uint16_t W32_WCN_STATUSBAR[] = {'m','s','c','t','l','s','_','s','t','a','t','u','s','b','a','r','3','2',0};
const uint16_t W32_WCN_LISTVIEW[]  = {'S','y','s','L','i','s','t','V','i','e','w','3','2',0};
const uint16_t W32_WCN_TREEVIEW[]  = {'S','y','s','T','r','e','e','V','i','e','w','3','2',0};
const uint16_t W32_WCN_TABCONTROL[] = {'S','y','s','T','a','b','C','o','n','t','r','o','l','3','2',0};
const uint16_t W32_WCN_TOOLTIP[]   = {'t','o','o','l','t','i','p','s','_','c','l','a','s','s','3','2',0};
const uint16_t W32_WCN_PROGRESS[]  = {'m','s','c','t','l','s','_','p','r','o','g','r','e','s','s','3','2',0};
const uint16_t W32_WCN_HEADER[]    = {'S','y','s','H','e','a','d','e','r','3','2',0};

static const struct {
    const uint16_t *name;
    W32_WNDPROC     proc;
    uint32_t        icc;      /* the ICC_ bit that documents the class */
} ctl_classes[] = {
    { W32_WC_TOOLBARW,    ctl_proc_TOOLBAR,  W32_ICC_BAR_CLASSES },
    { W32_WC_STATUSBARW,  ctl_proc_STATUS,   W32_ICC_BAR_CLASSES },
    { W32_WC_LISTVIEWW,   ctl_proc_LISTVIEW, W32_ICC_LISTVIEW_CLASSES },
    { W32_WC_TREEVIEWW,   ctl_proc_TREEVIEW, W32_ICC_TREEVIEW_CLASSES },
    { W32_WC_TABCONTROLW, ctl_proc_TAB,      W32_ICC_TAB_CLASSES },
    { W32_WC_TOOLTIPW,    ctl_proc_TOOLTIP,  0 },  /* no ICC bit */
    { W32_WC_PROGRESSW,   ctl_proc_PROGRESS, W32_ICC_PROGRESS_CLASS },
    { W32_WC_HEADERW,     ctl_proc_HEADER,   W32_ICC_LISTVIEW_CLASSES },
};



/* ===================================================================== *
 * The generic control proc: install, kind dispatch, teardown
 * ===================================================================== */

static W32_LRESULT ctl_toolbar_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_status_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_listview_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_treeview_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_tab_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_tooltip_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_progress_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_header_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);
static W32_LRESULT ctl_pspage_proc(ctl_state_t *, W32_HWND, W32_UINT, W32_WPARAM, W32_LPARAM);

static W32_LRESULT ctl_generic_proc(W32_HWND hwnd, W32_UINT msg,
                                    W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_DESTROY) { ctl_free(hwnd); return 0; }
    ctl_state_t *c = ctl_of(hwnd);
    if (!c) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (c->kind) {
    case CTL_KIND_TOOLBAR:  return ctl_toolbar_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_STATUS:   return ctl_status_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_LISTVIEW: return ctl_listview_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_TREEVIEW: return ctl_treeview_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_TAB:      return ctl_tab_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_TOOLTIP:  return ctl_tooltip_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_PROGRESS: return ctl_progress_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_HEADER:   return ctl_header_proc(c, hwnd, msg, wp, lp);
    case CTL_KIND_PSPAGE:   return ctl_pspage_proc(c, hwnd, msg, wp, lp);
    default:                return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

#ifndef W32_WM_LBUTTONDBLCLK
#define W32_WM_LBUTTONDBLCLK 0x0203
#endif
#ifndef COMCTL_ROW_H
#ifdef AURALITE_W32_HOST_TEST
#define COMCTL_ROW_H 14
#else
#define COMCTL_ROW_H AG_ROW_H
#endif
#endif

/* ===================================================================== *
 * Toolbar
 * ===================================================================== */

static void tb_geometry(ctl_state_t *c, W32_HWND hwnd, W32_RECT *rc) {
    GetClientRect(hwnd, rc);
    if (c->u.tb.btn_w <= 0) c->u.tb.btn_w = 40;
    if (c->u.tb.btn_h <= 0) c->u.tb.btn_h = 22;
}

static void ctl_toolbar_paint(ctl_state_t *c, W32_HWND hwnd) {
    W32_PAINTSTRUCT ps;
    W32_HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;
    W32_RECT rc;
    tb_geometry(c, hwnd, &rc);
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    for (int i = 0; i < c->u.tb.n_btn; i++) {
        tb_button_t *b = &c->u.tb.btn[i];
        if (b->state & W32_TBSTATE_HIDDEN) continue;
        if (b->style & W32_TBSTYLE_SEP) continue;   /* separators draw nothing */
        int32_t bx = 2 + i * (c->u.tb.btn_w + 2);
        W32_RECT br = { bx, 1, bx + c->u.tb.btn_w, 1 + c->u.tb.btn_h };
        uint32_t fill = pal->face;
        if (b->state & W32_TBSTATE_CHECKED) fill = pal->hot;
        W32_HBRUSH bb = CreateSolidBrush(ag_to_colorref(fill));
        FillRect(hdc, &br, bb);
        DeleteObject(bb);
        W32_HBRUSH fb = CreateSolidBrush(ag_to_colorref(pal->frame));
        W32_RECT fr = br;
        FrameRect(hdc, &fr, fb);
        DeleteObject(fb);
        int drawn = 0;
        if (c->u.tb.himl && b->bitmap >= 0) {
            drawn = ImageList_Draw(c->u.tb.himl, b->bitmap, hdc,
                                   bx + 2, 2, W32_ILD_NORMAL) ? 1 : 0;
        }
        if (!drawn && c->u.tb.text[i][0]) {
            SetBkMode(hdc, W32_TRANSPARENT);
            SetTextColor(hdc, ag_to_colorref(
                (b->state & W32_TBSTATE_ENABLED) ? pal->text : pal->frame));
            TextOutA(hdc, bx + 4, 2 + (c->u.tb.btn_h - 10) / 2,
                     c->u.tb.text[i], (int32_t)strlen(c->u.tb.text[i]));
        }
    }
    EndPaint(hwnd, &ps);
}

static W32_LRESULT ctl_toolbar_proc(ctl_state_t *c, W32_HWND hwnd,
                                    W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_toolbar_paint(c, hwnd);
        return 0;
    case W32_TB_BUTTONSTRUCTSIZE:
        return 0;                       /* documented: no return value */
    case W32_TB_ADDBUTTONSA:
    case W32_TB_ADDBUTTONSW: {
        int n = (int)(int32_t)(uint32_t)wp;
        const W32_TBBUTTON *src = (const W32_TBBUTTON *)(uintptr_t)lp;
        if (n < 0 || !src) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
        if (c->u.tb.n_btn + n > TB_MAX_BUTTONS) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        int first = c->u.tb.n_btn;
        for (int i = 0; i < n; i++) {
            tb_button_t *d = &c->u.tb.btn[c->u.tb.n_btn];
            memset(d, 0, sizeof *d);
            d->bitmap  = src[i].iBitmap;
            d->cmd     = src[i].idCommand;
            d->state   = src[i].fsState;
            d->style   = src[i].fsStyle;
            /* iString as an in-pointer: the documented LPSTR-in-iString
             * convention CreateToolbarEx callers use.  -1 and small
             * non-negative values are string-pool indices (no pool here:
             * no label); only a plausible pointer is copied. */
            const char *s = (const char *)(uintptr_t)src[i].iString;
            if (s && (int64_t)src[i].iString > 0x10000)
                ctl_strlcpy(c->u.tb.text[c->u.tb.n_btn],
                            sizeof c->u.tb.text[0], s);
            c->u.tb.n_btn++;
        }
        return first;
    }
    case W32_TB_GETBUTTON: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.tb.n_btn || !lp) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
        }
        W32_TBBUTTON *d = (W32_TBBUTTON *)(uintptr_t)lp;
        memset(d, 0, sizeof *d);
        d->iBitmap   = c->u.tb.btn[i].bitmap;
        d->idCommand = c->u.tb.btn[i].cmd;
        d->fsState   = (uint8_t)c->u.tb.btn[i].state;
        d->fsStyle   = (uint8_t)c->u.tb.btn[i].style;
        d->dwData    = 0;
        d->iString   = -1;
        return 1;
    }
    case W32_TB_DELETEBUTTON: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.tb.n_btn) return 0;
        for (int j = i; j + 1 < c->u.tb.n_btn; j++) {
            c->u.tb.btn[j] = c->u.tb.btn[j+1];
            memcpy(c->u.tb.text[j], c->u.tb.text[j+1], sizeof c->u.tb.text[0]);
        }
        c->u.tb.n_btn--;
        return 1;
    }
    case W32_TB_BUTTONCOUNT:
        return c->u.tb.n_btn;
    case W32_TB_COMMANDTOINDEX: {
        int32_t cmd = (int32_t)(uint32_t)wp;
        for (int i = 0; i < c->u.tb.n_btn; i++)
            if (c->u.tb.btn[i].cmd == cmd) return i;
        return -1;
    }
    case W32_TB_SETBUTTONSIZE:
        c->u.tb.btn_w = lp_x(lp);
        c->u.tb.btn_h = lp_y(lp);
        return 1;
    case W32_TB_SETBITMAPSIZE:
        return 1;                       /* cell size comes from the image list */
    case W32_TB_GETBUTTONSIZE:
        return (W32_LRESULT)(uint32_t)
               ((uint32_t)(c->u.tb.btn_h ? c->u.tb.btn_h : 22) << 16 |
                (uint32_t)(c->u.tb.btn_w ? c->u.tb.btn_w : 40));
    case W32_TB_SETIMAGELIST:
        c->u.tb.himl = (W32_HIMAGELIST)(uintptr_t)lp;
        return 0;
    case W32_TB_GETIMAGELIST:
        return (W32_LRESULT)(uintptr_t)c->u.tb.himl;
    case W32_TB_ENABLEBUTTON: {
        int32_t cmd = (int32_t)(uint32_t)wp;
        int on = lp ? 1 : 0;
        for (int i = 0; i < c->u.tb.n_btn; i++)
            if (c->u.tb.btn[i].cmd == cmd) {
                if (on) c->u.tb.btn[i].state |= W32_TBSTATE_ENABLED;
                else    c->u.tb.btn[i].state &= ~(uint32_t)W32_TBSTATE_ENABLED;
                return 1;
            }
        return 0;
    }
    case W32_TB_CHECKBUTTON: {
        int32_t cmd = (int32_t)(uint32_t)wp;
        int on = lp ? 1 : 0;
        for (int i = 0; i < c->u.tb.n_btn; i++)
            if (c->u.tb.btn[i].cmd == cmd) {
                if (on) c->u.tb.btn[i].state |= W32_TBSTATE_CHECKED;
                else    c->u.tb.btn[i].state &= ~(uint32_t)W32_TBSTATE_CHECKED;
                return 1;
            }
        return 0;
    }
    case W32_TB_ISBUTTONENABLED:
    case W32_TB_ISBUTTONCHECKED:
    case W32_TB_ISBUTTONPRESSED:
    case W32_TB_ISBUTTONHIDDEN: {
        int32_t cmd = (int32_t)(uint32_t)wp;
        for (int i = 0; i < c->u.tb.n_btn; i++)
            if (c->u.tb.btn[i].cmd == cmd) {
                uint32_t st = c->u.tb.btn[i].state;
                switch (msg) {
                case W32_TB_ISBUTTONENABLED: return (st & W32_TBSTATE_ENABLED) ? 1 : 0;
                case W32_TB_ISBUTTONCHECKED: return (st & W32_TBSTATE_CHECKED) ? 1 : 0;
                case W32_TB_ISBUTTONPRESSED: return (st & W32_TBSTATE_PRESSED) ? 1 : 0;
                default:                     return (st & W32_TBSTATE_HIDDEN) ? 1 : 0;
                }
            }
        return -1;
    }
    case W32_TB_GETITEMRECT: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_RECT *r = (W32_RECT *)(uintptr_t)lp;
        if (i < 0 || i >= c->u.tb.n_btn || !r) return 0;
        W32_RECT rc;
        tb_geometry(c, hwnd, &rc);
        r->left = 2 + i * (c->u.tb.btn_w + 2);
        r->top = 1;
        r->right = r->left + c->u.tb.btn_w;
        r->bottom = r->top + c->u.tb.btn_h;
        return 1;
    }
    case W32_TB_CUSTOMIZE:
        /* Named refusal: no customisation dialog in this personality. */
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    case W32_WM_LBUTTONDOWN: {
        int32_t x = lp_x(lp);
        if (x < 2) return 0;
        int i = (x - 2) / (c->u.tb.btn_w + 2);
        if (i < 0 || i >= c->u.tb.n_btn) return 0;
        tb_button_t *b = &c->u.tb.btn[i];
        if (!(b->state & W32_TBSTATE_ENABLED)) return 0;
        if (b->style & W32_TBSTYLE_SEP) return 0;
        if (b->style & W32_TBSTYLE_CHECK)
            b->state ^= W32_TBSTATE_CHECKED;
        W32_HWND parent = GetParent(hwnd);
        if (parent)
            PostMessageW(parent, W32_WM_COMMAND,
                         (W32_WPARAM)(uint32_t)
                         ((uint32_t)b->cmd & 0xFFFFu),
                         (W32_LPARAM)(uintptr_t)hwnd);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Status bar
 * ===================================================================== */

static void ctl_status_paint(ctl_state_t *c, W32_HWND hwnd) {
    W32_PAINTSTRUCT ps;
    W32_HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;
    W32_RECT rc;
    GetClientRect(hwnd, &rc);
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, W32_TRANSPARENT);
    SetTextColor(hdc, ag_to_colorref(pal->text));
    int32_t prev = 0;
    for (int i = 0; i < c->u.sb.n_parts; i++) {
        int32_t right = c->u.sb.edge[i] ? c->u.sb.edge[i]
                                        : rc.right * (i + 1) / c->u.sb.n_parts;
        if (!(c->u.sb.op[i] & W32_SBT_NOBORDERS)) {
            W32_HBRUSH fb = CreateSolidBrush(ag_to_colorref(pal->frame));
            W32_RECT fr = { prev, 0, right, rc.bottom };
            FrameRect(hdc, &fr, fb);
            DeleteObject(fb);
        }
        if (c->u.sb.text[i][0])
            TextOutA(hdc, prev + 4, (rc.bottom - 10) / 2,
                     c->u.sb.text[i], (int32_t)strlen(c->u.sb.text[i]));
        prev = right;
    }
    if (c->u.sb.simple && c->u.sb.text[0][0])
        TextOutA(hdc, 4, (rc.bottom - 10) / 2,
                 c->u.sb.text[0], (int32_t)strlen(c->u.sb.text[0]));
    EndPaint(hwnd, &ps);
}

static W32_LRESULT ctl_status_proc(ctl_state_t *c, W32_HWND hwnd,
                                   W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_status_paint(c, hwnd);
        return 0;
    case W32_SB_SETPARTS: {
        int n = (int)(int32_t)(uint32_t)wp;
        const int32_t *e = (const int32_t *)(uintptr_t)lp;
        if (n < 0 || n > SB_MAX_PARTS || (n && !e)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
        }
        for (int i = 0; i < n; i++) c->u.sb.edge[i] = e[i];
        c->u.sb.n_parts = n;
        return 1;
    }
    case W32_SB_GETPARTS: {
        int cap = (int)(int32_t)(uint32_t)wp;
        int32_t *e = (int32_t *)(uintptr_t)lp;
        int n = c->u.sb.n_parts;
        if (e && cap > 0)
            for (int i = 0; i < n && i < cap; i++) e[i] = c->u.sb.edge[i];
        return n;
    }
    case W32_SB_SETTEXTA:
    case W32_SB_SETTEXTW: {
        int part = (int)((uint32_t)wp & 0xFFu);
        if (part < 0 || part >= SB_MAX_PARTS) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
        }
        c->u.sb.op[part] = (uint32_t)wp & 0xFF00u;
        if (msg == W32_SB_SETTEXTW) {
            w32_utf16z_to_utf8((const uint16_t *)(uintptr_t)lp,
                               c->u.sb.text[part],
                               (int32_t)sizeof c->u.sb.text[0]);
        } else {
            ctl_strlcpy(c->u.sb.text[part], sizeof c->u.sb.text[0], (const char *)(uintptr_t)lp);
        }
        if (c->u.sb.n_parts <= part) c->u.sb.n_parts = part + 1;
        return 1;
    }
    case W32_SB_GETTEXTA:
    case W32_SB_GETTEXTW: {
        int part = (int)((uint32_t)wp & 0xFFu);
        if (part < 0 || part >= SB_MAX_PARTS) return 0;
        size_t len = strlen(c->u.sb.text[part]);
        if (msg == W32_SB_GETTEXTW) {
            a_to_w16_buf((uint16_t *)(uintptr_t)lp, 64, c->u.sb.text[part]);
        } else {
            memcpy((void *)(uintptr_t)lp, c->u.sb.text[part], len + 1);
        }
        return (W32_LRESULT)(uint32_t)
               ((c->u.sb.op[part] & 0xFF00u) | (uint32_t)len);
    }
    case W32_SB_GETTEXTLENGTHA:
    case W32_SB_GETTEXTLENGTHW: {
        int part = (int)((uint32_t)wp & 0xFFu);
        if (part < 0 || part >= SB_MAX_PARTS) return 0;
        return (W32_LRESULT)(uint32_t)
               ((c->u.sb.op[part] & 0xFF00u) |
                (uint32_t)strlen(c->u.sb.text[part]));
    }
    case W32_SB_SIMPLE:
        c->u.sb.simple = wp ? 1 : 0;
        return 1;
    case W32_SB_SETMINHEIGHT:
        return 1;
    case W32_SB_GETBORDERS: {
        int32_t *b = (int32_t *)(uintptr_t)lp;
        if (!b) return 0;
        b[0] = 1; b[1] = 1; b[2] = 1;
        return 1;
    }
    case W32_SB_GETRECT: {
        int part = (int)(int32_t)(uint32_t)wp;
        W32_RECT *r = (W32_RECT *)(uintptr_t)lp;
        if (!r || part < 0 || part >= c->u.sb.n_parts) return 0;
        W32_RECT rc;
        GetClientRect(hwnd, &rc);
        int32_t prev = 0;
        for (int i = 0; i < part; i++)
            prev = c->u.sb.edge[i] ? c->u.sb.edge[i]
                                   : rc.right * (i + 1) / c->u.sb.n_parts;
        r->left = prev;
        r->top = 0;
        r->right = c->u.sb.edge[part] ? c->u.sb.edge[part]
                      : rc.right * (part + 1) / c->u.sb.n_parts;
        r->bottom = rc.bottom;
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Listview (the widget is the renderer; LVM_* is the semantics)
 * ===================================================================== */

/* Rebuild the listbox rows from the item array (order == identity). */
static void lv_rebuild(ctl_state_t *c) {
    ctl_surf_clear(&c->surf);
    for (int i = 0; i < c->u.lv.n_items; i++)
        ctl_surf_add_row(&c->surf, -1, c->u.lv.text[i]);
}

static void lv_splice(ctl_state_t *c, int at, int del, const char *with_text,
                      uint32_t with_state, int64_t with_lparam) {
    if (at < 0 || at > c->u.lv.n_items) at = c->u.lv.n_items;
    if (del > 0) {
        for (int i = at; i + del < c->u.lv.n_items; i++) {
            memcpy(c->u.lv.text[i], c->u.lv.text[i + del],
                   sizeof c->u.lv.text[0]);
            c->u.lv.state[i]  = c->u.lv.state[i + del];
            c->u.lv.lparam[i] = c->u.lv.lparam[i + del];
        }
        c->u.lv.n_items -= del;
    }
    if (with_text) {
        for (int i = c->u.lv.n_items; i > at; i--) {
            memcpy(c->u.lv.text[i], c->u.lv.text[i-1], sizeof c->u.lv.text[0]);
            c->u.lv.state[i]  = c->u.lv.state[i-1];
            c->u.lv.lparam[i] = c->u.lv.lparam[i-1];
        }
        ctl_strlcpy(c->u.lv.text[at], sizeof c->u.lv.text[0], with_text);
        c->u.lv.state[at]  = with_state;
        c->u.lv.lparam[at] = with_lparam;
        c->u.lv.n_items++;
    }
    lv_rebuild(c);
    /* keep the widget's selection in step with item state */
    int sel = -1;
    for (int i = 0; i < c->u.lv.n_items; i++)
        if (c->u.lv.state[i] & W32_LVIS_SELECTED) { sel = i; break; }
    if (sel >= 0) ctl_surf_set_sel(&c->surf, sel);
}

static void lv_fill_item_out(ctl_state_t *c, W32_LVITEMW *it) {
    if (it->mask & W32_LVIF_TEXT) {
        if (it->pszText && it->cchTextMax > 0)
            a_to_w16_buf((uint16_t *)(uintptr_t)it->pszText, (size_t)it->cchTextMax,
                         c->u.lv.text[it->iItem]);
    }
    if (it->mask & W32_LVIF_STATE)
        it->state = c->u.lv.state[it->iItem];
    if (it->mask & W32_LVIF_PARAM)
        it->lParam = c->u.lv.lparam[it->iItem];
}

static W32_LRESULT ctl_listview_proc(ctl_state_t *c, W32_HWND hwnd,
                                     W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_surf_render(&c->surf, w32_win_cls_ag_wid(hwnd));
        return 0;
    case W32_LVM_INSERTITEMA:
    case W32_LVM_INSERTITEMW: {
        W32_LVITEMW *it = (W32_LVITEMW *)(uintptr_t)lp;
        if (!it || c->u.lv.n_items >= LV_MAX_ITEMS) {
            w32_set_last_error(!it ? W32_ERROR_INVALID_PARAMETER
                                   : W32_ERROR_NOT_ENOUGH_MEMORY);
            return -1;
        }
        char label[48];
        if (msg == W32_LVM_INSERTITEMW && it->pszText) {
            w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
        } else if (it->pszText) {
            ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
        } else label[0] = 0;
        int at = (it->iItem < 0 || it->iItem > c->u.lv.n_items)
                 ? c->u.lv.n_items : it->iItem;
        lv_splice(c, at, 0, label,
                  (it->mask & W32_LVIF_STATE) ? it->state : 0,
                  (it->mask & W32_LVIF_PARAM) ? it->lParam : 0);
        W32_NMLISTVIEW nm;
        memset(&nm, 0, sizeof nm);
        nm.iItem = at; nm.lParam = c->u.lv.lparam[at];
        notify_parent(hwnd, W32_LVN_INSERTITEM, &nm.hdr);
        return at;
    }
    case W32_LVM_DELETEITEM: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.lv.n_items) return 0;
        lv_splice(c, i, 1, 0, 0, 0);
        return 1;
    }
    case W32_LVM_DELETEALLITEMS:
        c->u.lv.n_items = 0;
        c->u.lv.sel_mark = -1;
        ctl_surf_clear(&c->surf);
        return 1;
    case W32_LVM_GETITEMCOUNT:
        return c->u.lv.n_items;
    case W32_LVM_GETITEMA:
    case W32_LVM_GETITEMW: {
        W32_LVITEMW *it = (W32_LVITEMW *)(uintptr_t)lp;
        if (!it || it->iItem < 0 || it->iItem >= c->u.lv.n_items) return 0;
        lv_fill_item_out(c, it);
        return 1;
    }
    case W32_LVM_GETITEMTEXTA:
    case W32_LVM_GETITEMTEXTW: {
        W32_LVITEMW *it = (W32_LVITEMW *)(uintptr_t)lp;
        if (!it || it->iItem < 0 || it->iItem >= c->u.lv.n_items) return 0;
        it->mask |= W32_LVIF_TEXT;
        lv_fill_item_out(c, it);
        return 1;
    }
    case W32_LVM_SETITEMA:
    case W32_LVM_SETITEMW: {
        W32_LVITEMW *it = (W32_LVITEMW *)(uintptr_t)lp;
        if (!it || it->iItem < 0 || it->iItem >= c->u.lv.n_items) return 0;
        uint32_t changed = 0;
        if (it->mask & W32_LVIF_TEXT) {
            char label[48];
            if (it->pszText) {
                if (msg == W32_LVM_SETITEMW)
                    w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
                else
                    ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
            } else label[0] = 0;
            memcpy(c->u.lv.text[it->iItem], label, sizeof label);
            changed |= W32_LVIF_TEXT;
        }
        if (it->mask & W32_LVIF_PARAM) {
            c->u.lv.lparam[it->iItem] = it->lParam;
            changed |= W32_LVIF_PARAM;
        }
        if (it->mask & W32_LVIF_STATE) {
            W32_NMLISTVIEW nm;
            memset(&nm, 0, sizeof nm);
            nm.iItem = it->iItem;
            nm.uOldState = c->u.lv.state[it->iItem];
            nm.uChanged = W32_LVIF_STATE;
            uint32_t m = it->stateMask ? it->stateMask : 0xFFFFFFFFu;
            c->u.lv.state[it->iItem] =
                (c->u.lv.state[it->iItem] & ~m) | (it->state & m);
            nm.uNewState = c->u.lv.state[it->iItem];
            nm.lParam = c->u.lv.lparam[it->iItem];
            if (c->u.lv.state[it->iItem] & W32_LVIS_SELECTED)
                ctl_surf_set_sel(&c->surf, it->iItem);
            notify_parent(hwnd, W32_LVN_ITEMCHANGED, &nm.hdr);
            changed |= W32_LVIF_STATE;
        }
        lv_rebuild(c);              /* text may have changed */
        if (c->u.lv.state[it->iItem] & W32_LVIS_SELECTED)
            ctl_surf_set_sel(&c->surf, it->iItem);
        return 1;
    }
    case W32_LVM_GETITEMSTATE: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.lv.n_items) return 0;
        return c->u.lv.state[i] & (uint32_t)lp;
    }
    case W32_LVM_SETITEMSTATE: {
        /* the macro form: wp = iItem (-1 = all), lp = LVITEMW carrying
         * state + stateMask */
        W32_LVITEMW *it = (W32_LVITEMW *)(uintptr_t)lp;
        if (!it) return 0;
        int from = (int)(int32_t)(uint32_t)wp;
        int to = from;
        if (from == -1) { from = 0; to = c->u.lv.n_items - 1; }
        uint32_t m = it->stateMask ? it->stateMask : 0xFFFFFFFFu;
        for (int i = from; i <= to; i++) {
            if (i < 0 || i >= c->u.lv.n_items) continue;
            W32_NMLISTVIEW nm;
            memset(&nm, 0, sizeof nm);
            nm.iItem = i; nm.uChanged = W32_LVIF_STATE;
            nm.uOldState = c->u.lv.state[i];
            c->u.lv.state[i] = (c->u.lv.state[i] & ~m) | (it->state & m);
            nm.uNewState = c->u.lv.state[i];
            nm.lParam = c->u.lv.lparam[i];
            if (c->u.lv.state[i] & W32_LVIS_SELECTED)
                ctl_surf_set_sel(&c->surf, i);
            notify_parent(hwnd, W32_LVN_ITEMCHANGED, &nm.hdr);
        }
        return 1;
    }
    case W32_LVM_GETNEXTITEM: {
        int start = (int)(int32_t)(uint32_t)wp;
        uint32_t flags = (uint32_t)lp;
        for (int i = start + 1; i < c->u.lv.n_items; i++) {
            if ((flags & W32_LVNI_SELECTED) &&
                !(c->u.lv.state[i] & W32_LVIS_SELECTED)) continue;
            return i;
        }
        return -1;
    }
    case W32_LVM_GETSELECTEDCOUNT: {
        int n = 0;
        for (int i = 0; i < c->u.lv.n_items; i++)
            if (c->u.lv.state[i] & W32_LVIS_SELECTED) n++;
        return n;
    }
    case W32_LVM_GETSELECTIONMARK:
        return c->u.lv.sel_mark;
    case W32_LVM_SETSELECTIONMARK: {
        int old = c->u.lv.sel_mark;
        c->u.lv.sel_mark = (int)(int32_t)(uint32_t)wp;
        return old;
    }
    case W32_LVM_INSERTCOLUMNA:
    case W32_LVM_INSERTCOLUMNW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_LVCOLUMNW *col = (W32_LVCOLUMNW *)(uintptr_t)lp;
        if (!col || i < 0 || i > c->u.lv.n_cols || c->u.lv.n_cols >= LV_MAX_COLUMNS)
            return -1;
        char label[32];
        if (col->mask & W32_LVCF_TEXT) {
            if (msg == W32_LVM_INSERTCOLUMNW && col->pszText)
                w32_utf16z_to_utf8(col->pszText, label, (int32_t)sizeof label);
            else if (col->pszText)
                ctl_strlcpy(label, sizeof label, (const char *)col->pszText);
            else label[0] = 0;
        } else label[0] = 0;
        for (int j = c->u.lv.n_cols; j > i; j--) {
            memcpy(c->u.lv.col_text[j], c->u.lv.col_text[j-1],
                   sizeof c->u.lv.col_text[0]);
            c->u.lv.col_cx[j] = c->u.lv.col_cx[j-1];
        }
        ctl_strlcpy(c->u.lv.col_text[i], sizeof c->u.lv.col_text[0], label);
        c->u.lv.col_cx[i] = (col->mask & W32_LVCF_WIDTH) ? col->cx : 100;
        c->u.lv.n_cols++;
        return i;
    }
    case W32_LVM_DELETECOLUMN: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.lv.n_cols) return 0;
        for (int j = i; j + 1 < c->u.lv.n_cols; j++) {
            memcpy(c->u.lv.col_text[j], c->u.lv.col_text[j+1],
                   sizeof c->u.lv.col_text[0]);
            c->u.lv.col_cx[j] = c->u.lv.col_cx[j+1];
        }
        c->u.lv.n_cols--;
        return 1;
    }
    case W32_LVM_GETCOLUMNA:
    case W32_LVM_GETCOLUMNW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_LVCOLUMNW *col = (W32_LVCOLUMNW *)(uintptr_t)lp;
        if (!col || i < 0 || i >= c->u.lv.n_cols) return 0;
        if ((col->mask & W32_LVCF_TEXT) && col->pszText && col->cchTextMax > 0)
            a_to_w16_buf((uint16_t *)(uintptr_t)col->pszText, (size_t)col->cchTextMax,
                         c->u.lv.col_text[i]);
        if (col->mask & W32_LVCF_WIDTH) col->cx = c->u.lv.col_cx[i];
        if (col->mask & W32_LVCF_FMT)   col->fmt = W32_LVCFMT_LEFT;
        return 1;
    }
    case W32_LVM_ENSUREVISIBLE:
    case W32_LVM_REDRAWITEMS:
        return 1;
    case W32_LVM_HITTEST: {
        W32_LVHITTESTINFO *ht = (W32_LVHITTESTINFO *)(uintptr_t)lp;
        if (!ht) return -1;
        int row = ht->pt.y / COMCTL_ROW_H;
        ht->flags = 0;
        ht->iItem = -1;
        if (row >= 0 && row < c->u.lv.n_items) {
            ht->flags = W32_LVHT_ONITEMLABEL;
            ht->iItem = row;
        }
        return ht->iItem;
    }
    case W32_WM_LBUTTONDOWN: {
        int row = lp_y(lp) / COMCTL_ROW_H;
        if (row < 0 || row >= c->u.lv.n_items) return 0;
        int prev = -1;
        for (int i = 0; i < c->u.lv.n_items; i++)
            if (c->u.lv.state[i] & W32_LVIS_SELECTED) prev = i;
        ctl_surf_dispatch(&c->surf, CTL_EVT_DOWN, lp_x(lp), lp_y(lp));
        /* single-selection semantics: the clicked row takes the mark */
        for (int i = 0; i < c->u.lv.n_items; i++)
            c->u.lv.state[i] &= ~(uint32_t)W32_LVIS_SELECTED;
        c->u.lv.state[row] |= W32_LVIS_SELECTED | W32_LVIS_FOCUSED;
        c->u.lv.sel_mark = row;
        ctl_surf_set_sel(&c->surf, row);
        W32_NMLISTVIEW nm;
        memset(&nm, 0, sizeof nm);
        nm.iItem = row; nm.iSubItem = 0;
        nm.uNewState = c->u.lv.state[row];
        nm.uOldState = prev >= 0 ? (uint32_t)W32_LVIS_SELECTED : 0;
        nm.uChanged = W32_LVIF_STATE;
        nm.lParam = c->u.lv.lparam[row];
        notify_parent(hwnd, W32_LVN_ITEMCHANGED, &nm.hdr);
        notify_parent(hwnd, W32_NM_CLICK, &nm.hdr);
        return 0;
    }
    case W32_WM_LBUTTONDBLCLK: {
        int row = lp_y(lp) / COMCTL_ROW_H;
        if (row < 0 || row >= c->u.lv.n_items) return 0;
        W32_NMHDR nm;
        notify_parent(hwnd, W32_NM_DBLCLK, &nm);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Treeview (the ag_add_tree widget is the renderer)
 * ===================================================================== *
 * Array slot i is widget node i (ag_tree_add order), and the handle the
 * application holds is (W32_HWND)(uintptr_t)(i + 1) -- never a window,
 * just the opaque item handle Win32 documents.  A mutation that changes
 * order (insert-after, delete with subtree) replays the array into a
 * cleared widget, so the identities hold after every mutation. */

static W32_HWND tv_handle(int idx) {
    return (W32_HWND)(uintptr_t)(size_t)(idx + 1);
}
static int tv_index(W32_HWND h) {
    size_t v = (size_t)(uintptr_t)h;
    if (!v || v > 0xFFFF0000ull) return -1;   /* rejects TVI_* sentinels */
    return (int)v - 1;
}

/* Row-order questions route through the ONE authority (tree_walk via
 * these wrappers); host mirrors them with hooks. */
static int ag_tree_visible_rows_static(ctl_surface_t *s) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_visible_rows(s->wdg);
#else
    return ag_tree_visible_rows(s->widget);
#endif
}
static int ag_tree_row_node_static(ctl_surface_t *s, int row) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_row_node(s->wdg, row);
#else
    return ag_tree_row_node(s->widget, row);
#endif
}
static int ag_tree_node_row_static(ctl_surface_t *s, int node) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_node_row(s->wdg, node);
#else
    return ag_tree_node_row(s->widget, node);
#endif
}
static int ag_tree_expanded_static(ctl_surface_t *s, int node) {
#ifdef AURALITE_W32_HOST_TEST
    return w32_comctl_host_wdg_expanded(s->wdg, node);
#else
    return s->widget && s->widget->kind == AG_W_TREE &&
           node >= 0 && node < s->widget->tree_count
        ? s->widget->tree[node].expanded : 0;
#endif
}

static void tv_rebuild(ctl_state_t *c) {
    ctl_surf_clear(&c->surf);
    for (int i = 0; i < c->u.tv.n_items; i++)
        ctl_surf_add_row(&c->surf, c->u.tv.parent[i], c->u.tv.text[i]);
    for (int i = 0; i < c->u.tv.n_items; i++)
        if (c->u.tv.expanded[i]) ctl_surf_set_expanded(&c->surf, i, 1);
    if (c->u.tv.caret >= 0 && c->u.tv.caret < c->u.tv.n_items)
        ctl_surf_set_sel(&c->surf, c->u.tv.caret);
}

static W32_LRESULT ctl_treeview_proc(ctl_state_t *c, W32_HWND hwnd,
                                     W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_surf_render(&c->surf, w32_win_cls_ag_wid(hwnd));
        return 0;
    case W32_TVM_INSERTITEMA:
    case W32_TVM_INSERTITEMW: {
        W32_TVINSERTSTRUCTW *ins = (W32_TVINSERTSTRUCTW *)(uintptr_t)lp;
        if (!ins || c->u.tv.n_items >= TV_MAX_ITEMS) {
            w32_set_last_error(!ins ? W32_ERROR_INVALID_PARAMETER
                                    : W32_ERROR_NOT_ENOUGH_MEMORY);
            return (W32_LRESULT)(uintptr_t)0;
        }
        int parent = tv_index(ins->hParent);
        if (ins->hParent == W32_TVI_ROOT) parent = -1;
        if (ins->hParent && parent < 0 && ins->hParent != W32_TVI_ROOT) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return (W32_LRESULT)(uintptr_t)0;
        }
        char label[48];
        if ((ins->item.mask & W32_TVIF_TEXT) && ins->item.pszText) {
            if (msg == W32_TVM_INSERTITEMW)
                w32_utf16z_to_utf8(ins->item.pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)ins->item.pszText);
        } else label[0] = 0;

        /* Position: TVI_LAST appends under the parent; a specific
         * hInsertAfter splices right after it; TVI_FIRST splices before
         * the parent's first child.  All three replay the widget. */
        int at = c->u.tv.n_items;
        int after = tv_index(ins->hInsertAfter);
        if (ins->hInsertAfter == W32_TVI_FIRST) {
            at = parent + 1;
            if (parent < 0) at = 0;
            while (at < c->u.tv.n_items && c->u.tv.parent[at] == parent) at++;
            /* at = start of the NEXT sibling group: splice just before
             * it == last position of this parent's children */
        } else if (after >= 0) {
            at = after + 1;
        }
        if (at < 0 || at > c->u.tv.n_items) at = c->u.tv.n_items;
        for (int i = c->u.tv.n_items; i > at; i--) {
            memcpy(c->u.tv.text[i], c->u.tv.text[i-1], sizeof c->u.tv.text[0]);
            c->u.tv.parent[i]  = c->u.tv.parent[i-1];
            c->u.tv.lparam[i]  = c->u.tv.lparam[i-1];
            c->u.tv.expanded[i] = c->u.tv.expanded[i-1];
        }
        ctl_strlcpy(c->u.tv.text[at], sizeof c->u.tv.text[0], label);
        c->u.tv.parent[at]  = parent;
        c->u.tv.lparam[at]  = (ins->item.mask & W32_TVIF_PARAM) ? ins->item.lParam : 0;
        c->u.tv.expanded[at] = 0;
        c->u.tv.n_items++;
        if (c->u.tv.caret >= at) c->u.tv.caret++;
        tv_rebuild(c);
        return (W32_LRESULT)(uintptr_t)tv_handle(at);
    }
    case W32_TVM_DELETEITEM: {
        /* lp is the item handle (or TVI_ROOT = delete everything) */
        if ((W32_HWND)(uintptr_t)lp == W32_TVI_ROOT) {
            c->u.tv.n_items = 0;
            c->u.tv.caret = -1;
            ctl_surf_clear(&c->surf);
            return 1;
        }
        int idx = tv_index((W32_HWND)(uintptr_t)lp);
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        /* mark the node and every descendant dead, compact, replay */
        uint8_t dead[TV_MAX_ITEMS] = {0};
        dead[idx] = 1;
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int i = 0; i < c->u.tv.n_items; i++)
                if (!dead[i] && c->u.tv.parent[i] >= 0 && dead[c->u.tv.parent[i]]) {
                    dead[i] = 1; changed = 1;
                }
        }
        int w = 0;
        for (int i = 0; i < c->u.tv.n_items; i++) {
            if (dead[i]) continue;
            if (w != i) {
                memcpy(c->u.tv.text[w], c->u.tv.text[i], sizeof c->u.tv.text[0]);
                c->u.tv.parent[w]  = c->u.tv.parent[i];
                c->u.tv.lparam[w]  = c->u.tv.lparam[i];
                c->u.tv.expanded[w] = c->u.tv.expanded[i];
            }
            w++;
        }
        int was_sel = c->u.tv.caret;
        c->u.tv.n_items = w;
        c->u.tv.caret = (c->u.tv.caret >= 0 && !dead[was_sel] &&
                        c->u.tv.caret < w) ? c->u.tv.caret : -1;
        tv_rebuild(c);
        return 1;
    }
    case W32_TVM_GETCOUNT:
        return c->u.tv.n_items;
    case W32_TVM_GETVISIBLECOUNT:
        return ag_tree_visible_rows_static(&c->surf);
    case W32_TVM_EXPAND: {
        uint32_t action = (uint32_t)wp;
        int idx = tv_index((W32_HWND)(uintptr_t)lp);
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        int on = (action & W32_TVE_EXPAND) ? 1 : 0;
        if (action & W32_TVE_TOGGLE) on = !c->u.tv.expanded[idx];
        /* ITEMEXPANDING first, honour the TRUE-veto, then the state
         * change and ITEMEXPANDED -- the documented order. */
        W32_NMTREEVIEWW nm;
        memset(&nm, 0, sizeof nm);
        nm.action = on ? W32_TVE_EXPAND : W32_TVE_COLLAPSE;
        nm.itemNew.hItem = tv_handle(idx);
        if (notify_parent(hwnd, W32_TVN_ITEMEXPANDINGW, &nm.hdr)) return 0;
        c->u.tv.expanded[idx] = (uint8_t)on;
        ctl_surf_set_expanded(&c->surf, idx, on);
        notify_parent(hwnd, W32_TVN_ITEMEXPANDEDW, &nm.hdr);
        return 1;
    }
    case W32_TVM_GETITEMA:
    case W32_TVM_GETITEMW: {
        W32_TVITEMW *it = (W32_TVITEMW *)(uintptr_t)lp;
        int idx = it ? tv_index(it->hItem) : -1;
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        if ((it->mask & W32_TVIF_TEXT) && it->pszText && it->cchTextMax > 0) {
            if (msg == W32_TVM_GETITEMW)
                a_to_w16_buf((uint16_t *)(uintptr_t)it->pszText, (size_t)it->cchTextMax,
                             c->u.tv.text[idx]);
            else
                memcpy((void *)it->pszText, c->u.tv.text[idx],
                       strlen(c->u.tv.text[idx]) + 1);
        }
        if (it->mask & W32_TVIF_PARAM) it->lParam = c->u.tv.lparam[idx];
        if (it->mask & W32_TVIF_STATE) {
            it->state = 0;
            if (idx == c->u.tv.caret) it->state |= W32_TVIS_SELECTED;
            if (c->u.tv.expanded[idx]) it->state |= W32_TVIS_EXPANDED;
        }
        if (it->mask & W32_TVIF_CHILDREN) it->cChildren = -1; /* compute */
        {
            int kids = 0;
            for (int i = 0; i < c->u.tv.n_items; i++)
                if (c->u.tv.parent[i] == idx) kids++;
            if (it->mask & W32_TVIF_CHILDREN) it->cChildren = kids;
        }
        return 1;
    }
    case W32_TVM_SETITEMA:
    case W32_TVM_SETITEMW: {
        W32_TVITEMW *it = (W32_TVITEMW *)(uintptr_t)lp;
        int idx = it ? tv_index(it->hItem) : -1;
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        if ((it->mask & W32_TVIF_TEXT) && it->pszText) {
            char label[48];
            if (msg == W32_TVM_SETITEMW)
                w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
            ctl_strlcpy(c->u.tv.text[idx], sizeof c->u.tv.text[0], label);
            tv_rebuild(c);
        }
        if (it->mask & W32_TVIF_PARAM) c->u.tv.lparam[idx] = it->lParam;
        return 1;
    }
    case W32_TVM_GETNEXTITEM: {
        uint32_t flag = (uint32_t)wp;
        int idx = tv_index((W32_HWND)(uintptr_t)lp);
        switch (flag) {
        case W32_TVGN_ROOT:
            return c->u.tv.n_items ? (W32_LRESULT)(uintptr_t)tv_handle(0) : 0;
        case W32_TVGN_CARET:
        case 0x000Au /* TVGN_SELECTION */:
            return c->u.tv.caret >= 0
                ? (W32_LRESULT)(uintptr_t)tv_handle(c->u.tv.caret) : 0;
        case W32_TVGN_PARENT:
            if (idx < 0 || c->u.tv.parent[idx] < 0) return 0;
            return (W32_LRESULT)(uintptr_t)tv_handle(c->u.tv.parent[idx]);
        case W32_TVGN_CHILD:
            if (idx < 0) return 0;
            for (int i = 0; i < c->u.tv.n_items; i++)
                if (c->u.tv.parent[i] == idx)
                    return (W32_LRESULT)(uintptr_t)tv_handle(i);
            return 0;
        case W32_TVGN_NEXT:
            if (idx < 0) return 0;
            for (int i = idx + 1; i < c->u.tv.n_items; i++)
                if (c->u.tv.parent[i] == c->u.tv.parent[idx])
                    return (W32_LRESULT)(uintptr_t)tv_handle(i);
            return 0;
        case W32_TVGN_PREVIOUS:
            if (idx <= 0) return 0;
            for (int i = idx - 1; i >= 0; i--)
                if (c->u.tv.parent[i] == c->u.tv.parent[idx])
                    return (W32_LRESULT)(uintptr_t)tv_handle(i);
            return 0;
        default:
            return 0;
        }
    }
    case W32_TVM_SELECTITEM: {
        uint32_t flag = (uint32_t)wp;
        int idx = tv_index((W32_HWND)(uintptr_t)lp);
        if (flag != W32_TVGN_CARET && flag != 0x000Au) return 0;
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        if (idx == c->u.tv.caret) return 1;
        W32_NMTREEVIEWW nm;
        memset(&nm, 0, sizeof nm);
        nm.itemOld.hItem = c->u.tv.caret >= 0 ? tv_handle(c->u.tv.caret) : 0;
        nm.itemNew.hItem = tv_handle(idx);
        if (notify_parent(hwnd, W32_TVN_SELCHANGINGW, &nm.hdr)) return 0;
        c->u.tv.caret = idx;
        ctl_surf_set_sel(&c->surf, idx);
        notify_parent(hwnd, W32_TVN_SELCHANGEDW, &nm.hdr);
        return 1;
    }
    case W32_TVM_GETITEMRECT: {
        /* wp: item-visible flag; lp: LPRECT, with the HTREEITEM at its
         * first word (the documented ABI). */
        W32_RECT *r = (W32_RECT *)(uintptr_t)lp;
        if (!r) return 0;
        int idx = tv_index(*(W32_HWND *)(void *)r);
        if (idx < 0 || idx >= c->u.tv.n_items) return 0;
        int row = ag_tree_node_row_static(&c->surf, idx);
        if (row < 0) return 0;
        W32_RECT rc;
        GetClientRect(hwnd, &rc);
        r->left = 0; r->right = rc.right;
        r->top = row * COMCTL_ROW_H;
        r->bottom = r->top + COMCTL_ROW_H;
        return 1;
    }
    case W32_TVM_ENSUREVISIBLE:
        return 1;
    case W32_TVM_HITTEST: {
        W32_TVHITTESTINFO *ht = (W32_TVHITTESTINFO *)(uintptr_t)lp;
        if (!ht) return 0;
        int row = ht->pt.y / COMCTL_ROW_H;
        int node = ag_tree_row_node_static(&c->surf, row);
        ht->flags = 0;
        ht->hItem = 0;
        if (node >= 0) {
            ht->flags = W32_TVHT_ONITEMLABEL;
            ht->hItem = tv_handle(node);
        }
        return (W32_LRESULT)(uintptr_t)ht->hItem;
    }
    case W32_WM_LBUTTONDOWN:
    case W32_WM_LBUTTONDBLCLK: {
        /* Let the widget run its own hit logic (gutter toggle, label
         * select, double-click toggle), then derive the notifications
         * from the before/after state.  Documented seam: through the
         * message path (TVM_EXPAND / TVM_SELECTITEM) the notifications
         * fire in the documented before/after order with the veto
         * honoured; through the interactive path the widget has already
         * applied the change, so both notifications follow it. */
        int prev_sel = ctl_surf_sel(&c->surf);
        uint8_t prev_exp[TV_MAX_ITEMS];
        memcpy(prev_exp, c->u.tv.expanded, sizeof prev_exp);
        ctl_surf_dispatch(&c->surf,
                          msg == W32_WM_LBUTTONDBLCLK ? CTL_EVT_DBLCLICK
                                                      : CTL_EVT_DOWN,
                          lp_x(lp), lp_y(lp));
        /* mirror any gutter/double-click toggle back into the arrays */
        for (int i = 0; i < c->u.tv.n_items; i++) {
            int now = ag_tree_expanded_static(&c->surf, i);
            if ((prev_exp[i] ? 1 : 0) != now) {
                W32_NMTREEVIEWW nm;
                memset(&nm, 0, sizeof nm);
                nm.action = now ? W32_TVE_EXPAND : W32_TVE_COLLAPSE;
                nm.itemNew.hItem = tv_handle(i);
                c->u.tv.expanded[i] = (uint8_t)now;
                notify_parent(hwnd, W32_TVN_ITEMEXPANDINGW, &nm.hdr);
                notify_parent(hwnd, W32_TVN_ITEMEXPANDEDW, &nm.hdr);
            }
        }
        int sel = ctl_surf_sel(&c->surf);
        if (sel != prev_sel) {
            W32_NMTREEVIEWW nm;
            memset(&nm, 0, sizeof nm);
            nm.itemOld.hItem = prev_sel >= 0 ? tv_handle(prev_sel) : 0;
            nm.itemNew.hItem = sel >= 0 ? tv_handle(sel) : 0;
            c->u.tv.caret = sel;
            notify_parent(hwnd, W32_TVN_SELCHANGEDW, &nm.hdr);
        }
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Tab control (ag_add_tab renders the strip)
 * ===================================================================== *
 * Label/lparam live in the mirror array; the widget's rows replay from
 * it (same replay discipline as listview/treeview). */


static void tc_rebuild(ctl_state_t *c) {
    ctl_surf_clear(&c->surf);
    for (int i = 0; i < c->u.tc.n; i++)
        ctl_surf_add_row(&c->surf, -1, c->u.tc.text[i]);
    if (c->u.tc.active >= 0 && c->u.tc.active < c->u.tc.n)
        ctl_surf_set_active(&c->surf, c->u.tc.active);
    else
        ctl_surf_set_active(&c->surf, 0);
}

static W32_LRESULT ctl_tab_proc(ctl_state_t *c, W32_HWND hwnd,
                                W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_surf_render(&c->surf, w32_win_cls_ag_wid(hwnd));
        return 0;
    case W32_TCM_INSERTITEMA:
    case W32_TCM_INSERTITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_TCITEMW *it = (W32_TCITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i > c->u.tc.n || c->u.tc.n >= 8) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return -1;
        }
        char label[32];
        if ((it->mask & W32_TCIF_TEXT) && it->pszText) {
            if (msg == W32_TCM_INSERTITEMW)
                w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
        } else label[0] = 0;
        for (int j = c->u.tc.n; j > i; j--) {
            memcpy(c->u.tc.text[j], c->u.tc.text[j-1], sizeof c->u.tc.text[0]);
            c->u.tc.lparam[j] = c->u.tc.lparam[j-1];
        }
        ctl_strlcpy(c->u.tc.text[i], sizeof c->u.tc.text[0], label);
        c->u.tc.lparam[i] = (it->mask & W32_TCIF_PARAM) ? it->lParam : 0;
        c->u.tc.n++;
        /* the first tab selects itself; a tab inserted at or before the
         * selection pushes it up (documented behaviour) */
        if (c->u.tc.active < 0) c->u.tc.active = 0;
        else if (i <= c->u.tc.active) c->u.tc.active++;
        tc_rebuild(c);
        return i;
    }
    case W32_TCM_DELETEITEM: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.tc.n) return 0;
        for (int j = i; j + 1 < c->u.tc.n; j++) {
            memcpy(c->u.tc.text[j], c->u.tc.text[j+1], sizeof c->u.tc.text[0]);
            c->u.tc.lparam[j] = c->u.tc.lparam[j+1];
        }
        c->u.tc.n--;
        if (c->u.tc.active > i) c->u.tc.active--;
        else if (c->u.tc.active == i) c->u.tc.active = c->u.tc.n ? 0 : -1;
        tc_rebuild(c);
        return 1;
    }
    case W32_TCM_DELETEALLITEMS:
        c->u.tc.n = 0;
        c->u.tc.active = -1;
        ctl_surf_clear(&c->surf);
        return 1;
    case W32_TCM_GETITEMCOUNT:
        return c->u.tc.n;
    case W32_TCM_GETITEMA:
    case W32_TCM_GETITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_TCITEMW *it = (W32_TCITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i >= c->u.tc.n) return 0;
        if ((it->mask & W32_TCIF_TEXT) && it->pszText && it->cchTextMax > 0) {
            if (msg == W32_TCM_GETITEMW)
                a_to_w16_buf((uint16_t *)(uintptr_t)it->pszText, (size_t)it->cchTextMax, c->u.tc.text[i]);
            else
                memcpy((void *)it->pszText, c->u.tc.text[i],
                       strlen(c->u.tc.text[i]) + 1);
        }
        if (it->mask & W32_TCIF_PARAM) it->lParam = c->u.tc.lparam[i];
        return 1;
    }
    case W32_TCM_SETITEMA:
    case W32_TCM_SETITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_TCITEMW *it = (W32_TCITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i >= c->u.tc.n) return 0;
        if ((it->mask & W32_TCIF_TEXT) && it->pszText) {
            char label[32];
            if (msg == W32_TCM_SETITEMW)
                w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
            ctl_strlcpy(c->u.tc.text[i], sizeof c->u.tc.text[0], label);
            tc_rebuild(c);
        }
        if (it->mask & W32_TCIF_PARAM) c->u.tc.lparam[i] = it->lParam;
        return 1;
    }
    case W32_TCM_GETCURSEL:
    case W32_TCM_GETCURFOCUS:
        return c->u.tc.active;
    case W32_TCM_SETCURSEL: {
        /* Documented: changes the selection WITHOUT notifying. */
        int i = (int)(int32_t)(uint32_t)wp;
        int old = c->u.tc.active;
        if (i < 0 || i >= c->u.tc.n) return old;
        c->u.tc.active = i;
        ctl_surf_set_active(&c->surf, i);
        return old;
    }
    case W32_TCM_ADJUSTRECT: {
        /* wp TRUE: window rect in, display rect out.  The strip is a
         * fixed 24px band above the display area (approximated,
         * documented). */
        W32_RECT *r = (W32_RECT *)(uintptr_t)lp;
        if (!r) return 0;
        if (wp) r->top += 24;
        else    r->top -= 24;
        return 1;
    }
    case W32_TCM_GETITEMRECT: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_RECT *r = (W32_RECT *)(uintptr_t)lp;
        if (!r || i < 0 || i >= c->u.tc.n) return 0;
        r->left = i * 90; r->top = 0;
        r->right = r->left + 90; r->bottom = 22;
        return 1;
    }
    case W32_WM_LBUTTONDOWN:
    case W32_WM_LBUTTONDBLCLK: {
        int prev = c->u.tc.active;
        ctl_surf_dispatch(&c->surf, CTL_EVT_DOWN, lp_x(lp), lp_y(lp));
        int now = ctl_surf_active(&c->surf);
        if (now != prev) {
            c->u.tc.active = now;
            W32_NMHDR nm;
            notify_parent(hwnd, W32_TCN_SELCHANGE, &nm);
        }
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Tooltip
 * ===================================================================== */

static void tt_hide(ctl_state_t *c, W32_HWND hwnd) {
    if (!c->u.tt.shown) return;
    c->u.tt.shown = 0;
    c->u.tt.current = -1;
    ShowWindow(hwnd, W32_SW_HIDE);
    KillTimer(hwnd, 1);
    W32_NMHDR nm;
    notify_parent(hwnd, W32_TTN_POP, &nm);
}

static W32_LRESULT ctl_tooltip_proc(ctl_state_t *c, W32_HWND hwnd,
                                    W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT: {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc) return 0;
        W32_RECT rc;
        GetClientRect(hwnd, &rc);
        /* the classic tip colours, version-independent */
        W32_HBRUSH bg = CreateSolidBrush(0x00E1FFFFu);   /* COLORREF pale yellow */
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        W32_HBRUSH fb = CreateSolidBrush(0x00000000u);
        W32_RECT fr = rc;
        FrameRect(hdc, &fr, fb);
        DeleteObject(fb);
        SetBkMode(hdc, W32_TRANSPARENT);
        SetTextColor(hdc, 0x00000000u);
        int idx = c->u.tt.current;
        if (idx >= 0 && idx < TT_MAX_TOOLS) {
            char label[64];
            w32_utf16z_to_utf8(c->u.tt.tool[idx].text, label, (int32_t)sizeof label);
            TextOutA(hdc, 3, 2, label, (int32_t)strlen(label));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case W32_TTM_ADDTOOLA:
    case W32_TTM_ADDTOOLW: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti || ti->cbSize < offsetof(W32_TOOLINFOW, lParam)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
        }
        int slot = -1;
        for (int i = 0; i < TT_MAX_TOOLS; i++)
            if (!c->u.tt.tool[i].in_use) { slot = i; break; }
        if (slot < 0) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
        c->u.tt.tool[slot].in_use = 1;
        c->u.tt.tool[slot].hwnd = ti->hwnd;
        c->u.tt.tool[slot].uId  = ti->uId;
        c->u.tt.tool[slot].rect = ti->rect;
        if (msg == W32_TTM_ADDTOOLW) {
            if (ti->lpszText && (uintptr_t)ti->lpszText > 0xFFFF)
                memcpy_w16(c->u.tt.tool[slot].text,
                           (const uint16_t *)ti->lpszText,
                           W32_ARRAY_COUNT(c->u.tt.tool[0].text));
            else c->u.tt.tool[slot].text[0] = 0;
        } else {
            a_to_w16_buf(c->u.tt.tool[slot].text,
                         W32_ARRAY_COUNT(c->u.tt.tool[0].text),
                         (const char *)(uintptr_t)ti->lpszText);
        }
        return 1;
    }
    case W32_TTM_DELTOOLA:
    case W32_TTM_DELTOOLW: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti) return 0;
        for (int i = 0; i < TT_MAX_TOOLS; i++)
            if (c->u.tt.tool[i].in_use && c->u.tt.tool[i].hwnd == ti->hwnd &&
                c->u.tt.tool[i].uId == ti->uId) {
                c->u.tt.tool[i].in_use = 0;
                if (c->u.tt.current == i) tt_hide(c, hwnd);
                return 1;
            }
        return 0;
    }
    case W32_TTM_NEWTOOLRECTA:
    case W32_TTM_NEWTOOLRECTW: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti) return 0;
        for (int i = 0; i < TT_MAX_TOOLS; i++)
            if (c->u.tt.tool[i].in_use && c->u.tt.tool[i].hwnd == ti->hwnd &&
                c->u.tt.tool[i].uId == ti->uId) {
                c->u.tt.tool[i].rect = ti->rect;
                return 1;
            }
        return 0;
    }
    case W32_TTM_UPDATETIPTEXTA:
    case W32_TTM_UPDATETIPTEXTW: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti) return 0;
        int idx = c->u.tt.current;
        if (idx < 0) return 0;
        if (msg == W32_TTM_UPDATETIPTEXTW)
            memcpy_w16(c->u.tt.tool[idx].text,
                       (const uint16_t *)ti->lpszText,
                       W32_ARRAY_COUNT(c->u.tt.tool[0].text));
        else
            a_to_w16_buf(c->u.tt.tool[idx].text,
                         W32_ARRAY_COUNT(c->u.tt.tool[0].text),
                         (const char *)(uintptr_t)ti->lpszText);
        return 1;
    }
    case W32_TTM_RELAYEVENT: {
        W32_MSG *m = (W32_MSG *)(uintptr_t)lp;
        if (!m || (m->message != W32_WM_MOUSEMOVE &&
                   m->message != W32_WM_LBUTTONDOWN)) return 0;
        if (!c->u.tt.active) return 0;
        /* find the tool whose rect contains the point (client coords of
         * the tool's own window) */
        int hit = -1;
        for (int i = 0; i < TT_MAX_TOOLS; i++) {
            if (!c->u.tt.tool[i].in_use) continue;
            if (c->u.tt.tool[i].hwnd != m->hwnd) continue;
            W32_POINT pt = { lp_x(m->lParam), lp_y(m->lParam) };
            if (pt.x >= c->u.tt.tool[i].rect.left &&
                pt.x <  c->u.tt.tool[i].rect.right &&
                pt.y >= c->u.tt.tool[i].rect.top &&
                pt.y <  c->u.tt.tool[i].rect.bottom) { hit = i; break; }
        }
        if (hit < 0) { tt_hide(c, hwnd); return 0; }
        if (c->u.tt.shown && c->u.tt.current == hit) return 0;
        W32_NMHDR nm;
        notify_parent(hwnd, W32_TTN_SHOW, &nm);
        c->u.tt.current = hit;
        c->u.tt.shown = 1;
        /* place near the cursor: translate the tool's client point */
        W32_POINT pt = { lp_x(m->lParam), lp_y(m->lParam) };
        ClientToScreen(m->hwnd, &pt);
        char label[64];
        w32_utf16z_to_utf8(c->u.tt.tool[hit].text, label, (int32_t)sizeof label);
        int32_t w = (int32_t)strlen(label) * 8 + 8;
        MoveWindow(hwnd, pt.x + 16, pt.y + 18, w, 22, 1);
        ShowWindow(hwnd, W32_SW_SHOWNOACTIVATE);
        SetTimer(hwnd, 1, 5000, 0);          /* auto-pop, like the default */
        return 0;
    }
    case W32_TTM_ACTIVATE:
        c->u.tt.active = wp ? 1 : 0;
        if (!c->u.tt.active) tt_hide(c, hwnd);
        return 0;
    case W32_TTM_SETDELAYTIME:
        return 0;                            /* delays are fixed (5s pop) */
    case W32_TTM_GETCURRENTTOOLA:
    case W32_TTM_GETCURRENTTOOLW: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti) return 0;
        if (!c->u.tt.shown || c->u.tt.current < 0) return 0;
        int i = c->u.tt.current;
        ti->hwnd = c->u.tt.tool[i].hwnd;
        ti->uId  = c->u.tt.tool[i].uId;
        ti->rect = c->u.tt.tool[i].rect;
        if (msg == W32_TTM_GETCURRENTTOOLW)
            memcpy_w16(ti->lpszText, c->u.tt.tool[i].text,
                       W32_ARRAY_COUNT(c->u.tt.tool[0].text));
        return 1;
    }
    case W32_TTM_GETTOOLCOUNT: {
        int n = 0;
        for (int i = 0; i < TT_MAX_TOOLS; i++)
            if (c->u.tt.tool[i].in_use) n++;
        return n;
    }
    case W32_TTM_GETTEXTW:
    case W32_TTM_GETTEXTA: {
        W32_TOOLINFOW *ti = (W32_TOOLINFOW *)(uintptr_t)lp;
        if (!ti) return 0;
        for (int i = 0; i < TT_MAX_TOOLS; i++)
            if (c->u.tt.tool[i].in_use && c->u.tt.tool[i].hwnd == ti->hwnd &&
                c->u.tt.tool[i].uId == ti->uId) {
                if (msg == W32_TTM_GETTEXTW)
                    memcpy_w16(ti->lpszText, c->u.tt.tool[i].text, 64);
                else
                    w32_utf16z_to_utf8(c->u.tt.tool[i].text,
                                       (char *)(uintptr_t)ti->lpszText, 64);
                return 1;
            }
        return 0;
    }
    case W32_WM_TIMER:
        tt_hide(c, hwnd);
        return 0;
    case W32_WM_LBUTTONDOWN:
        tt_hide(c, hwnd);                    /* click dismisses, like Win32 */
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Progress bar (ag_add_progress renders it)
 * ===================================================================== */

static W32_LRESULT ctl_progress_proc(ctl_state_t *c, W32_HWND hwnd,
                                     W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    /* The widget holds [0, span]; Win32's [min, max] maps by offset. */
    switch (msg) {
    case W32_WM_PAINT:
        ctl_surf_render(&c->surf, w32_win_cls_ag_wid(hwnd));
        return 0;
    case W32_PBM_SETRANGE:
        c->u.pb.min = (int16_t)(uint16_t)(uint32_t)lp;
        c->u.pb.max = (int16_t)(uint16_t)((uint64_t)lp >> 16);
        ctl_surf_set_progress(&c->surf, ctl_surf_progress(&c->surf),
                              c->u.pb.max - c->u.pb.min);
        return 0;
    case W32_PBM_SETRANGE32:
        c->u.pb.min = (int32_t)(uint32_t)wp;
        c->u.pb.max = (int32_t)(uint32_t)lp;
        ctl_surf_set_progress(&c->surf, ctl_surf_progress(&c->surf),
                              c->u.pb.max - c->u.pb.min);
        return 0;
    case W32_PBM_SETPOS: {
        int prev = ctl_surf_progress(&c->surf) + c->u.pb.min;
        ctl_surf_set_progress(&c->surf,
                              (int)(int32_t)(uint32_t)wp - c->u.pb.min,
                              c->u.pb.max - c->u.pb.min);
        return prev;
    }
    case W32_PBM_DELTAPOS: {
        int prev = ctl_surf_progress(&c->surf) + c->u.pb.min;
        ctl_surf_set_progress(&c->surf,
                              prev + (int)(int32_t)(int64_t)lp - c->u.pb.min,
                              c->u.pb.max - c->u.pb.min);
        return prev;
    }
    case W32_PBM_SETSTEP:
        c->u.pb.step = (int)(int32_t)(uint32_t)wp;
        return 0;
    case W32_PBM_STEPIT: {
        int prev = ctl_surf_progress(&c->surf) + c->u.pb.min;
        ctl_surf_set_progress(&c->surf, prev + c->u.pb.step - c->u.pb.min,
                              c->u.pb.max - c->u.pb.min);
        return prev;
    }
    case W32_PBM_GETPOS:
        return ctl_surf_progress(&c->surf) + c->u.pb.min;
    case W32_PBM_GETRANGE: {
        /* wp: which limit (TRUE = low); lp: PBRANGE* out */
        int32_t *out = (int32_t *)(uintptr_t)lp;
        if (!out) return 0;
        out[0] = c->u.pb.min;
        out[1] = c->u.pb.max;
        return wp ? c->u.pb.min : c->u.pb.max;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * Header (the column strip)
 * ===================================================================== */


static void ctl_header_paint(ctl_state_t *c, W32_HWND hwnd) {
    W32_PAINTSTRUCT ps;
    W32_HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;
    W32_RECT rc;
    GetClientRect(hwnd, &rc);
    const w32_comctl_palette_t *pal = w32_comctl_palette();
    W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    W32_HBRUSH fb = CreateSolidBrush(ag_to_colorref(pal->frame));
    SetBkMode(hdc, W32_TRANSPARENT);
    SetTextColor(hdc, ag_to_colorref(pal->text));
    int32_t x = 0;
    for (int i = 0; i < c->u.hd.n_items; i++) {
        W32_RECT ir = { x, 0, x + c->u.hd.cx[i], rc.bottom };
        FrameRect(hdc, &ir, fb);
        if (c->u.hd.text[i][0])
            TextOutA(hdc, x + 4, (rc.bottom - 10) / 2,
                     c->u.hd.text[i], (int32_t)strlen(c->u.hd.text[i]));
        x += c->u.hd.cx[i];
    }
    DeleteObject(fb);
    EndPaint(hwnd, &ps);
}

static W32_LRESULT ctl_header_proc(ctl_state_t *c, W32_HWND hwnd,
                                   W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    switch (msg) {
    case W32_WM_PAINT:
        ctl_header_paint(c, hwnd);
        return 0;
    case W32_HDM_INSERTITEMA:
    case W32_HDM_INSERTITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_HDITEMW *it = (W32_HDITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i > c->u.hd.n_items || c->u.hd.n_items >= HD_MAX_ITEMS)
            return -1;
        char label[32];
        if ((it->mask & W32_HDI_TEXT) && it->pszText) {
            if (msg == W32_HDM_INSERTITEMW)
                w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
        } else label[0] = 0;
        for (int j = c->u.hd.n_items; j > i; j--) {
            memcpy(c->u.hd.text[j], c->u.hd.text[j-1], sizeof c->u.hd.text[0]);
            c->u.hd.cx[j] = c->u.hd.cx[j-1];
            c->u.hd.fmt[j] = c->u.hd.fmt[j-1];
            c->u.hd.lparam[j] = c->u.hd.lparam[j-1];
        }
        ctl_strlcpy(c->u.hd.text[i], sizeof c->u.hd.text[0], label);
        c->u.hd.cx[i] = (it->mask & W32_HDI_WIDTH) ? it->cxy : 100;
        c->u.hd.fmt[i] = (it->mask & W32_HDI_FORMAT) ? (int32_t)it->fmt : (int32_t)W32_HDF_LEFT;
        c->u.hd.lparam[i] = (it->mask & W32_HDI_LPARAM) ? it->lParam : 0;
        c->u.hd.n_items++;
        return i;
    }
    case W32_HDM_DELETEITEM: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (i < 0 || i >= c->u.hd.n_items) return 0;
        for (int j = i; j + 1 < c->u.hd.n_items; j++) {
            memcpy(c->u.hd.text[j], c->u.hd.text[j+1], sizeof c->u.hd.text[0]);
            c->u.hd.cx[j] = c->u.hd.cx[j+1];
            c->u.hd.fmt[j] = c->u.hd.fmt[j+1];
            c->u.hd.lparam[j] = c->u.hd.lparam[j+1];
        }
        c->u.hd.n_items--;
        return 1;
    }
    case W32_HDM_GETITEMCOUNT:
        return c->u.hd.n_items;
    case W32_HDM_GETITEMA:
    case W32_HDM_GETITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_HDITEMW *it = (W32_HDITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i >= c->u.hd.n_items) return 0;
        if ((it->mask & W32_HDI_TEXT) && it->pszText && it->cchTextMax > 0) {
            if (msg == W32_HDM_GETITEMW)
                a_to_w16_buf((uint16_t *)(uintptr_t)it->pszText, (size_t)it->cchTextMax, c->u.hd.text[i]);
            else
                memcpy((void *)it->pszText, c->u.hd.text[i],
                       strlen(c->u.hd.text[i]) + 1);
        }
        if (it->mask & W32_HDI_WIDTH)  it->cxy = c->u.hd.cx[i];
        if (it->mask & W32_HDI_FORMAT) it->fmt = (uint32_t)c->u.hd.fmt[i];
        if (it->mask & W32_HDI_LPARAM) it->lParam = c->u.hd.lparam[i];
        return 1;
    }
    case W32_HDM_SETITEMA:
    case W32_HDM_SETITEMW: {
        int i = (int)(int32_t)(uint32_t)wp;
        W32_HDITEMW *it = (W32_HDITEMW *)(uintptr_t)lp;
        if (!it || i < 0 || i >= c->u.hd.n_items) return 0;
        if ((it->mask & W32_HDI_TEXT) && it->pszText) {
            char label[32];
            if (msg == W32_HDM_SETITEMW)
                w32_utf16z_to_utf8(it->pszText, label, (int32_t)sizeof label);
            else
                ctl_strlcpy(label, sizeof label, (const char *)it->pszText);
            ctl_strlcpy(c->u.hd.text[i], sizeof c->u.hd.text[0], label);
        }
        if (it->mask & W32_HDI_WIDTH)  c->u.hd.cx[i] = it->cxy;
        if (it->mask & W32_HDI_FORMAT) c->u.hd.fmt[i] = (int32_t)it->fmt;
        if (it->mask & W32_HDI_LPARAM) c->u.hd.lparam[i] = it->lParam;
        return 1;
    }
    case W32_HDM_LAYOUT: {
        /* Approximate but honest: the header occupies the top strip and
         * the caller's window rect is placed below it. */
        typedef struct { W32_RECT *prc; void *pwpos; } hd_layout_t;
        hd_layout_t *lay = (hd_layout_t *)(uintptr_t)lp;
        if (!lay || !lay->prc) return 0;
        int32_t h = (int32_t)lay->prc->bottom - lay->prc->top;
        lay->prc->top += 18;                 /* the strip we just drew */
        (void)h;
        return 1;
    }
    case W32_HDM_HITTEST: {
        typedef struct { W32_POINT pt; uint32_t flags; int32_t iItem; } hd_ht_t;
        hd_ht_t *ht = (hd_ht_t *)(uintptr_t)lp;
        if (!ht) return -1;
        int32_t x = 0;
        ht->iItem = -1;
        ht->flags = 0;
        for (int i = 0; i < c->u.hd.n_items; i++) {
            if (ht->pt.x >= x && ht->pt.x < x + c->u.hd.cx[i]) {
                ht->iItem = i;
                ht->flags = 2 /*HHT_ONHEADER*/;
                break;
            }
            x += c->u.hd.cx[i];
        }
        return ht->iItem;
    }
    case W32_WM_LBUTTONDOWN: {
        int32_t x = lp_x(lp);
        int32_t acc = 0;
        for (int i = 0; i < c->u.hd.n_items; i++) {
            acc += c->u.hd.cx[i];
            if (x < acc) {
                /* NMHEADER: NMHDR + iItem + iButton */
                struct { W32_NMHDR h; int32_t iItem; int32_t iButton; } nmh;
                memset(&nmh, 0, sizeof nmh);
                nmh.iItem = i;
                nmh.iButton = 0;
                notify_parent(hwnd, W32_HDN_ITEMCLICKW, &nmh.h);
                return 0;
            }
        }
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ===================================================================== *
 * ImageList
 * ===================================================================== *
 * Each cell is one ARGB copy (pool_alloc'd like the GDI bitmap pool:
 * bump with tail-trim, interior leak bounded per process).  Cells come
 * from the real pixel paths: AddMasked through the public GetDIBits,
 * ReplaceIcon through w32_gdi_icon_pixels, GetIcon back through
 * w32_gdi_icon_from_argb, Draw through a temp bitmap + BitBlt.
 */

#define IL_MAX_LISTS 16
#define IL_MAX_IMAGES 64
#define IL_POOL_U32 (256u * 1024u)

static uint32_t il_pool[IL_POOL_U32];
static size_t   il_pool_used;              /* in uint32 units */

static uint32_t *il_alloc(size_t n_u32) {
    n_u32 = (n_u32 + 3u) & ~(size_t)3u;
    if (il_pool_used + n_u32 > IL_POOL_U32) return 0;
    uint32_t *p = &il_pool[il_pool_used];
    il_pool_used += n_u32;
    return p;
}
static void il_free(uint32_t *p, size_t n_u32) {
    n_u32 = (n_u32 + 3u) & ~(size_t)3u;
    if (p + n_u32 == &il_pool[il_pool_used]) il_pool_used -= n_u32;
}

typedef struct {
    int      used;
    int32_t  cx, cy;
    int      n;
    uint32_t *px[IL_MAX_IMAGES];           /* cx*cy ARGB each, or 0 */
    size_t   alloc[IL_MAX_IMAGES];         /* pool block size, u32 */
} il_list_t;

static il_list_t il_lists[IL_MAX_LISTS];

static il_list_t *il_from_h(W32_HIMAGELIST himl) {
    il_list_t *l = (il_list_t *)(uintptr_t)himl;
    if (!l || (uintptr_t)l < 0x1000) return 0;
    /* handles are table slots; a bogus pointer must not crash us */
    if (l < &il_lists[0] || l >= &il_lists[IL_MAX_LISTS] || !l->used) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    return l;
}

W32ABI W32_HIMAGELIST ImageList_Create(int32_t cx, int32_t cy, W32_UINT flags,
                                       int32_t initial, int32_t grow) {
    (void)flags; (void)initial; (void)grow;
    if (cx <= 0 || cy <= 0 || cx > 128 || cy > 128) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int i = 0; i < IL_MAX_LISTS; i++) {
        if (!il_lists[i].used) {
            memset(&il_lists[i], 0, sizeof il_lists[i]);
            il_lists[i].used = 1;
            il_lists[i].cx = cx;
            il_lists[i].cy = cy;
            return (W32_HIMAGELIST)(uintptr_t)&il_lists[i];
        }
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

W32ABI W32_BOOL ImageList_Destroy(W32_HIMAGELIST himl) {
    il_list_t *l = il_from_h(himl);
    if (!l) return 0;
    for (int i = 0; i < IL_MAX_IMAGES; i++)
        if (l->px[i]) il_free(l->px[i], l->alloc[i]);
    memset(l, 0, sizeof *l);
    return 1;
}

static int il_store(il_list_t *l, int i, const uint32_t *argb) {
    size_t n = (size_t)l->cx * (size_t)l->cy;
    uint32_t *p = il_alloc(n);
    if (!p) return 0;
    memcpy(p, argb, n * 4);
    if (l->px[i]) il_free(l->px[i], l->alloc[i]);   /* replace path */
    l->px[i] = p;
    l->alloc[i] = n;
    return 1;
}

W32ABI int32_t ImageList_AddMasked(W32_HIMAGELIST himl, W32_HBITMAP hbm,
                                   uint32_t crMask) {
    il_list_t *l = il_from_h(himl);
    if (!l) return -1;
    if (l->n >= IL_MAX_IMAGES) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return -1;
    }
    W32_BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = l->cx;
    bi.bmiHeader.biHeight = -l->cy;       /* top-down, like our cells */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = W32_BI_RGB;
    uint32_t buf[128 * 128];
    if (!GetDIBits(0, hbm, 0, (W32_UINT)l->cy, buf, &bi, 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return -1;
    }
    /* the mask colour reads transparent (alpha 0) */
    uint32_t mask = (crMask & 0xFFu) << 16 | (crMask & 0xFF00u) |
                    ((crMask >> 16) & 0xFFu);
    for (int i = 0; i < l->cx * l->cy; i++)
        if ((buf[i] & 0xFFFFFFu) == mask) buf[i] &= 0x00FFFFFFu;
        else buf[i] |= 0xFF000000u;
    if (!il_store(l, l->n, buf)) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    return l->n++;
}

W32ABI int32_t ImageList_ReplaceIcon(W32_HIMAGELIST himl, int32_t i,
                                     W32_HICON icon) {
    il_list_t *l = il_from_h(himl);
    if (!l) return -1;
    int32_t iw, ih;
    const uint32_t *px = w32_gdi_icon_pixels(icon, &iw, &ih);
    if (!px) return -1;
    if (i < 0) {
        if (l->n >= IL_MAX_IMAGES) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return -1;
        }
        i = l->n;
    }
    if (i >= IL_MAX_IMAGES) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return -1;
    }
    uint32_t buf[128 * 128];
    /* scale-to-cell (nearest neighbour): icons commonly differ in size
     * from the list's cells, and the documented behaviour is a fit */
    for (int32_t y = 0; y < l->cy; y++) {
        int32_t sy = ih > 0 ? y * ih / l->cy : y;
        for (int32_t x = 0; x < l->cx; x++) {
            int32_t sx = iw > 0 ? x * iw / l->cx : x;
            buf[y * l->cx + x] = px[sy * iw + sx];
        }
    }
    if (!il_store(l, (int)i, buf)) return -1;
    if (i >= l->n) l->n = i + 1;
    return i;
}

W32ABI int32_t ImageList_GetImageCount(W32_HIMAGELIST himl) {
    il_list_t *l = il_from_h(himl);
    return l ? l->n : 0;
}

W32ABI W32_BOOL ImageList_GetIconSize(W32_HIMAGELIST himl, int32_t *cx,
                                      int32_t *cy) {
    il_list_t *l = il_from_h(himl);
    if (!l || !cx || !cy) return 0;
    *cx = l->cx; *cy = l->cy;
    return 1;
}

W32ABI W32_BOOL ImageList_SetIconSize(W32_HIMAGELIST himl, int32_t cx,
                                      int32_t cy) {
    il_list_t *l = il_from_h(himl);
    if (!l || cx <= 0 || cy <= 0 || cx > 128 || cy > 128) return 0;
    l->cx = cx; l->cy = cy;
    return 1;
}

W32ABI W32_HICON ImageList_GetIcon(W32_HIMAGELIST himl, int32_t i,
                                   W32_UINT flags) {
    il_list_t *l = il_from_h(himl);
    (void)flags;
    if (!l || i < 0 || i >= l->n || !l->px[i]) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return w32_gdi_icon_from_argb(l->cx, l->cy, l->px[i]);
}

W32ABI W32_BOOL ImageList_GetImageInfo(W32_HIMAGELIST himl, int32_t i,
                                       W32_IMAGEINFO *info) {
    il_list_t *l = il_from_h(himl);
    if (!l || !info || i < 0 || i >= l->n) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(info, 0, sizeof *info);
    /* hbmImage is minted as a real bitmap so callers can draw it; the
     * mask stays 0 (our cells carry alpha instead). */
    info->hbmImage = CreateBitmap(l->cx, l->cy, 1, 32, l->px[i]);
    info->rcImage.left = 0;
    info->rcImage.top = 0;
    info->rcImage.right = l->cx;
    info->rcImage.bottom = l->cy;
    return 1;
}

W32ABI W32_BOOL ImageList_Draw(W32_HIMAGELIST himl, int32_t i, W32_HDC hdc,
                               int32_t x, int32_t y, W32_UINT style) {
    il_list_t *l = il_from_h(himl);
    if (!l || i < 0 || i >= l->n || !l->px[i] || !hdc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    (void)style;      /* ILD_NORMAL/TRANSPARENT: cells carry alpha */
    /* Per-pixel through the public DC API: alpha-0 cells (the AddMasked
     * mask colour) stay transparent -- a straight BitBlt would stamp
     * them opaquely, and the A-7 engine's window blit has no ROP for
     * source alpha.  16x16 cells make the loop cheap. */
    for (int32_t yy = 0; yy < l->cy; yy++) {
        for (int32_t xx = 0; xx < l->cx; xx++) {
            uint32_t argb = l->px[i][yy * l->cx + xx];
            if (!(argb & 0xFF000000u)) continue;      /* masked out */
            uint32_t cref = (argb & 0x000000FFu) |            /* R */
                            ((argb >> 8) & 0xFFu) << 8 |       /* G */
                            ((argb >> 16) & 0xFFu) << 16;      /* B */
            SetPixel(hdc, x + xx, y + yy, cref);
        }
    }
    return 1;
}

W32ABI W32_BOOL ImageList_Remove(W32_HIMAGELIST himl, int32_t i) {
    il_list_t *l = il_from_h(himl);
    if (!l) return 0;
    if (i < 0) {                          /* remove everything */
        for (int j = 0; j < IL_MAX_IMAGES; j++)
            if (l->px[j]) { il_free(l->px[j], l->alloc[j]); l->px[j] = 0; }
        l->n = 0;
        return 1;
    }
    if (i >= l->n) return 0;
    il_free(l->px[i], l->alloc[i]);
    for (int j = i; j + 1 < l->n; j++) {
        l->px[j] = l->px[j+1];
        l->alloc[j] = l->alloc[j+1];
    }
    l->n--;
    l->px[l->n] = 0; l->alloc[l->n] = 0;
    return 1;
}

/* ---- the drag set ---------------------------------------------------- *
 * A real drag: a borderless popup window (class AuraDragWin) that paints
 * the tracked cell and moves with DragMove.  Receipts come from
 * GetPixel on the lock window's DC, so the drag image is visible pixels
 * on screen, exactly like the real thing. */

static const uint16_t drag_cls[] = {'A','u','r','a','D','r','a','g','W','i','n',0};
static struct {
    int used;
    W32_HWND hwnd;
    W32_HIMAGELIST himl;
    int index;
    W32_HWND lock;
    int shown;
} drag;

static W32_LRESULT W32ABI ctl_dragwin_proc(W32_HWND hwnd, W32_UINT msg,
                                           W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_PAINT && drag.used && drag.himl) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            ImageList_Draw(drag.himl, drag.index, hdc, 0, 0, W32_ILD_NORMAL);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

W32ABI W32_BOOL ImageList_BeginDrag(W32_HIMAGELIST himl, int32_t track,
                                    int32_t dxHotspot, int32_t dyHotspot) {
    (void)dxHotspot; (void)dyHotspot;     /* hotspot tracked, not offset */
    il_list_t *l = il_from_h(himl);
    if (!l || track < 0 || track >= l->n) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (drag.used) ImageList_EndDrag();
    w32_comctl_register_classes();
    drag.hwnd = CreateWindowExW(0, drag_cls, 0, W32_WS_POPUP,
                                -32000, -32000, l->cx, l->cy,
                                0, 0, GetModuleHandleW(0), 0);
    if (!drag.hwnd) return 0;
    drag.used = 1;
    drag.himl = himl;
    drag.index = track;
    drag.shown = 0;
    return 1;
}

W32ABI W32_BOOL ImageList_DragEnter(W32_HWND lock, int32_t x, int32_t y) {
    if (!drag.used) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    drag.lock = lock;
    il_list_t *l = il_from_h(drag.himl);
    MoveWindow(drag.hwnd, x, y, l ? l->cx : 24, l ? l->cy : 24, 0);
    ShowWindow(drag.hwnd, W32_SW_SHOWNOACTIVATE);
    drag.shown = 1;
    return 1;
}

W32ABI W32_BOOL ImageList_DragMove(int32_t x, int32_t y) {
    if (!drag.used || !drag.shown) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    il_list_t *l = il_from_h(drag.himl);
    MoveWindow(drag.hwnd, x, y, l ? l->cx : 24, l ? l->cy : 24, 1);
    return 1;
}

W32ABI W32_BOOL ImageList_DragShowNolock(W32_BOOL show) {
    if (!drag.used) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    ShowWindow(drag.hwnd, show ? W32_SW_SHOWNOACTIVATE : W32_SW_HIDE);
    return 1;
}

W32ABI void ImageList_EndDrag(void) {
    if (!drag.used) return;
    if (drag.hwnd) DestroyWindow(drag.hwnd);
    memset(&drag, 0, sizeof drag);
}

/* ===================================================================== *
 * The subclass engine (ordinals 410-413)
 * ===================================================================== *
 * Entries stack per (hwnd, id): a message arriving from the system runs
 * the entries newest-first (the trampoline is the window's top proc --
 * SetWindowLongPtrW pushed it), and DefSubclassProc walks the SAME
 * message down the stack to the proc that was top when the first
 * subclass was installed.  A nested SendMessage inside a subclass proc
 * starts at the top again, like Win32. */

#define SUB_MAX 24
typedef struct {
    W32_HWND hwnd;
    uint64_t id;
    W32_SUBCLASSPROC proc;
    int64_t  data;
    uint32_t seq;                 /* install order: bigger == newer  */
    int      in_use;
} sub_t;
static sub_t   subs[SUB_MAX];
static uint32_t sub_seq_next;
static int     sub_depth = -1;    /* executing entry index, -1 none */

/* One trampoline install per window (RemoveWindowSubclass keeps it:
 * with no entries left it forwards everything to DefWindowProcW). */
static W32_HWND sub_installed[32];
static int sub_is_installed(W32_HWND hwnd) {
    for (int i = 0; i < 32; i++)
        if (sub_installed[i] == hwnd) return 1;
    return 0;
}
static void sub_mark_installed(W32_HWND hwnd) {
    for (int i = 0; i < 32; i++) {
        if (sub_installed[i] == hwnd) return;
        if (!sub_installed[i]) { sub_installed[i] = hwnd; return; }
    }
}

static int sub_find(W32_HWND hwnd, uint64_t id) {
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].in_use && subs[i].hwnd == hwnd && subs[i].id == id) return i;
    return -1;
}

static int sub_newest(W32_HWND hwnd) {
    int best = -1;
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].in_use && subs[i].hwnd == hwnd &&
            (best < 0 || subs[i].seq > subs[best].seq)) best = i;
    return best;
}

static W32_LRESULT W32ABI sub_trampoline(W32_HWND hwnd, W32_UINT msg,
                                         W32_WPARAM wp, W32_LPARAM lp) {
    int top = sub_newest(hwnd);
    int saved = sub_depth;
    sub_depth = top;
    W32_LRESULT r = top >= 0
        ? subs[top].proc(hwnd, msg, wp, lp, subs[top].id, subs[top].data)
        : DefWindowProcW(hwnd, msg, wp, lp);
    sub_depth = saved;
    return r;
}

W32ABI W32_BOOL SetWindowSubclass(W32_HWND hwnd, W32_SUBCLASSPROC proc,
                                  uint64_t id, int64_t data) {
    if (!hwnd || !proc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (w32_win_index_from_hwnd(hwnd) < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    int slot = sub_find(hwnd, id);
    int existed = slot >= 0;
    if (!existed) {
        for (int i = 0; i < SUB_MAX; i++)
            if (!subs[i].in_use) { slot = i; break; }
        if (slot < 0) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        subs[slot].in_use = 1;
        subs[slot].hwnd = hwnd;
        subs[slot].id = id;
        subs[slot].seq = ++sub_seq_next;
    }
    subs[slot].proc = proc;
    subs[slot].data = data;
    /* first subclass on this window: push the trampoline once */
    if (!existed && !sub_is_installed(hwnd)) {
        SetWindowLongPtrW(hwnd, W32_GWL_WNDPROC,
                          (W32_LRESULT)(uintptr_t)sub_trampoline);
        sub_mark_installed(hwnd);
    }
    return 1;
}

W32ABI W32_BOOL RemoveWindowSubclass(W32_HWND hwnd, uint64_t id) {
    int slot = sub_find(hwnd, id);
    if (slot < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    subs[slot].in_use = 0;
    /* the trampoline stays installed: with no entries left it forwards
     * everything to DefWindowProcW, and a later SetWindowSubclass
     * reuses it without pushing a second proc slot. */
    return 1;
}

W32ABI W32_BOOL GetWindowSubclass(W32_HWND hwnd, W32_SUBCLASSPROC proc,
                                  uint64_t id, int64_t *data) {
    int slot = sub_find(hwnd, id);
    if (slot < 0 || (proc && subs[slot].proc != proc)) return 0;
    if (data) *data = subs[slot].data;
    return 1;
}

W32ABI W32_LRESULT DefSubclassProc(W32_HWND hwnd, W32_UINT msg,
                                   W32_WPARAM wp, W32_LPARAM lp) {
    /* run the entry OLDER than the one executing, newest-first among
     * the older ones; below the oldest, the window's own chain. */
    if (sub_depth < 0 || !subs[sub_depth].in_use ||
        subs[sub_depth].hwnd != hwnd)
        return DefWindowProcW(hwnd, msg, wp, lp);
    uint32_t cur = subs[sub_depth].seq;
    int below = -1;
    for (int i = 0; i < SUB_MAX; i++)
        if (subs[i].in_use && subs[i].hwnd == hwnd && subs[i].seq < cur &&
            (below < 0 || subs[i].seq > subs[below].seq)) below = i;
    if (below < 0) return DefWindowProcW(hwnd, msg, wp, lp);
    int saved = sub_depth;
    sub_depth = below;
    W32_LRESULT r = subs[below].proc(hwnd, msg, wp, lp,
                                     subs[below].id, subs[below].data);
    sub_depth = saved;
    return r;
}

/* ===================================================================== *
 * _TrackMouseEvent / LoadIconWithScaleDown / InitCommonControls
 * ===================================================================== */

W32ABI W32_BOOL _TrackMouseEvent(W32_TRACKMOUSEEVENT *evt) {
    return TrackMouseEvent(evt);           /* the W32A-5 USER32 entry */
}

W32ABI int32_t LoadIconWithScaleDown(W32_HINSTANCE inst, const uint16_t *name,
                                     int32_t cx, int32_t cy, W32_HICON *out) {
    if (!out || cx <= 0 || cy <= 0 || cx > 128 || cy > 128) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0x80070057u;                /* E_INVALIDARG */
    }
    /* the A-6 resource path: LoadIconW decodes and caches, then the
     * pixels are read back and scaled into the requested cell */
    W32_HICON hi = (W32_HICON)LoadIconW((void *)inst, name);
    if (!hi) {
        w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0x8007007Eu;                /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
    }
    int32_t iw, ih;
    const uint32_t *px = w32_gdi_icon_pixels(hi, &iw, &ih);
    if (!px) return 0x80004005u;           /* E_FAIL */
    uint32_t buf[128 * 128];
    for (int32_t y = 0; y < cy; y++) {
        int32_t sy = ih > 0 ? y * ih / cy : y;
        for (int32_t x = 0; x < cx; x++) {
            int32_t sx = iw > 0 ? x * iw / cx : x;
            buf[y * cx + x] = px[sy * iw + sx];
        }
    }
    *out = w32_gdi_icon_from_argb(cx, cy, buf);
    return *out ? 0 /* S_OK */ : 0x80004005u;
}

void W32ABI InitCommonControls(void) {
    w32_comctl_register_classes();
}

W32ABI W32_BOOL InitCommonControlsEx(const W32_INITCOMMONCONTROLSEX *icc) {
    if (!icc || icc->dwSize < sizeof *icc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Every class registers regardless of the mask: the mask documents
     * intent, the create-any-time contract (documented above) means the
     * classes must exist whenever the caller gets around to them. */
    w32_comctl_register_classes();
    return 1;
}

/* ===================================================================== *
 * CreateToolbarEx / CreateStatusWindowW
 * ===================================================================== */

W32ABI W32_HWND CreateToolbarEx(W32_HWND hwnd, W32_DWORD ws, W32_UINT wID,
                                int32_t nBitmaps, W32_HINSTANCE hBMInst,
                                uint64_t wBMID, const W32_TBBUTTON *buttons,
                                int32_t nButtons, int32_t dxButton,
                                int32_t dyButton, int32_t dxBitmap,
                                int32_t dyBitmap, W32_UINT uStructSize) {
    (void)nBitmaps; (void)hBMInst; (void)wBMID; (void)dxBitmap;
    (void)dyBitmap; (void)uStructSize;
    w32_comctl_register_classes();
    W32_HWND tb = CreateWindowExW(0, W32_WC_TOOLBARW, 0,
                                  ws | W32_WS_CHILD | W32_WS_VISIBLE,
                                  0, 0, 400, dyButton ? dyButton + 4 : 26,
                                  hwnd, (W32_HMENU)(uintptr_t)wID,
                                  GetModuleHandleW(0), 0);
    if (!tb) return 0;
    if (dxButton || dyButton)
        SendMessageW(tb, W32_TB_SETBUTTONSIZE, 0,
                     (W32_LPARAM)(uint32_t)
                     ((uint32_t)(dyButton & 0xFFFF) << 16 |
                      (uint32_t)(dxButton & 0xFFFF)));
    if (nButtons > 0 && buttons)
        SendMessageW(tb, W32_TB_ADDBUTTONSW, (W32_WPARAM)(uint32_t)nButtons,
                     (W32_LPARAM)(uintptr_t)buttons);
    return tb;
}

W32ABI W32_HWND CreateStatusWindowW(int32_t style, const uint16_t *text,
                                    W32_HWND parent, W32_UINT id) {
    w32_comctl_register_classes();
    W32_HWND sb = CreateWindowExW(0, W32_WC_STATUSBARW,
                                  text, (W32_DWORD)style | W32_WS_CHILD |
                                  W32_WS_VISIBLE,
                                  0, 0, 400, 22, parent,
                                  (W32_HMENU)(uintptr_t)id,
                                  GetModuleHandleW(0), 0);
    return sb;
}

/* ===================================================================== *
 * PropertySheetW
 * ===================================================================== *
 * The sheet is a real frame window (class AuraPSFrame) carrying:
 *   - a real WC_TABCONTROL child whose clicks switch pages,
 *   - one real page window per PROPSHEETPAGE (class AuraPSPage, a
 *     WS_CHILD comctl window), each hosting the caller's dialog proc
 *     through a bridge that honours the FALSE-falls-through dialog
 *     contract (the A-6 engine's dlg_frameproc pattern),
 *   - real button windows (class AuraPSBtn) for OK/Cancel/Apply whose
 *     clicks arrive as WM_COMMAND, exactly like Win32,
 *   - PSN_* notifications with the documented lParam direction
 *     (TRUE = OK/close, FALSE = Apply) and the PSNRET verdicts.
 * Page contents come from a dialog template (the A-6 resource path:
 * FindResource/LoadResource/LockResource, or PSP_DLGINDIRECT for an
 * in-memory template), expanded into real child windows. */

#define PS_SHEETS 4
#define PS_PAGES  8

typedef struct {
    int          used;
    W32_HWND     frame;
    W32_HWND     tab;
    W32_HWND     pages[PS_PAGES];
    W32_DLGPROCP procs[PS_PAGES];
    int64_t      lparams[PS_PAGES];
    int          n_pages;
    int          active;
    int          ended;
    uint8_t      inited[PS_PAGES];  /* WM_INITDIALOG sent on 1st show */
    W32_INT_PTR  result;
} ps_sheet_t;

static ps_sheet_t ps_sheets[PS_SHEETS];

static ps_sheet_t *ps_by_frame(W32_HWND frame) {
    for (int i = 0; i < PS_SHEETS; i++)
        if (ps_sheets[i].used && ps_sheets[i].frame == frame) return &ps_sheets[i];
    return 0;
}
static ps_sheet_t *ps_by_page(W32_HWND page) {
    for (int i = 0; i < PS_SHEETS; i++)
        if (ps_sheets[i].used)
            for (int j = 0; j < ps_sheets[i].n_pages; j++)
                if (ps_sheets[i].pages[j] == page) return &ps_sheets[i];
    return 0;
}
static int ps_page_index(ps_sheet_t *s, W32_HWND page) {
    for (int j = 0; j < s->n_pages; j++)
        if (s->pages[j] == page) return j;
    return -1;
}

/* PSN_* to one page, through WM_NOTIFY (the documented carrier). */
static W32_LRESULT ps_notify(ps_sheet_t *s, int page, uint32_t code,
                             int64_t lparam) {
    if (!s || page < 0 || page >= s->n_pages) return 0;
    W32_PSHNOTIFY pshn;
    memset(&pshn, 0, sizeof pshn);
    pshn.hdr.hwndFrom = s->frame;
    pshn.hdr.idFrom   = 0;
    pshn.hdr.code     = code;
    pshn.iPage        = page;
    pshn.lParam       = lparam;
    return SendMessageW(s->pages[page], W32_WM_NOTIFY,
                        (W32_WPARAM)0, (W32_LPARAM)(intptr_t)&pshn);
}

/* Switch pages: KILLACTIVE (TRUE = prevent), hide, show, SETACTIVE. */
static W32_BOOL ps_switch(ps_sheet_t *s, int to) {
    if (!s || to < 0 || to >= s->n_pages || to == s->active) return 0;
    if (s->active >= 0 && ps_notify(s, s->active, W32_PSN_KILLACTIVE, 0)) {
        /* page refused the switch */
        return 0;
    }
    if (s->active >= 0) ShowWindow(s->pages[s->active], W32_SW_HIDE);
    s->active = to;
    ShowWindow(s->pages[to], W32_SW_SHOW);
    SendMessageW(s->tab, W32_TCM_SETCURSEL, (W32_WPARAM)(uint32_t)to, 0);
    /* WM_INITDIALOG rides the first activation (the documented
     * property-sheet timing, not the A-6 modal dialog's) */
    if (!s->inited[to]) {
        s->inited[to] = 1;
        SendMessageW(s->pages[to], W32_WM_INITDIALOG, 0,
                     (W32_LPARAM)s->lparams[to]);
    }
    ps_notify(s, to, W32_PSN_SETACTIVE, 0);
    return 1;
}

/* PSN_APPLY to every page.  lParam: TRUE = OK/Close, FALSE = Apply.
 * Returns 0 if any page answered PSNRET_INVALID (sheet stays open). */
static int ps_apply_all(ps_sheet_t *s, int64_t lparam) {
    int ok = 1;
    for (int i = 0; i < s->n_pages; i++) {
        W32_LRESULT r = ps_notify(s, i, W32_PSN_APPLY, lparam);
        if (r == W32_PSNRET_INVALID || r == W32_PSNRET_INVALID_NOCHANGEPAGE)
            ok = 0;
    }
    return ok;
}

static void ps_finish(ps_sheet_t *s, W32_INT_PTR result) {
    s->result = result;
    s->ended = 1;
    PostQuitMessage(0);
}

/* ---- the frame proc: buttons, PSM_*, tab notifications ---------------- */

#define W32_PSM_GETCURRENTPAGEHWND (W32_WM_USER + 12)   /* 0x040C */

static W32_LRESULT W32ABI ps_frame_wndproc(W32_HWND hwnd, W32_UINT msg,
                                           W32_WPARAM wp, W32_LPARAM lp) {
    ps_sheet_t *s = ps_by_frame(hwnd);
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case W32_WM_PAINT: {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            const w32_comctl_palette_t *pal = w32_comctl_palette();
            W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case W32_WM_COMMAND: {
        uint32_t id = (uint32_t)wp & 0xFFFFu;
        if (id == (uint32_t)W32_IDOK) {
            if (ps_apply_all(s, 1)) ps_finish(s, W32_IDOK);
            return 0;
        }
        if (id == (uint32_t)W32_IDCANCEL) {
            for (int i = 0; i < s->n_pages; i++)
                ps_notify(s, i, W32_PSN_RESET, 0);
            ps_finish(s, W32_IDCANCEL);
            return 0;
        }
        if (id == W32_ID_APPLY_NOW) {
            ps_apply_all(s, 0);
            return 0;
        }
        return 0;
    }
    case W32_WM_CLOSE:
        for (int i = 0; i < s->n_pages; i++)
            ps_notify(s, i, W32_PSN_RESET, 0);
        ps_finish(s, W32_IDCANCEL);
        return 0;
    case W32_WM_NOTIFY: {
        W32_NMHDR *nm = (W32_NMHDR *)(uintptr_t)lp;
        if (nm && nm->hwndFrom == s->tab && nm->code == W32_TCN_SELCHANGE) {
            int cur = (int)SendMessageW(s->tab, W32_TCM_GETCURSEL, 0, 0);
            if (cur >= 0) ps_switch(s, cur);
        }
        return 0;
    }
    case W32_PSM_SETCURSEL:
        return ps_switch(s, (int)(int32_t)(uint32_t)wp);
    case W32_PSM_PRESSBUTTON: {
        switch ((uint32_t)wp) {
        case W32_PSBTN_OK:
            if (ps_apply_all(s, 1)) ps_finish(s, W32_IDOK);
            return 1;
        case W32_PSBTN_CANCEL:
            for (int i = 0; i < s->n_pages; i++)
                ps_notify(s, i, W32_PSN_RESET, 0);
            ps_finish(s, W32_IDCANCEL);
            return 1;
        case W32_PSBTN_APPLYNOW:
            return (W32_LRESULT)(int64_t)ps_apply_all(s, 0);
        default:
            return 0;               /* BACK/NEXT/FINISH: wizard-only */
        }
    }
    case W32_PSM_GETCURRENTPAGEHWND:
        return (W32_LRESULT)(uintptr_t)
               (s->active >= 0 ? s->pages[s->active] : 0);
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ---- the page host class proc ----------------------------------------- */

static W32_LRESULT ctl_pspage_proc(ctl_state_t *c, W32_HWND hwnd,
                                   W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    (void)c;
    if (msg == W32_WM_PAINT) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            const w32_comctl_palette_t *pal = w32_comctl_palette();
            W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The bridge above the class proc: run the caller's dialog proc under
 * the dialog contract (FALSE falls through; INITDIALOG returns TRUE). */
static W32_LRESULT W32ABI ps_page_bridge(W32_HWND hwnd, W32_UINT msg,
                                         W32_WPARAM wp, W32_LPARAM lp) {
    ps_sheet_t *s = ps_by_page(hwnd);
    if (s) {
        int idx = ps_page_index(s, hwnd);
        if (idx >= 0 && s->procs[idx]) {
            W32_LRESULT r = s->procs[idx](hwnd, msg, wp, lp);
            if (msg == W32_WM_INITDIALOG) return 1;
            if (r) return 0;                    /* handled */
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- sheet buttons and template items ---------------------------------- */

static const uint16_t ps_frame_cls[] = {'A','u','r','a','P','S','F','r','a','m','e',0};
static const uint16_t ps_btn_cls[]   = {'A','u','r','a','P','S','B','t','n',0};
static const uint16_t ps_item_cls[]  = {'A','u','r','a','P','S','I','t','e','m',0};

static W32_LRESULT W32ABI ps_button_proc(W32_HWND hwnd, W32_UINT msg,
                                         W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_PAINT) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            const w32_comctl_palette_t *pal = w32_comctl_palette();
            W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            W32_HBRUSH fb = CreateSolidBrush(ag_to_colorref(pal->frame));
            W32_RECT fr = rc;
            FrameRect(hdc, &fr, fb);
            DeleteObject(fb);
            uint16_t wtext[64];
            int n = GetWindowTextW(hwnd, wtext, 64);
            if (n > 0) {
                char label[64];
                w32_utf16z_to_utf8(wtext, label, (int32_t)sizeof label);
                SetBkMode(hdc, W32_TRANSPARENT);
                SetTextColor(hdc, ag_to_colorref(pal->text));
                TextOutA(hdc, rc.left + 6, (rc.bottom - 10) / 2, label,
                         (int32_t)strlen(label));
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    if (msg == W32_WM_LBUTTONDOWN) {
        W32_HWND parent = GetParent(hwnd);
        if (parent)
            PostMessageW(parent, W32_WM_COMMAND,
                         (W32_WPARAM)(uint32_t)
                         ((uint32_t)w32_comctl_ctrl_id(hwnd) & 0xFFFFu),
                         (W32_LPARAM)(uintptr_t)hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static W32_LRESULT W32ABI ps_item_proc(W32_HWND hwnd, W32_UINT msg,
                                       W32_WPARAM wp, W32_LPARAM lp) {
    if (msg == W32_WM_PAINT) {
        W32_PAINTSTRUCT ps;
        W32_HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            W32_RECT rc;
            GetClientRect(hwnd, &rc);
            const w32_comctl_palette_t *pal = w32_comctl_palette();
            W32_HBRUSH bg = CreateSolidBrush(ag_to_colorref(pal->face));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            uint16_t wtext[64];
            int n = GetWindowTextW(hwnd, wtext, 64);
            if (n > 0) {
                char label[64];
                w32_utf16z_to_utf8(wtext, label, (int32_t)sizeof label);
                SetBkMode(hdc, W32_TRANSPARENT);
                SetTextColor(hdc, ag_to_colorref(pal->text));
                TextOutA(hdc, rc.left + 2, rc.top + 2, label,
                         (int32_t)strlen(label));
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- template walk (the A-6 item grammar, expanded here) --------------- */

static const uint16_t *ps_skip_z(const uint16_t *p) {
    if (!p) return 0;
    if (p[0] == 0xFFFF) return p + 2;
    while (*p) p++;
    return p + 1;
}

static const uint16_t *ps_after_header(const W32_DLGTEMPLATE *t) {
    const uint16_t *p = (const uint16_t *)((const uint8_t *)t + sizeof *t);
    p = ps_skip_z(p);                    /* menu */
    p = ps_skip_z(p);                    /* class */
    p = ps_skip_z(p);                    /* title */
    if (t->style & 0x40u /* DS_SETFONT */) {
        p += 3;                          /* point, weight, italic|charset */
        p = ps_skip_z(p);                /* typeface */
    }
    while (((uintptr_t)p) & 2) p++;
    return p;
}

static W32_HWND ps_expand_page(W32_HWND page, W32_HINSTANCE inst,
                               const W32_DLGTEMPLATE *t) {
    const uint16_t *p = ps_after_header(t);
    for (uint32_t i = 0; i < t->items && p; i++) {
        while (((uintptr_t)p) & 3) p++;  /* each item is DWORD-aligned */
        /* DLGITEMTEMPLATE: style, exStyle, x, y, cx, cy, id (all 16-bit
         * words except the leading pair of 32-bit words).  cdit bounds
         * the walk -- a zero style word is legal (we read the full pair). */
        uint32_t style  = (uint32_t)p[0] | ((uint32_t)p[1] << 16);
        uint32_t exst   = (uint32_t)p[2] | ((uint32_t)p[3] << 16);
        int32_t  x      = (int16_t)p[4], y = (int16_t)p[5];
        int32_t  cx     = (int16_t)p[6], cy = (int16_t)p[7];
        uint16_t id     = p[8];
        p += 9;
        const uint16_t *cls;
        if (p[0] == 0xFFFF) {
            /* ordinal classes: Button gets real button behaviour, the
             * other five draw as items; a class named by string must be
             * one the caller registered. */
            uint16_t ord = p[1];
            p += 2;
            cls = (ord == 0x0080) ? ps_btn_cls : ps_item_cls;
        } else {
            cls = p;
            p = ps_skip_z(p);
        }
        const uint16_t *title = p;
        p = ps_skip_z(p);
        uint16_t cd = *p++;
        p += (cd + 1) / 2;
        (void)exst;
        (void)style;
        CreateWindowExW(0, cls, title,
                        W32_WS_CHILD | W32_WS_VISIBLE,
                        x * 2, y * 2, cx * 2, cy * 2,
                        page, (W32_HMENU)(uintptr_t)id, inst, 0);
    }
    return page;
}

/* ---- PropertySheetW ----------------------------------------------------- */

W32ABI W32_INT_PTR PropertySheetW(W32_PROPSHEETHEADERW *header) {
    if (!header || header->dwSize < sizeof *header || !header->ppsp ||
        header->nPages == 0 || header->nPages > PS_PAGES) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (!(header->dwFlags & W32_PSH_PROPSHEETPAGE)) {
        /* an HPROPSHEETPAGE array needs CreatePropertySheetPage, which
         * is not in the 27 (refused by name, not silently faked) */
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return -1;
    }

    w32_comctl_register_classes();

    ps_sheet_t *s = 0;
    for (int i = 0; i < PS_SHEETS; i++)
        if (!ps_sheets[i].used) { s = &ps_sheets[i]; break; }
    if (!s) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return -1; }
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->active = -1;
    s->result = -1;

    s->frame = CreateWindowExW(0, ps_frame_cls, header->pszCaption,
                               W32_WS_OVERLAPPEDWINDOW | W32_WS_VISIBLE,
                               120, 80, 420, 330, header->hwndParent, 0,
                               header->hInstance, 0);
    if (!s->frame) { s->used = 0; return -1; }

    s->tab = CreateWindowExW(0, W32_WC_TABCONTROLW, 0,
                             W32_WS_CHILD | W32_WS_VISIBLE,
                             4, 4, 404, 26, s->frame, 0,
                             header->hInstance, 0);
    if (!s->tab) { DestroyWindow(s->frame); s->used = 0; return -1; }

    const W32_PROPSHEETPAGEW *pages =
        (const W32_PROPSHEETPAGEW *)header->ppsp;

    for (uint32_t i = 0; i < header->nPages; i++) {
        const W32_PROPSHEETPAGEW *pg = &pages[i];
        const W32_DLGTEMPLATE *tmpl = 0;
        if (pg->dwFlags & W32_PSP_DLGINDIRECT) {
            tmpl = (const W32_DLGTEMPLATE *)pg->pResource;
        } else {
            /* pResource holds the template NAME in the non-indirect case
             * (the documented union): a resource id or name string. */
            const uint16_t *name = (const uint16_t *)pg->pResource;
            void *h = FindResourceW((void *)pg->hInstance, name,
                                    (const uint16_t *)(uintptr_t)W32_RT_DIALOG);
            if (h) {
                void *hr = LoadResource((void *)pg->hInstance, h);
                tmpl = hr ? (const W32_DLGTEMPLATE *)LockResource(hr) : 0;
            }
        }
        if (!tmpl) {
            w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND);
            DestroyWindow(s->frame);
            s->used = 0;
            return -1;
        }
        int32_t pw = tmpl->cx * 2, ph = tmpl->cy * 2;
        if (pw < 40) pw = 200;
        if (ph < 20) ph = 200;
        W32_HWND page = CreateWindowExW(0, ps_page_cls, pg->pszTitle,
                                        W32_WS_CHILD,
                                        8, 32, pw, ph, s->frame, 0,
                                        pg->hInstance, 0);
        if (!page) { DestroyWindow(s->frame); s->used = 0; return -1; }
        ps_expand_page(page, pg->hInstance, tmpl);
        SetWindowLongPtrW(page, W32_GWL_WNDPROC,
                          (W32_LRESULT)(uintptr_t)ps_page_bridge);
        s->pages[i] = page;
        s->procs[i] = pg->pfnDlgProc;
        s->lparams[i] = pg->lParam;
        s->n_pages = (int)i + 1;
        W32_TCITEMW tci;
        memset(&tci, 0, sizeof tci);
        tci.mask = W32_TCIF_TEXT;
        tci.pszText = (uint16_t *)(uintptr_t)pg->pszTitle;
        SendMessageW(s->tab, W32_TCM_INSERTITEMW,
                     (W32_WPARAM)(uint32_t)i, (W32_LPARAM)(intptr_t)&tci);
    }

    /* the sheet's own buttons */
    struct { int32_t x; int32_t id; const uint16_t *label; } btns[3] = {
        { 300, W32_IDOK,         (const uint16_t *)(uintptr_t)0 },
        { 354, W32_IDCANCEL,     0 },
        { 246, (int32_t)W32_ID_APPLY_NOW, 0 },
    };
    static const uint16_t lab_ok[]  = {'O','K',0};
    static const uint16_t lab_can[] = {'C','a','n','c','e','l',0};
    static const uint16_t lab_app[] = {'A','p','p','l','y',0};
    btns[0].label = lab_ok; btns[1].label = lab_can; btns[2].label = lab_app;
    for (int i = 0; i < 3; i++) {
        if (btns[i].id == (int32_t)W32_ID_APPLY_NOW &&
            (header->dwFlags & W32_PSH_NOAPPLYNOW)) continue;
        CreateWindowExW(0, ps_btn_cls, btns[i].label,
                        W32_WS_CHILD | W32_WS_VISIBLE,
                        btns[i].x, 272, 54, 24, s->frame,
                        (W32_HMENU)(uintptr_t)(uint32_t)btns[i].id,
                        header->hInstance, 0);
    }

    /* activate the first page and start the modal loop (A-6 pattern) */
    s->active = 0;
    s->inited[0] = 1;
    ShowWindow(s->pages[0], W32_SW_SHOW);
    SendMessageW(s->tab, W32_TCM_SETCURSEL, 0, 0);
    SendMessageW(s->pages[0], W32_WM_INITDIALOG, 0,
                 (W32_LPARAM)s->lparams[0]);
    ps_notify(s, 0, W32_PSN_SETACTIVE, 0);

    W32_MSG m;
    while (!s->ended && GetMessageW(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    W32_INT_PTR result = s->result;
    DestroyWindow(s->frame);
    memset(s, 0, sizeof *s);
    return result;
}

/* ===================================================================== *
 * Class registration (idempotent): the eight documented control classes,
 * the property-sheet plumbing (page host, frame, button, item) and the
 * drag-image window.  InitCommonControls(Ex) and the two create helpers
 * all land here, so the classes exist whenever a caller creates one --
 * the documented create-any-time contract.
 * ===================================================================== */

static int ctl_classes_done;

void w32_comctl_register_classes(void) {
    if (ctl_classes_done) return;
    ctl_classes_done = 1;
    for (size_t i = 0; i < W32_ARRAY_COUNT(ctl_classes); i++) {
        W32_WNDCLASSEXW c;
        memset(&c, 0, sizeof c);
        c.cbSize = (uint32_t)sizeof c;
        c.lpszClassName = ctl_classes[i].name;
        c.lpfnWndProc = ctl_classes[i].proc;
        /* Registered through the comctl marker so WS_CHILD is admitted
         * for exactly these classes.  An application that already
         * registered the name keeps its own class -- the duplicate
         * error is swallowed on purpose and the control still works
         * through that class's proc (documented behaviour). */
        w32_win_register_comctl_class(&c);
    }
    static const struct {
        const uint16_t *name;
        W32_WNDPROC proc;
    } aux_classes[] = {
        { ps_page_cls,  ctl_proc_PSPAGE },
        { ps_frame_cls, ps_frame_wndproc },
        { ps_btn_cls,   ps_button_proc },
        { ps_item_cls,  ps_item_proc },
        { drag_cls,     ctl_dragwin_proc },
    };
    for (size_t i = 0; i < W32_ARRAY_COUNT(aux_classes); i++) {
        W32_WNDCLASSEXW c;
        memset(&c, 0, sizeof c);
        c.cbSize = (uint32_t)sizeof c;
        c.lpszClassName = aux_classes[i].name;
        c.lpfnWndProc = aux_classes[i].proc;
        w32_win_register_comctl_class(&c);
    }
}

int w32_comctl_is_class(const uint16_t *clsname) {
    if (!clsname) return 0;
    static const uint16_t *const all[] = {
        W32_WC_TOOLBARW,    W32_WC_STATUSBARW,
        W32_WC_LISTVIEWW,   W32_WC_TREEVIEWW,
        W32_WC_TABCONTROLW, W32_WC_TOOLTIPW,
        W32_WC_PROGRESSW,   W32_WC_HEADERW,
        ps_page_cls, ps_frame_cls, ps_btn_cls, ps_item_cls, drag_cls,
    };
    for (size_t i = 0; i < W32_ARRAY_COUNT(all); i++) {
        size_t ln = 0;
        while (all[i][ln]) ln++;
        if (!memcmp(clsname, all[i], (ln + 1) * sizeof(uint16_t))) return 1;
    }
    return 0;
}
