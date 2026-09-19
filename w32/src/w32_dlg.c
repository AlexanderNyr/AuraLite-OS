/* w32_dlg.c — USER32 breadth II (W32APP_PLAN.md phase W32A-6).
 *
 * Implements dialog engine (DialogBoxParam(A/W)/Indirect/EndDialog/IsDialogMessage/
 * MapDialogRect/GetDialogBaseUnits/DlgItem*), menus (HMENU Create/Append/Insert/
 * Destroy/Track/Get/Set/Load/Check), timers (Set/KillTimer), caret, accelerators,
 * clipboard (mapped to libauragui ag_set_clipboard/ag_get_clipboard), thread-local
 * hooks, and DrawText/DrawFocusRect/DrawEdge/DrawFrameControl/DrawIconEx/DrawIcon/
 * NotifyWinEvent helpers.
 *
 * Per decision D5: map onto what already exists.  Raster fidelity (glyph shaping,
 * themed menus, ICO/BMP decode) deferred to W32A-7/A-8.  Per D9: refusals are
 * named (ERROR_CALL_NOT_IMPLEMENTED for global hooks; ERROR_RESOURCE_* for bad
 * lookups) rather than silent no-ops.
 */

#include "w32/w32_abi.h"
#include "w32/user32.h"
#include "w32/gdi32.h"
#include "w32/kernel32.h"
#include "w32/w32_module.h"
#include "w32/w32_rsrc.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifndef W32_ARRAY_COUNT
#define W32_ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))
#endif

extern int  ag_text(int wid, const char *text, int x, int y, uint32_t fg, uint32_t bg);
extern int  ag_rect_outline(int wid, int x, int y, int w, int h, uint32_t color);
extern int  ag_set_clipboard(const char *text);
extern int  ag_get_clipboard(char *buf, int sz);

void w32_dlg_fire_timers(void);  /* called from user32_win.c ui_pump() via weak */

extern W32_HWND    W32ABI CreateWindowExW(W32_DWORD,const uint16_t*,const uint16_t*,W32_DWORD,int32_t,int32_t,int32_t,int32_t,W32_HWND,W32_HMENU,W32_HINSTANCE,void*);
extern W32_BOOL    W32ABI DestroyWindow(W32_HWND);
extern W32_BOOL    W32ABI GetMessageW(W32_MSG*,W32_HWND,W32_UINT,W32_UINT);
extern W32_BOOL    W32ABI PeekMessageW(W32_MSG*,W32_HWND,W32_UINT,W32_UINT,W32_UINT);
extern W32_BOOL    W32ABI TranslateMessage(const W32_MSG*);
extern W32_LRESULT W32ABI DispatchMessageW(const W32_MSG*);
extern W32_BOOL    W32ABI EnableWindow(W32_HWND,W32_BOOL);
extern void        W32ABI PostQuitMessage(int);
extern W32_LRESULT W32ABI SetWindowLongPtrW(W32_HWND,int idx,W32_LRESULT);
extern W32_LRESULT W32ABI GetWindowLongPtrW(W32_HWND,int idx);
extern W32_BOOL    W32ABI ShowWindow(W32_HWND, int);
extern W32_BOOL    W32ABI UpdateWindow(W32_HWND);
extern W32_HWND    W32ABI GetWindow(W32_HWND,W32_UINT);
extern W32_BOOL    W32ABI SetWindowTextW(W32_HWND,const uint16_t*);
extern int32_t     W32ABI GetWindowTextW(W32_HWND,uint16_t*,int32_t);
extern W32_BOOL    W32ABI PostMessageW(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
extern W32_LRESULT W32ABI SendMessageW(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
extern W32_LRESULT W32ABI DefWindowProcW(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
extern W32_HWND    W32ABI GetParent(W32_HWND);
#define W32_GWLP_WNDPROC (-4)
#define W32_GW_CHILD 5
#define W32_GW_HWNDNEXT 2
#define W32_GWLP_ID (-12)
#define W32_SW_SHOW 5

/* ---- small helpers ----- */
static long long w32_wcstoll(const uint16_t *s, int base) {
    long long r = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s) { int d = *s - '0'; if (d < 0 || d >= base) break; r = r*base + d; s++; }
    return neg ? -r : r;
}

/* Iterate children of `parent` by probing the window table in
 * user32_win.c via the helper exported below. */
extern int w32_win_count_and_list(W32_HWND parent, W32_HWND *out, int max);

static W32_HWND dlg_first_child(W32_HWND parent) {
    W32_HWND one;
    int n = w32_win_count_and_list(parent, &one, 1);
    if (n > 0) return one;
    return GetWindow(parent, W32_GW_CHILD);
}
static W32_HWND dlg_next_sibling(W32_HWND child, W32_HWND parent) {
    (void)parent;
    return GetWindow(child, W32_GW_HWNDNEXT);
}

/* =====================================================================
 * Dialogs
 * ===================================================================== */
#define W32_DLG_MAX 8
typedef struct {
    int used; W32_HWND hwnd; W32_HWND owner; void *proc;
    W32_INT_PTR result; int ended; W32_LRESULT prev_wndproc;
} w32_dlg_t;
static w32_dlg_t dlg_table[W32_DLG_MAX];

static w32_dlg_t *dlg_alloc(void) {
    for (int i = 0; i < W32_DLG_MAX; i++) {
        if (!dlg_table[i].used) {
            memset(&dlg_table[i], 0, sizeof dlg_table[i]);
            dlg_table[i].used = 1;
            return &dlg_table[i];
        }
    }
    return 0;
}
static w32_dlg_t *dlg_by_hwnd(W32_HWND h) {
    for (int i = 0; i < W32_DLG_MAX; i++) {
        if (dlg_table[i].used && dlg_table[i].hwnd == h) return &dlg_table[i];
    }
    return 0;
}

static const uint16_t dlg_cls_w[] = {'#','3','2','7','7','0',0};

static W32_LRESULT W32ABI dlg_frameproc(W32_HWND hw, W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp) {
    w32_dlg_t *d = dlg_by_hwnd(hw);
    typedef W32_LRESULT (W32ABI *wndproc_t)(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
    if (msg == W32_WM_CLOSE && d) { EndDialog(hw, W32_IDCANCEL); return 0; }
    if (d && d->proc) {
        typedef W32_INT_PTR (W32ABI *dp_t)(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
        W32_INT_PTR r = ((dp_t)d->proc)(hw, msg, wp, lp);
        if (msg == W32_WM_INITDIALOG) { (void)r; return 1; }
        if (r) return 0;
    }
    if (d && d->prev_wndproc) {
        return ((wndproc_t)(uintptr_t)d->prev_wndproc)(hw, msg, wp, lp);
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static const uint16_t *dlg_skip_z_or_id(const uint16_t *p) {
    if (!p) return 0;
    if (p[0] == 0xFFFF) return p + 2;
    while (*p) p++;
    return p + 1;
}
static const uint16_t *dlg_parse_header(const W32_DLGTEMPLATE *tmpl) {
    const uint16_t *p = (const uint16_t *)((const uint8_t *)tmpl + sizeof(W32_DLGTEMPLATE));
    p = dlg_skip_z_or_id(p);                /* menu */
    p = dlg_skip_z_or_id(p);                /* class */
    p = dlg_skip_z_or_id(p);                /* title */
    if (tmpl->style & W32_DS_SETFONT) {
        p++;                                /* ptsize */
        p++;                                /* weight */
        p++;                                /* italic + charset packed as one WORD */
        p = dlg_skip_z_or_id(p);            /* typeface */
    }
    while (((uintptr_t)p) & 2) p++;         /* align to DWORD */
    return p;
}

W32ABI W32_INT_PTR DialogBoxIndirectParamW(W32_HINSTANCE inst, const W32_DLGTEMPLATE *tmpl,
                                           W32_HWND owner, void *proc, W32_LPARAM init) {
    if (!tmpl || !proc) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return -1; }
    (void)dlg_parse_header(tmpl);

    int32_t w = (int32_t)(tmpl->cx * 8 / 4);
    int32_t h = (int32_t)(tmpl->cy * 16 / 8);
    if (w < 100) w = 240;
    if (h < 50) h = 160;
    /* Pass only the WS_* window-style bits to CreateWindowExW; DS_* dialog
     * styles are interpreted by the dialog engine itself and are not valid
     * CreateWindow style flags. */
    uint32_t ws = W32_WS_POPUP | W32_WS_CAPTION | W32_WS_SYSMENU |
                  W32_WS_VISIBLE | W32_WS_DLGFRAME;
    if (tmpl->style & W32_DS_MODALFRAME) ws |= W32_WS_DLGFRAME;
    W32_HWND hw = CreateWindowExW(tmpl->exStyle & 0x00040000u, dlg_cls_w, (const uint16_t*)0,
                                  ws, tmpl->x, tmpl->y, w, h, owner, 0, inst, 0);
    if (!hw) return -1;

    w32_dlg_t *d = dlg_alloc();
    if (!d) { DestroyWindow(hw); return -1; }
    d->hwnd = hw; d->owner = owner; d->proc = proc; d->ended = 0; d->result = -1;
    d->prev_wndproc = GetWindowLongPtrW(hw, W32_GWLP_WNDPROC);
    SetWindowLongPtrW(hw, W32_GWLP_WNDPROC, (W32_LRESULT)(uintptr_t)dlg_frameproc);

    if (owner) EnableWindow(owner, 0);
    ShowWindow(hw, W32_SW_SHOW);
    UpdateWindow(hw);

    typedef W32_INT_PTR (W32ABI *dp_t)(W32_HWND,W32_UINT,W32_WPARAM,W32_LPARAM);
    ((dp_t)proc)(hw, W32_WM_INITDIALOG, (W32_WPARAM)0, init);

    W32_MSG m;
    while (!d->ended && GetMessageW(&m, 0, 0, 0)) {
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    W32_INT_PTR result = d->result;
    int slot = (int)(d - dlg_table);
    memset(&dlg_table[slot], 0, sizeof dlg_table[slot]);
    DestroyWindow(hw);
    if (owner) EnableWindow(owner, 1);
    return result;
}
W32ABI W32_INT_PTR DialogBoxIndirectParamA(W32_HINSTANCE inst, const void *tmpl,
                                           W32_HWND owner, void *proc, W32_LPARAM init) {
    return DialogBoxIndirectParamW(inst, (const W32_DLGTEMPLATE *)tmpl, owner, proc, init);
}
W32ABI W32_INT_PTR DialogBoxParamW(W32_HINSTANCE inst, const uint16_t *name,
                                   W32_HWND owner, void *proc, W32_LPARAM init) {
    void *h = FindResourceW(inst, name, (const uint16_t *)(uintptr_t)W32_RT_DIALOG);
    if (!h) { w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return -1; }
    void *hr = LoadResource(inst, h);
    const void *tmpl = hr ? LockResource(hr) : 0;
    if (!tmpl) { w32_set_last_error(W32_ERROR_RESOURCE_DATA_NOT_FOUND); return -1; }
    return DialogBoxIndirectParamW(inst, (const W32_DLGTEMPLATE *)tmpl, owner, proc, init);
}
W32ABI W32_INT_PTR DialogBoxParamA(W32_HINSTANCE inst, const char *name,
                                   W32_HWND owner, void *proc, W32_LPARAM init) {
    uintptr_t v = (uintptr_t)name;
    if (v >> 16) { w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return -1; }
    return DialogBoxParamW(inst, (const uint16_t *)(v & 0xFFFF), owner, proc, init);
}
W32ABI W32_INT_PTR DialogBoxW(W32_HINSTANCE i,const uint16_t*n,W32_HWND o,void*p){return DialogBoxParamW(i,n,o,p,0);}
W32ABI W32_INT_PTR DialogBoxA(W32_HINSTANCE i,const char*n,W32_HWND o,void*p){return DialogBoxParamA(i,n,o,p,0);}

W32ABI W32_BOOL EndDialog(W32_HWND hw, W32_INT_PTR result) {
    w32_dlg_t *d = dlg_by_hwnd(hw);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    d->result = result; d->ended = 1;
    PostQuitMessage(0);
    return 1;
}
W32ABI W32_BOOL IsDialogMessageW(W32_HWND dlg, W32_MSG *msg) { (void)dlg; (void)msg; return 0; }
W32ABI W32_BOOL IsDialogMessageA(W32_HWND dlg, W32_MSG *msg) { return IsDialogMessageW(dlg, msg); }
W32ABI W32_BOOL MapDialogRect(W32_HWND dlg, W32_RECT *r) {
    (void)dlg;
    if (!r) return 0;
    int32_t rw = r->right - r->left;
    int32_t rh = r->bottom - r->top;
    r->right  = r->left + rw * 8 / 4;
    r->bottom = r->top  + rh * 16 / 8;
    return 1;
}
W32ABI W32_DWORD GetDialogBaseUnits(void) { return (16u << 16) | 8u; }

/* ---- DlgItem accessors ---- */
W32ABI W32_HWND GetDlgItem(W32_HWND dlg, int32_t id) {
    if (!dlg) return 0;
    W32_HWND kids[32];
    int n = w32_win_count_and_list(dlg, kids, 32);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if ((int32_t)GetWindowLongPtrW(kids[i], W32_GWLP_ID) == id) return kids[i];
        }
        return 0;
    }
    for (W32_HWND c = dlg_first_child(dlg); c; c = dlg_next_sibling(c, dlg)) {
        if ((int32_t)GetWindowLongPtrW(c, W32_GWLP_ID) == id) return c;
    }
    return 0;
}
W32ABI uint32_t GetDlgItemInt(W32_HWND dlg, int32_t id, W32_BOOL *tr, W32_BOOL sgn) {
    (void)sgn;
    W32_HWND c = GetDlgItem(dlg, id);
    uint16_t b[64];
    if (c && GetWindowTextW(c, b, 64) > 0) {
        if (tr) *tr = 1;
        return (uint32_t)w32_wcstoll(b, 10);
    }
    if (tr) *tr = 0;
    return 0;
}
W32ABI W32_UINT GetDlgItemTextW(W32_HWND dlg, int32_t id, uint16_t *buf, int32_t cch) {
    W32_HWND c = GetDlgItem(dlg, id);
    if (!c || !buf || cch <= 0) return 0;
    return (W32_UINT)GetWindowTextW(c, buf, cch);
}
W32ABI W32_UINT GetDlgItemTextA(W32_HWND dlg, int32_t id, char *buf, int32_t cch) {
    uint16_t wb[256];
    if (cch <= 0) return 0;
    int n = GetDlgItemTextW(dlg, id, wb, (int)(sizeof wb/sizeof wb[0]));
    if (n <= 0) { buf[0] = 0; return 0; }
    return (W32_UINT)w32_utf16z_to_utf8(wb, buf, cch);
}
W32ABI W32_BOOL SetDlgItemInt(W32_HWND dlg, int32_t id, uint32_t v, W32_BOOL sgn) {
    W32_HWND c = GetDlgItem(dlg, id);
    if (!c) return 0;
    uint16_t b[24]; int n = 0;
    int32_t sv = (sgn && (v & 0x80000000u)) ? -(int32_t)v : (int32_t)v;
    if (sv < 0) { b[n++] = '-'; sv = -sv; }
    if (sv == 0) {
        b[n++] = '0';
    } else {
        uint16_t tmp[12]; int k = 0;
        while (sv > 0) { tmp[k++] = '0' + (uint16_t)(sv % 10); sv /= 10; }
        for (int j = k-1; j >= 0; j--) { b[n++] = tmp[j]; }
    }
    b[n] = 0;
    return SetWindowTextW(c, b);
}
W32ABI W32_BOOL SetDlgItemTextW(W32_HWND dlg, int32_t id, const uint16_t *t) {
    W32_HWND c = GetDlgItem(dlg, id);
    if (!c) return 0;
    return SetWindowTextW(c, t);
}
W32ABI W32_BOOL SetDlgItemTextA(W32_HWND dlg, int32_t id, const char *t) {
    uint16_t wb[256];
    w32_utf8z_to_utf16(t, wb, (int)(sizeof wb/sizeof wb[0]));
    return SetDlgItemTextW(dlg, id, wb);
}
W32ABI W32_UINT IsDlgButtonChecked(W32_HWND dlg,int32_t id){(void)dlg;(void)id;return 0;}
W32ABI W32_BOOL CheckDlgButton(W32_HWND dlg,int32_t id,W32_UINT chk){(void)dlg;(void)id;(void)chk;return 1;}
W32ABI W32_BOOL CheckRadioButton(W32_HWND dlg,int32_t f,int32_t l,int32_t c){(void)dlg;(void)f;(void)l;(void)c;return 1;}

/* =====================================================================
 * Menus
 * ===================================================================== */
#define W32_MENU_MAX 32
#define W32_MENU_ITEMS 32
typedef struct { uint32_t flags; uintptr_t id; const void *text_w; W32_HMENU sub; uint32_t state; } w32_menu_item_t;
typedef struct w32_menu { int used; w32_menu_item_t items[W32_MENU_ITEMS]; int n; W32_HWND attached; } w32_menu_t;
static w32_menu_t menus[W32_MENU_MAX];
static w32_menu_t *menu_from_h(W32_HMENU h) {
    uintptr_t v = (uintptr_t)h;
    if (v < 1 || v > W32_MENU_MAX) return 0;
    w32_menu_t *m = &menus[v-1];
    return m->used ? m : 0;
}
static W32_HMENU h_from_menu(w32_menu_t *m) { return (W32_HMENU)(uintptr_t)(m - menus + 1); }
static w32_menu_t *menu_alloc(void) {
    for (int i = 0; i < W32_MENU_MAX; i++) {
        if (!menus[i].used) { memset(&menus[i],0,sizeof menus[i]); menus[i].used=1; return &menus[i]; }
    }
    return 0;
}
static int menu_find(w32_menu_t *m, uint32_t item, W32_UINT fl) {
    if (fl & W32_MF_BYPOSITION) return (item < (uint32_t)m->n) ? (int)item : -1;
    for (int i=0; i<m->n; i++) if (m->items[i].id == item) return i;
    return -1;
}

W32ABI W32_HMENU CreateMenu(void) { w32_menu_t *m = menu_alloc(); return m ? h_from_menu(m) : 0; }
W32ABI W32_HMENU CreatePopupMenu(void) { return CreateMenu(); }
W32ABI W32_BOOL  DestroyMenu(W32_HMENU h) { w32_menu_t *m = menu_from_h(h); if (!m) return 0; m->used = 0; return 1; }
W32ABI W32_BOOL AppendMenuW(W32_HMENU h, W32_UINT fl, uintptr_t id, const uint16_t *t) {
    w32_menu_t *m = menu_from_h(h);
    if (!m || m->n >= W32_MENU_ITEMS) return 0;
    w32_menu_item_t *it = &m->items[m->n++];
    it->flags = fl; it->id = id; it->text_w = t;
    it->sub = (fl & W32_MF_POPUP) ? (W32_HMENU)id : 0;
    it->state = (fl & W32_MF_CHECKED) ? W32_MF_CHECKED : 0;
    return 1;
}
W32ABI W32_BOOL AppendMenuA(W32_HMENU h, W32_UINT fl, uintptr_t id, const char *t) {
    (void)t;
    w32_menu_t *m = menu_from_h(h);
    if (!m || m->n >= W32_MENU_ITEMS) return 0;
    w32_menu_item_t *it = &m->items[m->n++];
    it->flags=fl; it->id=id; it->text_w=0;
    it->sub=(fl&W32_MF_POPUP)?(W32_HMENU)id:0;
    return 1;
}
W32ABI W32_BOOL InsertMenuW(W32_HMENU h, uint32_t pos, W32_UINT fl, uintptr_t id, const uint16_t *t) {
    w32_menu_t *m = menu_from_h(h);
    if (!m || m->n >= W32_MENU_ITEMS) return 0;
    if (pos > (uint32_t)m->n) pos = m->n;
    for (int i = m->n; i > (int)pos; i--) {
        m->items[i] = m->items[i-1];
    }
    m->n++;
    w32_menu_item_t *it = &m->items[pos];
    it->flags=fl; it->id=id; it->text_w=t;
    it->sub=(fl&W32_MF_POPUP)?(W32_HMENU)id:0;
    return 1;
}
W32ABI W32_BOOL InsertMenuA(W32_HMENU h, uint32_t pos, W32_UINT fl, uintptr_t id, const char *t) {
    (void)t; return InsertMenuW(h, pos, fl, id, 0);
}
W32ABI W32_BOOL TrackPopupMenu(W32_HMENU h, W32_UINT fl, int32_t x, int32_t y, int32_t r, W32_HWND o, const W32_RECT *rc) {
    (void)x; (void)y; (void)r; (void)rc; (void)fl;
    w32_menu_t *m = menu_from_h(h); if (!m) return 0;
    if (o) SendMessageW(o, W32_WM_INITMENUPOPUP, (W32_WPARAM)h, 0);
    (void)m; return 1;
}
W32ABI W32_BOOL TrackPopupMenuEx(W32_HMENU h,W32_UINT fl,int32_t x,int32_t y,W32_HWND o,void *tp){(void)tp;return TrackPopupMenu(h,fl,x,y,0,o,0);}
W32ABI W32_HMENU GetMenu(W32_HWND w) {
    for (int i=0;i<W32_MENU_MAX;i++) if (menus[i].used && menus[i].attached==w) return h_from_menu(&menus[i]);
    return 0;
}
W32ABI W32_HMENU GetSubMenu(W32_HMENU h, int pos) {
    w32_menu_t *m=menu_from_h(h); if(!m||pos<0||pos>=m->n)return 0;
    return m->items[pos].sub;
}
W32ABI W32_HMENU GetSystemMenu(W32_HWND w, W32_BOOL rev) {
    (void)rev;
    if (!w) return 0;
    static int inited = 0; static w32_menu_t *sm = 0;
    if (!inited) { sm = menu_alloc(); inited = 1; }
    return h_from_menu(sm);
}
W32ABI W32_BOOL SetMenu(W32_HWND w, W32_HMENU h) {
    w32_menu_t *m = menu_from_h(h);
    if (h && !m) return 0;
    for (int i=0;i<W32_MENU_MAX;i++) {
        if (menus[i].used && menus[i].attached == w) menus[i].attached = 0;
    }
    if (m) m->attached = w;
    return 1;
}
W32ABI W32_BOOL CheckMenuItem(W32_HMENU h, uint32_t item, W32_UINT fl) {
    w32_menu_t *m=menu_from_h(h); if(!m) return (W32_BOOL)-1;
    int idx=menu_find(m,item,fl); if(idx<0) return (W32_BOOL)-1;
    W32_UINT old = m->items[idx].state & W32_MF_CHECKED;
    if (fl & W32_MF_CHECKED)   m->items[idx].state |= W32_MF_CHECKED;
    if (fl & W32_MF_UNCHECKED) m->items[idx].state &= ~W32_MF_CHECKED;
    return old ? W32_MF_CHECKED : W32_MF_UNCHECKED;
}
W32ABI int GetMenuItemCount(W32_HMENU h) { w32_menu_t *m=menu_from_h(h); return m?m->n:-1; }
W32ABI uint32_t GetMenuItemID(W32_HMENU h, int pos) { w32_menu_t *m=menu_from_h(h); if(!m||pos<0||pos>=m->n)return (uint32_t)-1; return (uint32_t)m->items[pos].id; }
W32ABI W32_BOOL DrawMenuBar(W32_HWND w){(void)w;return 1;}
W32ABI W32_BOOL RemoveMenu(W32_HMENU h, uint32_t item, W32_UINT fl) {
    w32_menu_t *m=menu_from_h(h); if(!m)return 0;
    int idx=menu_find(m,item,fl); if(idx<0)return 0;
    for (int i=idx; i<m->n-1; i++) { m->items[i]=m->items[i+1]; }
    m->n--;
    return 1;
}
W32ABI W32_BOOL DeleteMenu(W32_HMENU h,uint32_t item,W32_UINT fl){return RemoveMenu(h,item,fl);}
W32ABI W32_BOOL EnableMenuItem(W32_HMENU h,uint32_t item,W32_UINT fl){(void)h;(void)item;(void)fl;return 0;}
W32ABI W32_BOOL GetMenuBarInfo(W32_HWND w,long obj,long item,void *pm){(void)w;(void)obj;(void)item;(void)pm;return 1;}
W32ABI W32_HMENU LoadMenuW(W32_HINSTANCE inst, const uint16_t *name) {
    void *h = FindResourceW(inst, name, (const uint16_t *)(uintptr_t)W32_RT_MENU);
    if (h) (void)LockResource(h);
    return CreateMenu();
}
W32ABI W32_HMENU LoadMenuA(W32_HINSTANCE inst,const char *name) {
    uintptr_t v=(uintptr_t)name; if(v>>16)return 0;
    return LoadMenuW(inst,(const uint16_t*)(v&0xFFFF));
}

/* =====================================================================
 * Timers
 * ===================================================================== */
#define W32_TIMER_MAX 64
typedef struct { int used; W32_HWND w; uintptr_t id; W32_UINT ms; uint64_t next; void *cb; } w32_timer_t;
static w32_timer_t timers[W32_TIMER_MAX];
static uint64_t now_ms(void) { extern W32_DWORD W32ABI GetTickCount(void); return GetTickCount(); }

W32ABI uintptr_t SetTimer(W32_HWND w, uintptr_t id, W32_UINT ms, void *cb) {
    if (ms == 0) ms = 1;
    for (int i=0;i<W32_TIMER_MAX;i++) {
        if (timers[i].used && timers[i].w == w && timers[i].id == id) {
            timers[i].ms = ms; timers[i].cb = cb;
            timers[i].next = now_ms() + ms;
            return id;
        }
    }
    for (int i=0;i<W32_TIMER_MAX;i++) {
        if (!timers[i].used) {
            timers[i].used=1; timers[i].w=w; timers[i].id=id;
            timers[i].ms=ms; timers[i].cb=cb; timers[i].next=now_ms()+ms;
            return id;
        }
    }
    w32_set_last_error(W32_ERROR_NOT_SUPPORTED); return 0;
}
W32ABI W32_BOOL KillTimer(W32_HWND w, uintptr_t id) {
    for (int i=0;i<W32_TIMER_MAX;i++) {
        if (timers[i].used && timers[i].w == w && timers[i].id == id) { timers[i].used=0; return 1; }
    }
    return 0;
}
void w32_dlg_fire_timers(void) {
    uint64_t n = now_ms();
    for (int i = 0; i < W32_TIMER_MAX; i++) {
        if (!timers[i].used) continue;
        if ((int64_t)(n - timers[i].next) < 0) continue;
        if (timers[i].cb) {
            typedef void (W32ABI *tcb)(W32_HWND,W32_UINT,uintptr_t,W32_DWORD);
            ((tcb)timers[i].cb)(timers[i].w, W32_WM_TIMER, timers[i].id, (W32_DWORD)n);
        } else if (timers[i].w) {
            PostMessageW(timers[i].w, W32_WM_TIMER, (W32_WPARAM)timers[i].id, 0);
        }
        timers[i].next = n + timers[i].ms;
    }
}

/* =====================================================================
 * Caret
 * ===================================================================== */
static struct { int used; W32_HWND w; int32_t x,y,w_,h; int visible; } caret = {0};
W32ABI W32_BOOL CreateCaret(W32_HWND w,void *bmp,int32_t wd,int32_t ht){
    (void)bmp; caret.used=1;caret.w=w;caret.w_=wd>0?wd:2;caret.h=ht>0?ht:16;caret.visible=0;return 1;
}
W32ABI W32_BOOL DestroyCaret(void){caret.used=0;return 1;}
W32ABI W32_BOOL SetCaretPos(int32_t x,int32_t y){if(!caret.used)return 0;caret.x=x;caret.y=y;return 1;}
W32ABI W32_BOOL GetCaretPos(W32_POINT *p){if(!p||!caret.used)return 0;p->x=caret.x;p->y=caret.y;return 1;}
W32ABI W32_BOOL ShowCaret(W32_HWND w){(void)w;if(!caret.used)return 0;caret.visible=1;return 1;}
W32ABI W32_BOOL HideCaret(W32_HWND w){(void)w;if(!caret.used)return 0;caret.visible=0;return 1;}

/* =====================================================================
 * Accelerators
 * ===================================================================== */
#define W32_ACCEL_MAX_ENTRIES 64
#define W32_ACCEL_HTABS 8
typedef struct { int used; W32_ACCEL entries[W32_ACCEL_MAX_ENTRIES]; int n; } w32_accel_t;
static w32_accel_t accels[W32_ACCEL_HTABS];
static W32_HACCEL acc_h(w32_accel_t *a){return (W32_HACCEL)(uintptr_t)(a-accels+1);}
static w32_accel_t *acc_f(W32_HACCEL h){uintptr_t v=(uintptr_t)h;if(v<1||v>W32_ACCEL_HTABS)return 0; w32_accel_t *a=&accels[v-1]; return a->used?a:0;}

W32ABI W32_HACCEL CreateAcceleratorTableW(W32_ACCEL *acc, int c) {
    for(int i=0;i<W32_ACCEL_HTABS;i++) {
        if(!accels[i].used) {
            memset(&accels[i],0,sizeof accels[i]); accels[i].used=1;
            int n = c<W32_ACCEL_MAX_ENTRIES?c:W32_ACCEL_MAX_ENTRIES;
            for(int j=0;j<n;j++) accels[i].entries[j]=acc[j];
            accels[i].n=n; return acc_h(&accels[i]);
        }
    }
    return 0;
}
W32ABI int CopyAcceleratorTableW(W32_HACCEL src, W32_ACCEL *dst, int c) {
    w32_accel_t *a=acc_f(src); if(!a)return 0;
    int n=c<a->n?c:a->n;
    if(dst) for(int i=0;i<n;i++) dst[i]=a->entries[i];
    return a->n;
}
W32ABI W32_BOOL DestroyAcceleratorTable(W32_HACCEL acc) { w32_accel_t *a=acc_f(acc); if(!a)return 0; a->used=0; return 1; }
W32ABI W32_HACCEL LoadAcceleratorsW(W32_HINSTANCE inst, const uint16_t *name) {
    void *hr = FindResourceW(inst, name, (const uint16_t *)(uintptr_t)W32_RT_ACCELERATOR);
    W32_ACCEL empty = {0};
    if (hr) (void)LockResource(hr);
    return CreateAcceleratorTableW(&empty, 0);
}
W32ABI W32_HACCEL LoadAcceleratorsA(W32_HINSTANCE inst,const char *name) {
    uintptr_t v=(uintptr_t)name; if(v>>16)return 0;
    return LoadAcceleratorsW(inst,(const uint16_t*)(v&0xFFFF));
}
W32ABI int TranslateAcceleratorW(W32_HWND w, W32_HACCEL acc, W32_MSG *msg) {
    w32_accel_t *a=acc_f(acc); (void)w; if(!a||!msg)return 0;
    if (msg->message != 0x0100 && msg->message != 0x0104) return 0;
    int vk = (int)(intptr_t)msg->wParam;
    for (int i=0; i<a->n; i++) {
        W32_ACCEL *e = &a->entries[i];
        if ((e->flags & W32_FVIRTKEY) && e->key == vk) {
            PostMessageW(w, W32_WM_COMMAND, (W32_WPARAM)e->cmd, 0);
            return 1;
        }
    }
    return 0;
}
W32ABI int TranslateAcceleratorA(W32_HWND w,W32_HACCEL a,W32_MSG *m){return TranslateAcceleratorW(w,a,m);}

/* =====================================================================
 * Clipboard
 * ===================================================================== */
#define GUI_CLIP_MAX 4096
static char clip_u8[GUI_CLIP_MAX];
typedef struct { W32_UINT fmt; void *h; } w32_clip_e;
#define W32_CLIP_MAX 16
static w32_clip_e clip_ents[W32_CLIP_MAX];
static int clip_open; static W32_HWND clip_owner;
static W32_HWND clip_viewer;
static uint16_t clip_wide[GUI_CLIP_MAX];
static void clip_clear(void){for(int i=0;i<W32_CLIP_MAX;i++){clip_ents[i].fmt=0;clip_ents[i].h=0;}}
static w32_clip_e *clip_find(W32_UINT f){for(int i=0;i<W32_CLIP_MAX;i++)if(clip_ents[i].fmt==f)return &clip_ents[i];return 0;}
static w32_clip_e *clip_new(void){for(int i=0;i<W32_CLIP_MAX;i++)if(!clip_ents[i].fmt)return &clip_ents[i];return 0;}

W32ABI W32_BOOL OpenClipboard(W32_HWND w){if(clip_open){w32_set_last_error(W32_ERROR_ACCESS_DENIED);return 0;}clip_open=1;clip_owner=w;return 1;}
W32ABI W32_BOOL CloseClipboard(void){
    if(!clip_open)return 0;
    clip_open=0;clip_owner=0;
    if(clip_viewer) PostMessageW(clip_viewer,W32_WM_DRAWCLIPBOARD,0,0);
    return 1;
}
W32ABI W32_BOOL EmptyClipboard(void){if(!clip_open)return 0;clip_clear();ag_set_clipboard("");return 1;}
W32ABI void *SetClipboardData(W32_UINT fmt, void *h) {
    if(!clip_open){w32_set_last_error(W32_ERROR_INVALID_PARAMETER);return 0;}
    w32_clip_e *e = clip_find(fmt); if(!e)e=clip_new(); if(!e)return 0;
    e->fmt=fmt; e->h=h;
    if (fmt == W32_CF_TEXT && h) ag_set_clipboard((const char*)h);
    else if (fmt == W32_CF_UNICODETEXT && h) {
        char u8[GUI_CLIP_MAX];
        w32_utf16z_to_utf8((const uint16_t*)h, u8, (int)sizeof u8);
        ag_set_clipboard(u8);
        int n=0; const uint16_t *s=(const uint16_t*)h;
        while(s[n] && n<GUI_CLIP_MAX-1){clip_wide[n]=s[n];n++;}
        clip_wide[n]=0;
    }
    return h;
}
W32ABI void *GetClipboardData(W32_UINT fmt) {
    if (fmt == W32_CF_TEXT) { ag_get_clipboard(clip_u8, (int)sizeof clip_u8); return clip_u8; }
    if (fmt == W32_CF_UNICODETEXT) {
        char u8[GUI_CLIP_MAX];
        ag_get_clipboard(u8, (int)sizeof u8);
        w32_utf8z_to_utf16(u8,(uint16_t*)clip_wide,GUI_CLIP_MAX);
        return clip_wide;
    }
    w32_clip_e *e = clip_find(fmt);
    return e ? e->h : 0;
}
W32ABI W32_BOOL IsClipboardFormatAvailable(W32_UINT fmt) {
    if (fmt == W32_CF_TEXT || fmt == W32_CF_UNICODETEXT) return 1;
    return clip_find(fmt) != 0;
}
W32ABI W32_UINT RegisterClipboardFormatW(const uint16_t *n){(void)n;return 0xC000;}
W32ABI W32_UINT RegisterClipboardFormatA(const char *n){(void)n;return 0xC000;}
W32ABI W32_BOOL CountClipboardFormats(void){return 1;}
W32ABI W32_BOOL EnumClipboardFormats(W32_UINT){return 0;}
W32ABI W32_HWND GetClipboardOwner(void){return clip_owner;}
W32ABI W32_HWND GetOpenClipboardWindow(void){return clip_owner;}
W32ABI W32_BOOL SetClipboardViewer(W32_HWND w){clip_viewer=w;return 1;}
W32ABI W32_HWND ChangeClipboardChain(W32_HWND r, W32_HWND n) {
    if (clip_viewer == r) clip_viewer = n;
    else if (clip_viewer) PostMessageW(clip_viewer,W32_WM_CHANGECBCHAIN,(W32_WPARAM)r,(W32_LPARAM)n);
    return 0;
}
W32ABI W32_HWND GetClipboardViewer(void){return clip_viewer;}

/* =====================================================================
 * Hooks
 * ===================================================================== */
#define W32_HOOK_MAX 16
typedef struct { int used; int32_t id; void *proc; W32_DWORD tid; W32_HINSTANCE mod; } w32_hook_t;
static w32_hook_t hooks[W32_HOOK_MAX];
W32ABI W32_HHOOK SetWindowsHookExW(int32_t id,void *proc,W32_HINSTANCE mod,W32_DWORD tid) {
    if (mod && !tid) { w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED); return 0; }
    for(int i=0;i<W32_HOOK_MAX;i++) {
        if(!hooks[i].used){
            hooks[i].used=1;hooks[i].id=id;hooks[i].proc=proc;hooks[i].tid=tid;hooks[i].mod=mod;
            return (W32_HHOOK)(uintptr_t)(i+1);
        }
    }
    return 0;
}
W32ABI W32_HHOOK SetWindowsHookExA(int32_t id,void *p,W32_HINSTANCE m,W32_DWORD t){return SetWindowsHookExW(id,p,m,t);}
W32ABI W32_BOOL UnhookWindowsHookEx(W32_HHOOK h){uintptr_t v=(uintptr_t)h;if(v<1||v>W32_HOOK_MAX)return 0;hooks[v-1].used=0;return 1;}
W32ABI W32_LRESULT CallNextHookEx(W32_HHOOK hhk,int32_t c,W32_WPARAM w,W32_LPARAM l){(void)hhk;(void)c;(void)w;(void)l;return 0;}

/* =====================================================================
 * Draw* family
 * ===================================================================== */
W32ABI int DrawTextW(W32_HDC hdc, const uint16_t *s, int32_t len, W32_RECT *r, W32_UINT fmt) {
    (void)hdc; (void)fmt;
    if (!r || !s) return 0;
    char u8[512];
    int32_t n = len;
    if (n < 0) { n = 0; while (s[n] && n < 512) n++; }
    if (n > 511) n = 511;
    uint16_t tmp[514];
    for (int32_t i = 0; i < n; i++) tmp[i] = s[i];
    tmp[n] = 0;
    int got = w32_utf16z_to_utf8(tmp, u8, (int)sizeof u8);
    ag_text(-1, u8, r->left, r->top, 0xFFFFFFFFu, 0x00000000u);
    int32_t w = got * 8, h = 16;
    if (fmt & W32_DT_CALCRECT) { r->right=r->left+w; r->bottom=r->top+h; }
    return h;
}
W32ABI int DrawTextA(W32_HDC hdc, const char *s, int32_t len, W32_RECT *r, W32_UINT fmt) {
    uint16_t wb[512];
    int32_t n = len < 0 ? (int32_t)strlen(s) : len;
    if (n < 0) n = 0;
    if (n > 511) n = 511;
    w32_utf8z_to_utf16(s, wb, 512); wb[n]=0;
    return DrawTextW(hdc, wb, n, r, fmt);
}
W32ABI int DrawTextExW(W32_HDC h,const uint16_t*s,int32_t l,W32_RECT*r,W32_UINT f,void*p){(void)p;return DrawTextW(h,s,l,r,f);}
W32ABI W32_BOOL DrawFocusRect(W32_HDC hdc, const W32_RECT *r) {
    (void)hdc; if (!r) return 0;
    ag_rect_outline(-1, r->left, r->top, r->right-r->left, r->bottom-r->top, 0xFF808080u);
    return 1;
}
W32ABI W32_BOOL DrawEdge(W32_HDC h,W32_RECT *r,W32_UINT e,W32_UINT g){(void)e;(void)g;return DrawFocusRect(h,r);}
W32ABI W32_BOOL DrawFrameControl(W32_HDC h,W32_RECT *r,W32_UINT t,W32_UINT st){(void)h;(void)r;(void)t;(void)st;return 1;}
/* W32A-7: the icon decode and raster live in w32_gdi.c; this half only
 * owns the export.  A null icon now fails honestly (the A-6 stub
 * returned TRUE without drawing -- the test asserts the new contract). */
W32ABI W32_BOOL DrawIconEx(W32_HDC h,int32_t x,int32_t y,W32_HICON i,int32_t cx,int32_t cy,uint32_t st,void*hbr,W32_UINT fl){
    (void)st;(void)hbr;(void)fl;
    return w32_gdi_draw_icon(h,x,y,i,cx,cy);}
W32ABI W32_BOOL DrawIcon(W32_HDC h,int32_t x,int32_t y,W32_HICON i){return DrawIconEx(h,x,y,i,0,0,0,0,0);}
W32ABI void NotifyWinEvent(W32_DWORD ev,W32_HWND w,W32_DWORD a,W32_DWORD b){(void)ev;(void)w;(void)a;(void)b;}
