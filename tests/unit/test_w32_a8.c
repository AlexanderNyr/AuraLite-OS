/* test_w32_a8.c — W32APP_PLAN.md phase W32A-8 host gate.
 *
 * The common-controls engine (w32/src/comctl32.c) amalgamated with the
 * window core (user32_win.c), the drawing half (w32_gdi.c) and the
 * resource walk (w32_rsrc.c), against the a5/a6/a7 fake compositor.
 *
 * What this gate proves that the guest fixture cannot:
 *   * the widget seam: every control drives the hooks exactly as the
 *     guest drives libauragui (row replay after every mutation, the
 *     tree's row order through an INDEPENDENT reference walk, dispatch
 *     translated to the widget's own hit semantics)
 *   * the notification ORDER (LVN_ITEMCHANGED before NM_CLICK;
 *     TVN_ITEMEXPANDINGW before TVN_ITEMEXPANDEDW) via a recording
 *     parent proc with sequence numbers
 *   * the subclass chain (newest first, DefSubclassProc down, remove
 *     restores, re-add reuses the trampoline)
 *   * the v5/v6 palette seam (the W32A-11 hand-off point): version 5
 *     answers the classic syscolors, version 6 answers the live theme
 *   * PropertySheetW's full PSN_* flow including the APPLY-now (lParam
 *     FALSE) direction and the OK (lParam TRUE) direction
 */
#define _GNU_SOURCE 1
#define AURALITE_W32_HOST_TEST 1

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "w32/w32_abi.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/kernel32.h"
#include "w32/user32.h"
#include "w32/gdi32.h"
#include "w32/comctl32.h"

/* ---- the suite's own checks ------------------------------------------ */
static int checks, fails;
static void ok(int cond, const char *what, ...) {
    checks++;
    if (!cond) {
        fails++;
        va_list ap;
        va_start(ap, what);
        vprintf(what, ap);
        va_end(ap);
        printf("\n");
    }
}

/* ---------------------------------------------------------------
 * Host primitives (tid, ticks, TEB, events, sleep) -- the a6/a7 set.
 * --------------------------------------------------------------- */
static __thread W32_DWORD host_tid;
W32ABI W32_DWORD GetCurrentThreadId(void) {
    if (!host_tid) host_tid = (W32_DWORD)(uintptr_t)pthread_self();
    return host_tid;
}
W32ABI W32_DWORD GetCurrentProcessId(void) { return 4242; }
static W32_DWORD tick_ms;
W32ABI W32_DWORD GetTickCount(void) { return tick_ms; }
W32ABI W32_ULONGLONG GetTickCount64(void) { return tick_ms; }
static __thread struct w32_teb host_teb;
struct w32_teb *w32_teb_self(void) { return &host_teb; }
W32ABI W32_DWORD GetLastError(void) { return w32_get_last_error_raw(); }
W32ABI W32_HANDLE CreateEventW(void*,W32_BOOL,W32_BOOL,const uint16_t*) { return (W32_HANDLE)(uintptr_t)1; }
W32ABI W32_BOOL SetEvent(W32_HANDLE) { return 1; }
W32ABI W32_DWORD WaitForSingleObject(W32_HANDLE,W32_DWORD) { return 0; }
W32ABI W32_DWORD WaitForMultipleObjects(W32_DWORD,W32_HANDLE*,W32_BOOL,W32_DWORD) { return 0; }
W32ABI W32_BOOL CloseHandle(W32_HANDLE) { return 1; }
W32ABI void Sleep(W32_DWORD ms) {
    struct timespec ts = { (time_t)ms / 1000, (long)((ms % 1000) * 1000000) };
    nanosleep(&ts, 0);
}

/* ===================================================================== *
 * The fake compositor (the a7 gate's shim, unchanged in contract).
 * ===================================================================== */
#define FAKE_WINS 64
typedef struct {
    int in_use;
    int visible;
    uint32_t w, h;
    uint32_t px[128 * 128];
    struct { uint32_t type; int32_t x, y; uint32_t key;
             uint8_t buttons, mods; uint16_t data; } evq[32];
    int evq_head, evq_tail;
} fake_win_t;
static fake_win_t fw[FAKE_WINS];

static int blit_alpha_calls;
static int32_t blit_last[4];

int ag_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                     const char *t, uint32_t fl) {
    (void)x; (void)y; (void)t; (void)fl;
    for (int i = 0; i < FAKE_WINS; i++)
        if (!fw[i].in_use) {
            memset(&fw[i], 0, sizeof fw[i]);
            fw[i].in_use = 1;
            if (w > 128) w = 128;
            if (h > 128) h = 128;
            fw[i].w = w; fw[i].h = h;
            return i + 1;
        }
    return -1;
}
static int wid_ok(int wid) { return wid >= 1 && wid <= FAKE_WINS && fw[wid-1].in_use; }
int ag_window_show(int wid){ if(!wid_ok(wid))return -1; fw[wid-1].visible=1; return 0;}
int ag_window_hide(int wid){ if(!wid_ok(wid))return -1; fw[wid-1].visible=0; return 0;}
int ag_window_destroy(int wid){if(!wid_ok(wid))return -1;fw[wid-1].in_use=0;return 0;}
int ag_window_focus(int wid){return wid_ok(wid)?0:-1;}
int ag_window_minimize(int wid){return wid_ok(wid)?0:-1;}
int ag_window_maximize(int wid){return wid_ok(wid)?0:-1;}
int ag_window_restore(int wid){return wid_ok(wid)?0:-1;}
int ag_window_move(int wid,int32_t x,int32_t y){(void)x;(void)y;return wid_ok(wid)?0:-1;}
int ag_window_resize(int wid,uint32_t w,uint32_t h){
    if(!wid_ok(wid))return -1;
    if (w > 128) w = 128;
    if (h > 128) h = 128;
    fw[wid-1].w=w; fw[wid-1].h=h; return 0;
}
int ag_window_set_title(int wid,const char*t){(void)t;return wid_ok(wid)?0:-1;}
int ag_window_invalidate(int wid){return wid_ok(wid)?0:-1;}
int ag_window_invalidate_rect(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h){
    (void)x;(void)y;(void)w;(void)h;return wid_ok(wid)?0:-1;}
int ag_window_get_size(int wid,uint32_t*w,uint32_t*h){
    if(!wid_ok(wid))return -1;
    *w=fw[wid-1].w;*h=fw[wid-1].h;return 0;
}
int ag_window_get_pos(int wid,int32_t*x,int32_t*y){(void)x;(void)y;return wid_ok(wid)?0:-1;}
int ag_window_lower(int wid){return wid_ok(wid)?0:-1;}
int ag_window_set_flags(int wid,uint32_t f){(void)f;return wid_ok(wid)?0:-1;}
uint32_t ag_window_get_flags(int wid){(void)wid;return 0x01|0x02|0x04|0x08|0x10;}
int ag_window_get_z(int wid){return wid_ok(wid)?1:-1;}
int ag_window_capture(int wid){return wid_ok(wid)?wid:-1;}
int ag_window_get_capture(void){return -1;}
int ag_window_focused(void){return -1;}
int ag_window_top(void){return -1;}
int ag_screen_size(uint32_t*w,uint32_t*h){*w=640;*h=480;return 0;}
int ag_mouse_position(int32_t*x,int32_t*y){*x=10;*y=10;return 0;}
int ag_clear(int wid,uint32_t c){
    if(!wid_ok(wid))return -1;
    for(uint32_t i=0;i<fw[wid-1].w*fw[wid-1].h;i++)fw[wid-1].px[i]=c;
    return 0;
}
int ag_fill_rect(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t c){
    if(!wid_ok(wid))return -1;
    fake_win_t*W=&fw[wid-1];
    for(uint32_t yy=0;yy<h;yy++)for(uint32_t xx=0;xx<w;xx++){
        int32_t px=x+(int32_t)xx,py=y+(int32_t)yy;
        if(px>=0&&py>=0&&px<(int32_t)W->w&&py<(int32_t)W->h)W->px[py*W->w+px]=c;
    }
    return 0;
}
int ag_draw_text(int wid,int32_t x,int32_t y,const char*s,uint32_t c){
    (void)x;(void)y;(void)s;(void)c;return wid_ok(wid)?0:-1;
}
int ag_draw_pixel(int wid,int32_t x,int32_t y,uint32_t c){
    if(!wid_ok(wid))return -1;
    fake_win_t*W=&fw[wid-1];
    if(x>=0&&y>=0&&x<(int32_t)W->w&&y<(int32_t)W->h)W->px[y*W->w+x]=c;
    return 0;
}
int ag_draw_line(int wid,int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint32_t c){
    (void)x0;(void)y0;(void)x1;(void)y1;(void)c;return wid_ok(wid)?0:-1;
}
int ag_blit(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h,
            const uint32_t*src,uint32_t stride){
    if(!wid_ok(wid)||!src)return -1;
    fake_win_t*W=&fw[wid-1];
    for(uint32_t yy=0;yy<h;yy++)for(uint32_t xx=0;xx<w;xx++){
        int32_t px=x+(int32_t)xx,py=y+(int32_t)yy;
        if(px>=0&&py>=0&&px<(int32_t)W->w&&py<(int32_t)W->h)
            W->px[py*W->w+px]=src[yy*stride+xx];
    }
    return 0;
}
int ag_blit_alpha(int wid,int32_t x,int32_t y,uint32_t w,uint32_t h,
                  const uint32_t*src,uint32_t stride){
    if(!wid_ok(wid)||!src)return -1;
    fake_win_t*W=&fw[wid-1];
    blit_alpha_calls++;
    blit_last[0]=x;blit_last[1]=y;blit_last[2]=(int32_t)w;blit_last[3]=(int32_t)h;
    for(uint32_t yy=0;yy<h;yy++)for(uint32_t xx=0;xx<w;xx++){
        int32_t px=x+(int32_t)xx,py=y+(int32_t)yy;
        if(px<0||py<0||px>=(int32_t)W->w||py>=(int32_t)W->h)continue;
        uint32_t s=src[yy*stride+xx]&0x00FFFFFFu;
        uint32_t d=W->px[py*W->w+px]&0x00FFFFFFu;
        uint32_t a=(src[yy*stride+xx]>>24)&0xFFu;
        uint32_t out=0;
        for(int ch=0;ch<3;ch++){
            uint32_t sc=(s>>(ch*8))&0xFFu,dc=(d>>(ch*8))&0xFFu;
            out|=((sc*a+dc*(255-a))/255)<<(ch*8);
        }
        W->px[py*W->w+px]=out|0xFF000000u;
    }
    return 0;
}
int ag_get_pixel(int wid,int32_t x,int32_t y){
    if(!wid_ok(wid))return -1;
    fake_win_t*W=&fw[wid-1];
    if(x<0||y<0||x>=(int32_t)W->w||y>=(int32_t)W->h)return -1;
    return (int)(W->px[y*W->w+x]&0x00FFFFFFu);
}
int ag_text(int wid,const char*s,int x,int y,uint32_t fg,uint32_t bg){
    (void)s;(void)x;(void)y;(void)fg;(void)bg;return wid_ok(wid)?0:-1;
}
int ag_rect_outline(int wid,int x,int y,int w,int h,uint32_t c){
    (void)x;(void)y;(void)w;(void)h;(void)c;return wid_ok(wid)?0:-1;
}
void ag_render_now(void){}
void ag_alert(const char*a,const char*b){(void)a;(void)b;}
int ag_set_clipboard(const char*t){(void)t;return 0;}
int ag_get_clipboard(char*b,int sz){if(!b||sz<=0)return -1;b[0]=0;return 0;}
uint32_t w32_gdi_host_dpi(void){return 96;}

/* comctl32.c's host path calls GetModuleHandleW(0); nothing in this
 * amalgam defines it, so the host answers a stable fake. */
W32ABI void *GetModuleHandleW(W32_LPCWSTR name) { (void)name; return (void *)1; }

/* ===================================================================== *
 * The widget-seam hooks: a fake widget table that mirrors libauragui's
 * semantics INDEPENDENTLY (the row walk below shares no code with
 * auragui.c -- a drift between the two walks is exactly what this gate
 * exists to catch).
 * ===================================================================== */
#define HW_MAX 16
#define HW_ROWS 64
typedef struct {
    int used;
    int kind;                       /* 0 tab, 1 progress, 2 tree, 3 listbox */
    uint32_t w, h;
    /* tab + listbox rows */
    char rows[8][32];
    int row_count;
    /* tree */
    char tlabel[HW_ROWS][32];
    int parent[HW_ROWS];
    int expanded[HW_ROWS];
    int tcount;
    /* selection / active / progress */
    int active, sel, value, vmax;
    /* observation */
    int render_calls, last_render_wid;
    int dispatch_calls;
} hw_t;
static hw_t hws[HW_MAX];

static hw_t *hw_of(int wdg) {
    if (wdg < 0 || wdg >= HW_MAX || !hws[wdg].used) return 0;
    return &hws[wdg];
}

/* the reference walk: row order = DFS in insertion order, children
 * visible only while the parent is expanded (auragui's tree_walk,
 * re-derived here from the documented contract). */
static void hw_walk(hw_t *W, int parent, int depth, int *order, int *n) {
    (void)depth;
    for (int i = 0; i < W->tcount; i++) {
        if (W->parent[i] != parent) continue;
        order[(*n)++] = i;
        int kids = 0;
        for (int j = 0; j < W->tcount; j++) if (W->parent[j] == i) kids++;
        if (kids && W->expanded[i]) hw_walk(W, i, depth + 1, order, n);
    }
}
static int hw_tree_rows(hw_t *W, int *order) {
    int n = 0;
    hw_walk(W, -1, 0, order, &n);
    return n;
}
static int hw_has_children(hw_t *W, int node) {
    for (int j = 0; j < W->tcount; j++) if (W->parent[j] == node) return 1;
    return 0;
}
static int hw_depth(hw_t *W, int node) {
    int d = 0;
    for (int p = W->parent[node]; p >= 0; p = W->parent[p]) d++;
    return d;
}

int w32_comctl_host_wdg_create(int kind, int32_t x, int32_t y,
                               uint32_t w, uint32_t h) {
    (void)x; (void)y;
    for (int i = 0; i < HW_MAX; i++) {
        if (!hws[i].used) {
            memset(&hws[i], 0, sizeof hws[i]);
            hws[i].used = 1;
            hws[i].kind = kind;
            hws[i].w = w ? w : 1;
            hws[i].h = h ? h : 1;
            hws[i].sel = -1;
            hws[i].active = -1;
            return i;
        }
    }
    return -1;
}
void w32_comctl_host_wdg_destroy(int wdg) {
    hw_t *W = hw_of(wdg);
    if (W) memset(W, 0, sizeof *W);
}
void w32_comctl_host_wdg_clear(int wdg) {
    hw_t *W = hw_of(wdg);
    if (!W) return;
    W->row_count = 0;
    W->tcount = 0;
    W->sel = -1;
}
int w32_comctl_host_wdg_add_row(int wdg, int parent, const char *label) {
    hw_t *W = hw_of(wdg);
    if (!W || !label) return -1;
    if (W->kind == 2) {                      /* tree */
        if (W->tcount >= HW_ROWS) return -1;
        if (parent >= W->tcount) return -1;
        int idx = W->tcount++;
        snprintf(W->tlabel[idx], sizeof W->tlabel[0], "%s", label);
        W->parent[idx] = parent;
        W->expanded[idx] = 0;
        return idx;
    }
    if (W->kind == 0) {                      /* tab */
        if (W->row_count >= 8) return -1;
        snprintf(W->rows[W->row_count], sizeof W->rows[0], "%s", label);
        return W->row_count++;
    }
    if (W->kind == 3) {                      /* listbox */
        if (W->row_count >= 8) return -1;
        snprintf(W->rows[W->row_count], sizeof W->rows[0], "%s", label);
        return W->row_count++;
    }
    return -1;
}
void w32_comctl_host_wdg_remove_row(int wdg, int idx) {
    hw_t *W = hw_of(wdg);
    if (!W || W->kind != 0) return;
    for (int i = idx; i + 1 < W->row_count; i++)
        memcpy(W->rows[i], W->rows[i+1], sizeof W->rows[0]);
    if (idx < W->row_count) W->row_count--;
}
void w32_comctl_host_wdg_set_active(int wdg, int idx) {
    hw_t *W = hw_of(wdg);
    if (W) W->active = idx;
}
int w32_comctl_host_wdg_active(int wdg) {
    hw_t *W = hw_of(wdg);
    return W ? W->active : -1;
}
void w32_comctl_host_wdg_set_sel(int wdg, int idx) {
    hw_t *W = hw_of(wdg);
    if (W) W->sel = idx;
}
int w32_comctl_host_wdg_sel(int wdg) {
    hw_t *W = hw_of(wdg);
    return W ? W->sel : -1;
}
void w32_comctl_host_wdg_set_progress(int wdg, int value, int max) {
    hw_t *W = hw_of(wdg);
    if (!W) return;
    if (max < 0) max = 0;
    if (value < 0) value = 0;
    if (value > max) value = max;
    W->value = value;
    W->vmax = max;
}
int w32_comctl_host_wdg_progress(int wdg) {
    hw_t *W = hw_of(wdg);
    return W ? W->value : 0;
}
void w32_comctl_host_wdg_set_expanded(int wdg, int node, int on) {
    hw_t *W = hw_of(wdg);
    if (W && node >= 0 && node < W->tcount) W->expanded[node] = on ? 1 : 0;
}
int w32_comctl_host_wdg_expanded(int wdg, int node) {
    hw_t *W = hw_of(wdg);
    return (W && node >= 0 && node < W->tcount) ? W->expanded[node] : 0;
}
int w32_comctl_host_wdg_visible_rows(int wdg) {
    hw_t *W = hw_of(wdg);
    if (!W || W->kind != 2) return 0;
    int order[HW_ROWS];
    return hw_tree_rows(W, order);
}
int w32_comctl_host_wdg_row_node(int wdg, int row) {
    hw_t *W = hw_of(wdg);
    if (!W || W->kind != 2) return -1;
    int order[HW_ROWS];
    int n = hw_tree_rows(W, order);
    return (row >= 0 && row < n) ? order[row] : -1;
}
int w32_comctl_host_wdg_node_row(int wdg, int node) {
    hw_t *W = hw_of(wdg);
    if (!W || W->kind != 2) return -1;
    int order[HW_ROWS];
    int n = hw_tree_rows(W, order);
    for (int r = 0; r < n; r++) if (order[r] == node) return r;
    return -1;
}
int w32_comctl_host_wdg_row_count(int wdg) {
    hw_t *W = hw_of(wdg);
    return W ? W->row_count : 0;
}
void w32_comctl_host_wdg_render(int wdg, int wid) {
    hw_t *W = hw_of(wdg);
    if (!W) return;
    W->render_calls++;
    W->last_render_wid = wid;
}
void w32_comctl_host_wdg_dispatch(int wdg, int type, int32_t x, int32_t y) {
    hw_t *W = hw_of(wdg);
    if (!W) return;
    W->dispatch_calls++;
    if (W->kind == 3) {                      /* listbox: row pitch 14 */
        int row = y / 14;
        if (row >= 0 && row < W->row_count) W->sel = row;
        return;
    }
    if (W->kind == 0) {                      /* tab: equal strip cells */
        if (W->row_count <= 0) return;
        int tab_w = (int)W->w / W->row_count;
        if (tab_w <= 0) tab_w = 1;
        int idx = x / tab_w;
        if (idx < 0) idx = 0;
        if (idx >= W->row_count) idx = W->row_count - 1;
        W->active = idx;
        return;
    }
    if (W->kind == 2) {                      /* tree */
        int order[HW_ROWS];
        int n = hw_tree_rows(W, order);
        int row = y / 14;
        if (row < 0 || row >= n) return;
        int node = order[row];
        int d = hw_depth(W, node);
        int gutter_lo = 2 + 12 * d;
        int gutter_hi = gutter_lo + 12;
        if (x >= gutter_lo && x < gutter_hi) {
            if (hw_has_children(W, node))
                W->expanded[node] = !W->expanded[node];
            return;
        }
        if (type == 4 /* DBLCLICK */) {
            if (hw_has_children(W, node))
                W->expanded[node] = !W->expanded[node];
            return;
        }
        W->sel = node;
        return;
    }
}

/* the palette seam: v6 reads the live theme through this hook */
static int host_v6 = 1;
static uint32_t host_accent = 0x00AA3311u;
void w32_comctl_host_palette(w32_comctl_palette_t *out) {
    if (!out) return;
    out->face  = host_v6 ? 0x00F0F0F0u : 0x00C0C0C0u;
    out->text  = 0x00000000u;
    out->frame = host_v6 ? 0x00999999u : 0x00808080u;
    out->hot   = host_accent;
    out->sel   = host_accent;
}

/* ===================================================================== *
 * The personality, amalgamated.
 * ===================================================================== */
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/user32_win.c"
#include "../../w32/src/user32.c"
#include "../../w32/src/w32_gdi.c"       /* the A-7 drawing half */
#include "../../w32/src/w32_dlg.c"       /* timers + GetDlgItem (the a6 set) */
#include "../../w32/src/w32_rsrc.c"      /* LoadIconW (the 381 path) */
#include "../../w32/src/comctl32.c"      /* W32A-8: the engine under test */

/* mirror-struct calls: after the includes, like the a6/a7 gates */
int ag_theme_get(ui_theme_t *out){if(out)memset(out,0,sizeof(ui_theme_t));return 0;}
int ag_poll_event(int wid,ui_event_t *out){
    if(wid<1||wid>FAKE_WINS||!out)return -1;
    fake_win_t*w=&fw[wid-1];
    if(w->evq_head==w->evq_tail)return 0;
    memcpy(out,&w->evq[w->evq_head],sizeof w->evq[0]);
    w->evq_head=(w->evq_head+1)%32;
    return 1;
}

W32ABI void SetLastError(W32_DWORD c){ w32_set_last_error(c); }

/* w32_rsrc.c's module lookup: no module table on the host, nothing has
 * a name. */
W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name) { (void)name; return 0; }

/* w32_rsrc.c walks module bytes through this; no module here has any. */
int w32_module_file_bytes(void *h, const uint8_t **d, size_t *sz) {
    (void)h; (void)d; (void)sz; return 0;
}

/* ===================================================================== *
 * The recording parent (notifications + commands, with order)
 * ===================================================================== */
#define REC_NM 32
static uint32_t rec_codes[REC_NM];
static int rec_n;
static uint64_t rec_cmd_w;
static W32_HWND rec_cmd_from;
static int rec_cmd_seen;

static W32_LRESULT W32ABI a8_parent_proc(W32_HWND h, W32_UINT m,
                                         W32_WPARAM w, W32_LPARAM l) {
    if (m == W32_WM_NOTIFY) {
        W32_NMHDR *nm = (W32_NMHDR *)(uintptr_t)l;
        if (nm && rec_n < REC_NM) rec_codes[rec_n++] = nm->code;
        return 0;
    }
    if (m == W32_WM_COMMAND) {
        rec_cmd_w = w;
        rec_cmd_from = (W32_HWND)(uintptr_t)l;
        rec_cmd_seen++;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static int rec_index(uint32_t code) {
    for (int i = 0; i < rec_n; i++) if (rec_codes[i] == code) return i;
    return -1;
}
static void rec_reset(void) { rec_n = 0; rec_cmd_seen = 0; }

static void drain(void) {
    W32_MSG m;
    while (PeekMessageW(&m, (W32_HWND)0, 0, 0, 1))
        DispatchMessageW(&m);
}

/* find the live hook widget of a kind (the observation channel) */
static hw_t *hw_find(int kind) {
    hw_t *last = 0;
    for (int i = 0; i < HW_MAX; i++)
        if (hws[i].used && hws[i].kind == kind) last = &hws[i];
    return last;                    /* newest control of the kind */
}

static W32_LPARAM mk_lp(int x, int y) {
    return (W32_LPARAM)(uint32_t)(((uint32_t)y << 16) | (uint32_t)x);
}

/* ===================================================================== *
 * Property-sheet page procs (host, C): page 1 switches, page 2 drives
 * Apply-now then OK, so BOTH PSN_APPLY lParam directions are recorded.
 * ===================================================================== */
static int p1_init, p1_active, p1_kill, p1_apply0, p1_apply1;
static int p2_init, p2_active, p2_apply0, p2_apply1;
static int p_btn_found;

static W32_INT_PTR W32ABI a8_page1(W32_HWND h, W32_UINT m,
                                   W32_WPARAM w, W32_LPARAM l) {
    if (m == W32_WM_INITDIALOG) {
        p1_init++;
        p_btn_found = GetDlgItem(h, 1001) != (W32_HWND)0;
        PostMessageW(GetParent(h), W32_PSM_SETCURSEL, (W32_WPARAM)1, 0);
        return 1;
    }
    if (m == W32_WM_NOTIFY) {
        W32_PSHNOTIFY *n = (W32_PSHNOTIFY *)(uintptr_t)l;
        if (n->hdr.code == W32_PSN_SETACTIVE) { p1_active++; return 0; }
        if (n->hdr.code == W32_PSN_KILLACTIVE) { p1_kill++; return 0; }
        if (n->hdr.code == W32_PSN_APPLY) {
            if (n->lParam) p1_apply1++; else p1_apply0++;
            return W32_PSNRET_NOERROR;
        }
    }
    (void)w;
    return 0;
}
static W32_INT_PTR W32ABI a8_page2(W32_HWND h, W32_UINT m,
                                   W32_WPARAM w, W32_LPARAM l) {
    if (m == W32_WM_INITDIALOG) {
        p2_init++;
        PostMessageW(GetParent(h), W32_PSM_PRESSBUTTON,
                     (W32_WPARAM)W32_PSBTN_APPLYNOW, 0);
        return 1;
    }
    if (m == W32_WM_NOTIFY) {
        W32_PSHNOTIFY *n = (W32_PSHNOTIFY *)(uintptr_t)l;
        if (n->hdr.code == W32_PSN_SETACTIVE) { p2_active++; return 0; }
        if (n->hdr.code == W32_PSN_APPLY) {
            if (n->lParam) { p2_apply1++; return W32_PSNRET_NOERROR; }
            p2_apply0++;
            /* Apply done: end the sheet the documented way */
            PostMessageW(GetParent(h), W32_PSM_PRESSBUTTON,
                         (W32_WPARAM)W32_PSBTN_OK, 0);
            return W32_PSNRET_NOERROR;
        }
    }
    (void)w;
    return 0;
}

/* one-button in-memory templates (the A-6 item grammar) */
static uint16_t tmpl1[] = {
    0x0000, 0x0000,             /* style (lo/hi)  */
    0x0000, 0x0000,             /* exStyle        */
    0x0001, 0x0000,             /* cdit = 1       */
    4, 4, 80, 40,               /* x y cx cy      */
    0,                          /* menu: none     */
    0,                          /* class: none    */
    'P','a','g','e',' ','1',0,  /* title          */
    0x0000,                      /* pad to DWORD (item align) */
    0x0000, 0x5001,             /* item style WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON */
    0x0000, 0x0000,             /* item exStyle   */
    8, 8, 60, 14,               /* x y cx cy      */
    1001,                       /* id             */
    0xFFFF, 0x0080,             /* ordinal class: Button */
    'B','1',0,                  /* title          */
    0,                          /* creation data size */
};
static uint16_t tmpl2[] = {
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0001, 0x0000, 4, 4, 80, 40,
    0, 0, 'P','a','g','e',' ','2',0, 0x0000,
    0x0000, 0x5001, 0x0000, 0x0000, 8, 8, 60, 14, 1002,
    0xFFFF, 0x0080, 'B','2',0, 0,
};

/* ===================================================================== *
 * Subclass recorders
 * ===================================================================== */
static int sub_ran[8], sub_n;
static W32_LRESULT W32ABI hsub1(W32_HWND h, W32_UINT m, W32_WPARAM w,
                                W32_LPARAM l, uint64_t id, int64_t d) {
    (void)h; (void)m; (void)w; (void)l; (void)id; (void)d;
    if (sub_n < 8) sub_ran[sub_n++] = 1;
    return DefSubclassProc(h, m, w, l);
}
static W32_LRESULT W32ABI hsub2(W32_HWND h, W32_UINT m, W32_WPARAM w,
                                W32_LPARAM l, uint64_t id, int64_t d) {
    (void)h; (void)m; (void)w; (void)l; (void)id; (void)d;
    if (sub_n < 8) sub_ran[sub_n++] = 2;
    return DefSubclassProc(h, m, w, l);
}

/* ===================================================================== *
 * main
 * ===================================================================== */
int main(void) {
    printf("w32a8 host gate\n");

    /* ---- INIT + the version seam ---------------------------------------- */
    {
        W32_INITCOMMONCONTROLSEX icc = { 0, 0 };
        ok(InitCommonControlsEx(&icc) == 0, "icc: zero dwSize refused");
        icc.dwSize = (uint32_t)sizeof icc;
        icc.dwICC = W32_ICC_BAR_CLASSES | W32_ICC_LISTVIEW_CLASSES |
                    W32_ICC_TREEVIEW_CLASSES | W32_ICC_TAB_CLASSES |
                    W32_ICC_PROGRESS_CLASS;
        ok(InitCommonControlsEx(&icc) == 1, "icc: the class mask admits");
        InitCommonControls();               /* ordinal 17: no return value */

        w32_comctl_set_version(6);
        const w32_comctl_palette_t *p6 = w32_comctl_palette();
        ok(p6->face == 0x00F0F0F0u && p6->hot == host_accent,
           "v6 palette reads the live theme (face %06X hot %06X)",
           p6->face, p6->hot);
        w32_comctl_set_version(5);
        const w32_comctl_palette_t *p5 = w32_comctl_palette();
        ok(p5->face == 0x00C0C0C0u && p5->frame == 0x00808080u &&
           p5->sel == 0x00000080u, "v5 palette is the classic syscolors");
        ok(p5->hot != p6->hot && p5->face != p6->face,
           "the two versions render differently (the W32A-11 seam)");
        w32_comctl_set_version(6);
    }

    /* ---- the parent window ------------------------------------------------ */
    W32_HWND parent = 0;
    {
        static uint16_t cls[] = {'A','8','P','a','r','e','n','t',0};
        W32_WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = (uint32_t)sizeof wc;
        wc.lpfnWndProc = a8_parent_proc;
        wc.lpszClassName = cls;
        ok(RegisterClassExW(&wc) != 0, "parent class registers");
        parent = CreateWindowExW(0, cls, 0, W32_WS_OVERLAPPEDWINDOW,
                                 10, 10, 400, 300, 0, 0, 0, 0);
        ok(parent != 0, "parent window creates");
    }

    /* ---- WSCHILD: the gate ------------------------------------------------ */
    {
        static uint16_t acls[] = {'A','8','A','p','p',0};
        W32_WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = (uint32_t)sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = acls;
        ok(RegisterClassExW(&wc) != 0, "app class registers");
        SetLastError(0);
        W32_HWND bad = CreateWindowExW(0, acls, 0, W32_WS_CHILD,
                                       0, 0, 50, 50, parent, 0, 0, 0);
        ok(bad == 0, "WS_CHILD on an application class is refused");
        ok(GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "refusal names itself (err %u)", GetLastError());
        W32_HWND lv = CreateWindowExW(0, (const uint16_t *)W32_WC_LISTVIEWW,
                                      0, W32_WS_CHILD | W32_WS_VISIBLE,
                                      0, 0, 200, 160, parent, 0, 0, 0);
        ok(lv != 0, "WS_CHILD on a comctl class is admitted");
        ok(GetParent(lv) == parent, "GetParent lands the link");
    }
    W32_HWND lv = CreateWindowExW(0, (const uint16_t *)W32_WC_LISTVIEWW,
                                  0, W32_WS_CHILD | W32_WS_VISIBLE,
                                  0, 0, 200, 160, parent, 0, 0, 0);
    ok(lv != 0, "listview creates (second instance)");

    /* ---- TOOLBAR ----------------------------------------------------------- */
    W32_HWND tb = 0;
    {
        W32_TBBUTTON btns[3];
        memset(btns, 0, sizeof btns);
        for (int i = 0; i < 3; i++) {
            btns[i].iBitmap = i;
            btns[i].idCommand = 2001 + i;
            btns[i].fsState = W32_TBSTATE_ENABLED;
            btns[i].fsStyle = W32_TBSTYLE_BUTTON;
            btns[i].iString = -1;
        }
        tb = CreateToolbarEx(parent, W32_WS_CHILD | W32_WS_VISIBLE, 1001,
                             0, 0, 0, btns, 3, 40, 24, 16, 16,
                             (uint32_t)sizeof(W32_TBBUTTON));
        ok(tb != 0, "CreateToolbarEx makes the control");
        ok(SendMessageW(tb, W32_TB_BUTTONCOUNT, 0, 0) == 3, "three buttons");
        ok(SendMessageW(tb, W32_TB_COMMANDTOINDEX, 2002, 0) == 1,
           "command-to-index maps");
        ok(SendMessageW(tb, W32_TB_GETBUTTONSIZE, 0, 0) == (24u << 16 | 40u),
           "the create cells are the reported size");
        ok(SendMessageW(tb, W32_TB_ISBUTTONENABLED, 2002, 0) != 0,
           "buttons start enabled");
        ok(SendMessageW(tb, W32_TB_ENABLEBUTTON, 2002, 0) != 0,
           "disable accepted");
        ok(SendMessageW(tb, W32_TB_ISBUTTONENABLED, 2002, 0) == 0,
           "disabled reads back");
        ok(SendMessageW(tb, W32_TB_ENABLEBUTTON, 2002, 1) != 0,
           "re-enable accepted");
        rec_reset();
        SendMessageW(tb, W32_WM_LBUTTONDOWN, 1, mk_lp(80, 5));
        drain();
        ok(rec_cmd_seen == 1, "a click posts one WM_COMMAND");
        ok((uint32_t)rec_cmd_w == 2002, "the command id is the button's");
        ok(rec_cmd_from == tb, "lParam is the control window");
    }

    /* ---- STATUS ------------------------------------------------------------ */
    {
        static uint16_t s0[] = {'l','e','f','t',0};
        static uint16_t s1[] = {'r','i','g','h','t',0};
        W32_HWND sb = CreateStatusWindowW(W32_WS_CHILD | W32_WS_VISIBLE,
                                          0, parent, 1002);
        ok(sb != 0, "CreateStatusWindowW makes the control");
        int32_t parts[2] = { 200, 400 };
        ok(SendMessageW(sb, W32_SB_SETPARTS, 2, (W32_LPARAM)(intptr_t)parts) == 1,
           "parts set");
        ok(SendMessageW(sb, W32_SB_SETTEXTW, 0, (W32_LPARAM)(intptr_t)s0) == 1,
           "part 0 text set");
        ok(SendMessageW(sb, W32_SB_SETTEXTW, 1, (W32_LPARAM)(intptr_t)s1) == 1,
           "part 1 text set");
        ok(SendMessageW(sb, W32_SB_GETTEXTLENGTHW, 0, 0) == 4, "len(left)==4");
        uint16_t buf[64] = {0};
        ok(SendMessageW(sb, W32_SB_GETTEXTW, 1, (W32_LPARAM)(intptr_t)buf) == 5,
           "len(right)==5");
        ok(buf[0] == 'r' && buf[4] == 't', "the text round-trips");
        int32_t back[4] = {0};
        ok(SendMessageW(sb, W32_SB_GETPARTS, 4, (W32_LPARAM)(intptr_t)back) == 2,
           "two parts read back");
    }

    /* ---- LISTVIEW ----------------------------------------------------------- */
    {
        static uint16_t t0[] = {'a','l','p','h','a',0};
        static uint16_t t1[] = {'b','e','t','a',0};
        static uint16_t t2[] = {'g','a','m','m','a',0};
        static uint16_t col[] = {'c','o','l',0};
        W32_LVITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = W32_LVIF_TEXT | W32_LVIF_PARAM;
        it.pszText = t0; it.lParam = 0x111;
        ok(SendMessageW(lv, W32_LVM_INSERTITEMW, 0,
                        (W32_LPARAM)(intptr_t)&it) == 0, "insert @0");
        it.pszText = t1; it.lParam = 0x222; it.iItem = 1;
        ok(SendMessageW(lv, W32_LVM_INSERTITEMW, 0,
                        (W32_LPARAM)(intptr_t)&it) == 1, "insert @1");
        it.pszText = t2; it.lParam = 0x333; it.iItem = 2;
        ok(SendMessageW(lv, W32_LVM_INSERTITEMW, 0,
                        (W32_LPARAM)(intptr_t)&it) == 2, "insert @2");
        ok(SendMessageW(lv, W32_LVM_GETITEMCOUNT, 0, 0) == 3, "count 3");

        /* the seam: the widget's rows replay in item order */
        hw_t *W = hw_find(3);
        ok(W && W->row_count == 3, "the widget mirrors three rows");
        ok(W && W->rows[1][0] == 'b', "row 1 label replayed");

        uint16_t out[64] = {0};
        memset(&it, 0, sizeof it);
        it.mask = W32_LVIF_TEXT | W32_LVIF_PARAM;
        it.iItem = 1; it.pszText = out; it.cchTextMax = 64;
        ok(SendMessageW(lv, W32_LVM_GETITEMW, 0, (W32_LPARAM)(intptr_t)&it) == 1,
           "getitem ok");
        ok(out[0] == 'b' && out[1] == 'e', "text reads back");
        ok(it.lParam == 0x222, "lparam reads back");

        memset(&it, 0, sizeof it);
        it.mask = W32_LVIF_STATE;
        it.state = W32_LVIS_SELECTED; it.stateMask = W32_LVIS_SELECTED;
        it.iItem = 1;
        SendMessageW(lv, W32_LVM_SETITEMSTATE, 1, (W32_LPARAM)(intptr_t)&it);
        ok(SendMessageW(lv, W32_LVM_GETSELECTEDCOUNT, 0, 0) == 1, "one selected");
        ok(SendMessageW(lv, W32_LVM_GETNEXTITEM, (W32_WPARAM)-1,
                        W32_LVNI_SELECTED) == 1, "the selected one is item 1");
        ok(hw_find(3)->sel == 1, "the widget selection tracks state");

        W32_LVCOLUMNW c;
        memset(&c, 0, sizeof c);
        c.mask = W32_LVCF_FMT | W32_LVCF_WIDTH | W32_LVCF_TEXT;
        c.cx = 100; c.pszText = col;
        ok(SendMessageW(lv, W32_LVM_INSERTCOLUMNA, 0,
                        (W32_LPARAM)(intptr_t)&c) == 0, "column inserted");
        memset(&c, 0, sizeof c);
        c.mask = W32_LVCF_TEXT; c.pszText = out; c.cchTextMax = 64;
        ok(SendMessageW(lv, W32_LVM_GETCOLUMNA, 0,
                        (W32_LPARAM)(intptr_t)&c) == 1 && out[0] == 'c',
           "column text round-trips");

        /* click order: LVN_ITEMCHANGED before NM_CLICK */
        rec_reset();
        SendMessageW(lv, W32_WM_LBUTTONDOWN, 1, mk_lp(10, 18));
        drain();
        int i_chg = rec_index(W32_LVN_ITEMCHANGED);
        int i_clk = rec_index(W32_NM_CLICK);
        ok(i_chg >= 0 && i_clk >= 0, "click notified both (%d,%d)", i_chg, i_clk);
        ok(i_chg < i_clk, "ITEMCHANGED ran BEFORE NM_CLICK");
        ok(SendMessageW(lv, W32_LVM_GETNEXTITEM, (W32_WPARAM)-1,
                        W32_LVNI_SELECTED) == 1, "the click selected row 1");

        /* HitTest */
        W32_LVHITTESTINFO ht;
        memset(&ht, 0, sizeof ht);
        ht.pt.x = 10; ht.pt.y = 28;
        ok(SendMessageW(lv, W32_LVM_HITTEST, 0, (W32_LPARAM)(intptr_t)&ht) == 2,
           "hittest reads row 2");

        ok(SendMessageW(lv, W32_LVM_DELETEITEM, 0, 0) == 1, "delete ok");
        ok(SendMessageW(lv, W32_LVM_GETITEMCOUNT, 0, 0) == 2, "count drops");
        ok(hw_find(3)->row_count == 2, "the widget replayed the delete");
    }

    /* ---- TREEVIEW ----------------------------------------------------------- */
    W32_HWND tv = CreateWindowExW(0, (const uint16_t *)W32_WC_TREEVIEWW,
                                  0, W32_WS_CHILD | W32_WS_VISIBLE,
                                  210, 0, 200, 160, parent, 0, 0, 0);
    ok(tv != 0, "treeview creates");
    W32_HWND h_root = 0, h_c1 = 0, h_c2 = 0;
    {
        static uint16_t r[] = {'R',0};
        static uint16_t c1[] = {'C','1',0};
        static uint16_t c2[] = {'C','2',0};
        W32_TVINSERTSTRUCTW ins;
        memset(&ins, 0, sizeof ins);
        ins.hParent = W32_TVI_ROOT;
        ins.hInsertAfter = W32_TVI_LAST;
        ins.item.mask = W32_TVIF_TEXT | W32_TVIF_CHILDREN;
        ins.item.pszText = r;
        ins.item.cChildren = 1;
        ins.item.lParam = 0x100;
        h_root = (W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_INSERTITEMW, 0,
                                                   (W32_LPARAM)(intptr_t)&ins);
        ok(h_root != 0, "root inserted");
        ins.hParent = h_root;
        ins.item.pszText = c1; ins.item.cChildren = 0; ins.item.lParam = 0x101;
        h_c1 = (W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_INSERTITEMW, 0,
                                                 (W32_LPARAM)(intptr_t)&ins);
        ins.item.pszText = c2; ins.item.lParam = 0x102;
        h_c2 = (W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_INSERTITEMW, 0,
                                                 (W32_LPARAM)(intptr_t)&ins);
        ok(h_c1 != 0 && h_c2 != 0 && h_c1 != h_c2 && h_root != h_c1,
           "handles are distinct and nonzero");
        ok(SendMessageW(tv, W32_TVM_GETCOUNT, 0, 0) == 3, "count 3");

        /* the seam: the widget mirrors the tree shape */
        hw_t *W = hw_find(2);
        ok(W && W->tcount == 3, "three widget nodes");
        ok(W && W->parent[1] == 0 && W->parent[2] == 0,
           "children parented at node 0");

        /* walks */
        ok((W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_GETNEXTITEM,
              W32_TVGN_ROOT, 0) == h_root, "TVGN_ROOT");
        ok((W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_GETNEXTITEM,
              W32_TVGN_CHILD, (W32_LPARAM)(intptr_t)h_root) == h_c1,
           "TVGN_CHILD is the first child");
        ok((W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_GETNEXTITEM,
              W32_TVGN_NEXT, (W32_LPARAM)(intptr_t)h_c1) == h_c2,
           "TVGN_NEXT walks siblings");
        ok((W32_HWND)(uintptr_t)SendMessageW(tv, W32_TVM_GETNEXTITEM,
              W32_TVGN_PARENT, (W32_LPARAM)(intptr_t)h_c2) == h_root,
           "TVGN_PARENT climbs");

        /* select -> TVN_SELCHANGEDW */
        rec_reset();
        ok(SendMessageW(tv, W32_TVM_SELECTITEM, W32_TVGN_CARET,
                        (W32_LPARAM)(intptr_t)h_c1) == 1, "select ok");
        ok(rec_index(W32_TVN_SELCHANGEDW) >= 0, "selection notified");
        ok(hw_find(2)->sel == 1, "the widget selection tracks the caret");

        /* expand: EXPANDING before EXPANDED, and the walk changes */
        rec_reset();
        ok(SendMessageW(tv, W32_TVM_EXPAND, W32_TVE_EXPAND,
                        (W32_LPARAM)(intptr_t)h_root) == 1, "expand ok");
        int i_ping = rec_index(W32_TVN_ITEMEXPANDINGW);
        int i_ped  = rec_index(W32_TVN_ITEMEXPANDEDW);
        ok(i_ping >= 0 && i_ped >= 0, "expand notified both (%d,%d)",
           i_ping, i_ped);
        ok(i_ping < i_ped, "ITEMEXPANDING ran BEFORE ITEMEXPANDED");
        ok(hw_find(2)->expanded[0] == 1, "the widget node expanded");

        /* GETITEM text */
        W32_TVITEMW gi;
        uint16_t out[64] = {0};
        memset(&gi, 0, sizeof gi);
        gi.mask = W32_TVIF_TEXT;
        gi.hItem = h_c1; gi.pszText = out; gi.cchTextMax = 64;
        ok(SendMessageW(tv, W32_TVM_GETITEMW, 0, (W32_LPARAM)(intptr_t)&gi) == 1,
           "getitem ok");
        ok(out[0] == 'C' && out[1] == '1', "the child's text reads back");

        /* GETITEMRECT: the handle rides the RECT's first 8 bytes */
        W32_RECT rct;
        memset(&rct, 0, sizeof rct);
        memcpy(&rct, &h_c1, sizeof h_c1);
        ok(SendMessageW(tv, W32_TVM_GETITEMRECT, 1,
                        (W32_LPARAM)(intptr_t)&rct) == 1, "itemrect ok");
        ok(rct.top == 14 && rct.bottom == 28, "child is row 1 (%d..%d)",
           rct.top, rct.bottom);

        /* the gutter click collapses through the widget dispatch */
        rec_reset();
        SendMessageW(tv, W32_WM_LBUTTONDOWN, 1, mk_lp(5, 7));
        drain();
        ok(rec_index(W32_TVN_ITEMEXPANDINGW) >= 0 &&
           rec_index(W32_TVN_ITEMEXPANDEDW) >= 0,
           "the gutter toggle notified expand pair");
        ok(hw_find(2)->expanded[0] == 0, "the widget collapsed");
        ok(SendMessageW(tv, W32_TVM_GETVISIBLECOUNT, 0, 0) == 1,
           "visible rows honour the collapse");

        /* expand again, then delete the root: everything goes */
        SendMessageW(tv, W32_TVM_EXPAND, W32_TVE_EXPAND,
                     (W32_LPARAM)(intptr_t)h_root);
        ok(SendMessageW(tv, W32_TVM_DELETEITEM, 0,
                        (W32_LPARAM)(intptr_t)h_root) == 1, "delete root");
        ok(SendMessageW(tv, W32_TVM_GETCOUNT, 0, 0) == 0, "subtree deleted");
        ok(hw_find(2)->tcount == 0, "the widget replayed the subtree delete");
    }

    /* ---- TAB ------------------------------------------------------------------ */
    W32_HWND tc = CreateWindowExW(0, (const uint16_t *)W32_WC_TABCONTROLW,
                                  0, W32_WS_CHILD | W32_WS_VISIBLE,
                                  0, 200, 200, 26, parent, 0, 0, 0);
    ok(tc != 0, "tab creates");
    {
        static uint16_t t1[] = {'O','n','e',0};
        static uint16_t t2[] = {'T','w','o',0};
        W32_TCITEMW ti;
        memset(&ti, 0, sizeof ti);
        ti.mask = W32_TCIF_TEXT | W32_TCIF_PARAM;
        ti.pszText = t1; ti.lParam = 0xA1;
        ok(SendMessageW(tc, W32_TCM_INSERTITEMW, 0,
                        (W32_LPARAM)(intptr_t)&ti) == 0, "tab 0 inserted");
        ti.pszText = t2; ti.lParam = 0xA2;
        ok(SendMessageW(tc, W32_TCM_INSERTITEMW, 1,
                        (W32_LPARAM)(intptr_t)&ti) == 1, "tab 1 inserted");
        ok(SendMessageW(tc, W32_TCM_GETITEMCOUNT, 0, 0) == 2, "two tabs");
        rec_reset();
        ok(SendMessageW(tc, W32_TCM_SETCURSEL, 1, 0) == 0,
           "setcursel returns the previous (0)");
        ok(rec_n == 0, "setcursel does NOT notify");
        ok(SendMessageW(tc, W32_TCM_GETCURSEL, 0, 0) == 1, "selection moved");
        rec_reset();
        SendMessageW(tc, W32_WM_LBUTTONDOWN, 1, mk_lp(50, 5));
        drain();
        ok(rec_index(W32_TCN_SELCHANGE) >= 0, "the strip click notified");
        ok(SendMessageW(tc, W32_TCM_GETCURSEL, 0, 0) == 0, "back to tab 0");
        ok(hw_find(0)->active == 0, "the widget active tab tracks");
    }

    /* ---- PROGRESS --------------------------------------------------------------- */
    W32_HWND pb = CreateWindowExW(0, (const uint16_t *)W32_WC_PROGRESSW,
                                  0, W32_WS_CHILD | W32_WS_VISIBLE,
                                  0, 230, 200, 16, parent, 0, 0, 0);
    ok(pb != 0, "progress creates");
    {
        SendMessageW(pb, W32_PBM_SETRANGE, 0, (W32_LPARAM)(uint32_t)(100u << 16));
        ok(SendMessageW(pb, W32_PBM_SETPOS, 50, 0) == 0, "setpos returns prev");
        ok(SendMessageW(pb, W32_PBM_GETPOS, 0, 0) == 50, "pos reads back");
        SendMessageW(pb, W32_PBM_SETSTEP, 10, 0);
        ok(SendMessageW(pb, W32_PBM_STEPIT, 0, 0) == 50, "stepit returns prev");
        ok(SendMessageW(pb, W32_PBM_GETPOS, 0, 0) == 60, "stepped to 60");
        int32_t rng[2] = { -1, -1 };
        ok(SendMessageW(pb, W32_PBM_GETRANGE, 0, (W32_LPARAM)(intptr_t)rng)
           == 100, "getrange returns the high limit");
        ok(rng[0] == 0 && rng[1] == 100, "the range reads back");
        ok(hw_find(1)->value == 60 && hw_find(1)->vmax == 100,
           "the widget progress tracks");
    }

    /* ---- TOOLTIP ------------------------------------------------------------------- */
    W32_HWND tt = CreateWindowExW(0, (const uint16_t *)W32_WC_TOOLTIPW,
                                  0, 0, -32000, -32000, 120, 22, 0, 0, 0, 0);
    ok(tt != 0, "tooltip creates");
    {
        static uint16_t tip[] = {'t','i','p',' ','t','e','x','t',0};
        W32_TOOLINFOW ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = (uint32_t)sizeof ti;
        ti.hwnd = parent;
        ti.uId = 7;
        ti.rect.left = 0; ti.rect.top = 0;
        ti.rect.right = 50; ti.rect.bottom = 20;
        ti.lpszText = tip;
        ok(SendMessageW(tt, W32_TTM_ADDTOOLW, 0,
                        (W32_LPARAM)(intptr_t)&ti) == 1, "tool added");
        ok(SendMessageW(tt, W32_TTM_GETTOOLCOUNT, 0, 0) == 1, "one tool");
        SendMessageW(tt, W32_TTM_ACTIVATE, 1, 0);
        W32_MSG m;
        memset(&m, 0, sizeof m);
        m.hwnd = parent; m.message = W32_WM_MOUSEMOVE;
        m.lParam = mk_lp(10, 10);
        SendMessageW(tt, W32_TTM_RELAYEVENT, 0, (W32_LPARAM)(intptr_t)&m);
        ok(IsWindowVisible(tt) == 1, "the relayed move showed the tip");
        memset(&ti, 0, sizeof ti);
        ti.cbSize = (uint32_t)sizeof ti;
        ti.hwnd = parent; ti.uId = 7;
        uint16_t out[64] = {0};
        ti.lpszText = out;
        ok(SendMessageW(tt, W32_TTM_GETCURRENTTOOLW, 0,
                        (W32_LPARAM)(intptr_t)&ti) == 1, "current tool reads");
        ok(out[0] == 't', "the tip text is the tool's");
        m.lParam = mk_lp(100, 100);
        SendMessageW(tt, W32_TTM_RELAYEVENT, 0, (W32_LPARAM)(intptr_t)&m);
        ok(IsWindowVisible(tt) == 0, "the outside relay hid it");
    }

    /* ---- HEADER --------------------------------------------------------------------- */
    W32_HWND hd = CreateWindowExW(0, (const uint16_t *)W32_WC_HEADERW,
                                  0, W32_WS_CHILD | W32_WS_VISIBLE,
                                  0, 250, 200, 18, parent, 0, 0, 0);
    ok(hd != 0, "header creates");
    {
        static uint16_t hcol[] = {'c','o','l',0};
        W32_HDITEMW hi;
        memset(&hi, 0, sizeof hi);
        hi.mask = W32_HDI_WIDTH | W32_HDI_TEXT;
        hi.cxy = 100; hi.pszText = hcol;
        ok(SendMessageW(hd, W32_HDM_INSERTITEMW, 0,
                        (W32_LPARAM)(intptr_t)&hi) == 0, "item inserted");
        ok(SendMessageW(hd, W32_HDM_GETITEMCOUNT, 0, 0) == 1, "one item");
        rec_reset();
        SendMessageW(hd, W32_WM_LBUTTONDOWN, 1, mk_lp(10, 5));
        drain();
        ok(rec_index(W32_HDN_ITEMCLICKW) >= 0, "the column click notified");
    }

    /* ---- IMAGELIST -------------------------------------------------------------------- */
    {
        /* 16x16 32bpp: magenta with a green cross row */
        static uint32_t px[16 * 16];
        for (int i = 0; i < 16 * 16; i++) px[i] = 0xFFFF00FFu;
        for (int i = 0; i < 16; i++) px[7 * 16 + i] = 0xFF00FF00u;
        W32_HBITMAP bmp = CreateBitmap(16, 16, 1, 32, px);
        ok(bmp != 0, "the source bitmap mints");
        W32_HIMAGELIST himl = ImageList_Create(16, 16, W32_ILC_COLOR32, 0, 4);
        ok(himl != 0, "the list mints");
        ok(ImageList_AddMasked(himl, bmp, 0x00FF00FFu) == 0,
           "addmasked appends at 0");
        ok(ImageList_GetImageCount(himl) == 1, "one image");
        int32_t cx = 0, cy = 0;
        ok(ImageList_GetIconSize(himl, &cx, &cy) && cx == 16 && cy == 16,
           "icon size reads");
        W32_HICON hic = ImageList_GetIcon(himl, 0, W32_ILD_NORMAL);
        ok(hic != 0, "geticon mints an HICON");
        ok(ImageList_ReplaceIcon(himl, -1, hic) == 1, "replaceicon appends");
        ok(ImageList_GetImageCount(himl) == 2, "two images");
        W32_IMAGEINFO ii;
        ok(ImageList_GetImageInfo(himl, 0, &ii) &&
           ii.rcImage.right == 16 && ii.rcImage.bottom == 16,
           "imageinfo cell rect");
        ok(ii.hbmImage != 0, "imageinfo mints the strip bitmap");

        /* draw onto the parent's window DC and read the pixels back */
        W32_HDC hdc = GetDC(parent);
        ok(hdc != 0, "window dc");
        ok(ImageList_Draw(himl, 0, hdc, 10, 10, W32_ILD_NORMAL),
           "draw onto the window dc");
        ok(GetPixel(hdc, 12, 17) == 0x0000FF00u,
           "the green row drew (got %06X)", GetPixel(hdc, 12, 17));
        ok(GetPixel(hdc, 12, 11) != 0x00FF00FFu,
           "the mask colour did not draw");
        ReleaseDC(parent, hdc);

        /* remove + destroy */
        ok(ImageList_Remove(himl, 1) && ImageList_GetImageCount(himl) == 1,
           "remove drops the appended image");
        ok(ImageList_BeginDrag(himl, 0, 0, 0), "begindrag");
        ok(ImageList_DragEnter(parent, 40, 60), "dragenter");
        ok(ImageList_DragMove(80, 90), "dragmove");
        ok(ImageList_DragShowNolock(0), "draghidenolock");
        ImageList_EndDrag();
        ok(ImageList_Destroy(himl), "destroy");
        DeleteObject(bmp);
        /* a dead list handle is refused, not crashed */
        ok(ImageList_GetImageCount(himl) == 0,
           "a dead list answers zero, not garbage");
    }

    /* ---- SUBCLASS ------------------------------------------------------------------------ */
    {
        ok(SetWindowSubclass(lv, hsub1, 41, 0x1111), "subclass 41 installs");
        ok(SetWindowSubclass(lv, hsub2, 42, 0x2222), "subclass 42 installs");
        sub_n = 0;
        memset(sub_ran, 0, sizeof sub_ran);
        ok(SendMessageW(lv, 0x000E /*WM_GETTEXTLENGTH*/, 0, 0) == 0,
           "the class proc still answers through the chain");
        ok(sub_n == 2 && sub_ran[0] == 2 && sub_ran[1] == 1,
           "newest ran first, then chained down (%d,%d)",
           sub_ran[0], sub_ran[1]);
        int64_t data = 0;
        ok(GetWindowSubclass(lv, hsub1, 41, &data) && data == 0x1111,
           "getsubclass returns the install data");
        ok(RemoveWindowSubclass(lv, 42), "remove 42");
        sub_n = 0; memset(sub_ran, 0, sizeof sub_ran);
        SendMessageW(lv, 0x000E, 0, 0);
        ok(sub_n == 1 && sub_ran[0] == 1, "only 41 runs now");
        ok(RemoveWindowSubclass(lv, 41), "remove 41");
        sub_n = 0; memset(sub_ran, 0, sizeof sub_ran);
        ok(SendMessageW(lv, 0x000E, 0, 0) == 0, "the class still answers");
        ok(sub_n == 0, "no subclass runs after both removed");
        ok(!RemoveWindowSubclass(lv, 41), "double remove refused");
    }

    /* ---- PSHEET ------------------------------------------------------------------------------ */
    {
        W32_PROPSHEETPAGEW pages[2];
        memset(pages, 0, sizeof pages);
        pages[0].dwSize = (uint32_t)sizeof pages[0];
        pages[0].dwFlags = W32_PSP_DLGINDIRECT;
        pages[0].pResource = tmpl1;
        static uint16_t cap1[] = {'P','1',0};
        static uint16_t cap2[] = {'P','2',0};
        static uint16_t sheet[] = {'A','8',' ',0};
        pages[0].pszTitle = cap1;
        pages[0].pfnDlgProc = a8_page1;
        pages[0].lParam = 0x51;
        pages[1].dwSize = (uint32_t)sizeof pages[0];
        pages[1].dwFlags = W32_PSP_DLGINDIRECT;
        pages[1].pResource = tmpl2;
        pages[1].pszTitle = cap2;
        pages[1].pfnDlgProc = a8_page2;
        pages[1].lParam = 0x52;

        W32_PROPSHEETHEADERW ph;
        memset(&ph, 0, sizeof ph);
        ph.dwSize = (uint32_t)sizeof ph;
        ph.dwFlags = W32_PSH_PROPSHEETPAGE;
        ph.hwndParent = parent;
        ph.pszCaption = sheet;
        ph.nPages = 2;
        ph.ppsp = pages;

        W32_INT_PTR r = PropertySheetW(&ph);
        ok(r == W32_IDOK, "the sheet ended IDOK (got %lld)",
           (long long)r);
        ok(p1_init == 1 && p2_init == 1, "both pages initialised (%d,%d)",
           p1_init, p2_init);
        ok(p_btn_found, "the template's button became a window");
        ok(p1_active >= 1 && p2_active >= 1, "both pages activated");
        ok(p1_kill == 1, "the switch killed page 1");
        ok(p1_apply0 == 1 && p2_apply0 == 1,
           "Apply-now hit both pages with lParam FALSE (%d,%d)",
           p1_apply0, p2_apply0);
        ok(p1_apply1 == 1 && p2_apply1 == 1,
           "OK hit both pages with lParam TRUE (%d,%d)",
           p1_apply1, p2_apply1);
    }

    /* ---- REFUSE ------------------------------------------------------------------------------- */
    {
        W32_HICON out = 0;
        /* zero cell: E_INVALIDARG, the documented contract */
        ok(LoadIconWithScaleDown((W32_HINSTANCE)1, (const uint16_t *)(uintptr_t)1,
                                 0, 0, &out) == (int32_t)0x80070057u,
           "scale-down refuses a zero cell with E_INVALIDARG");
        /* no resource module: a real HRESULT, not a silent fake */
        ok(LoadIconWithScaleDown((W32_HINSTANCE)1, (const uint16_t *)(uintptr_t)1,
                                 16, 16, &out) == (int32_t)0x8007007Eu,
           "scale-down answers not-found as an HRESULT");
        SetLastError(0);
        ok(SendMessageW(tb, W32_TB_CUSTOMIZE, 0, 0) == 0,
           "TB_CUSTOMIZE returns FALSE");
        ok(GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "the customisation refusal names itself (err %u)", GetLastError());
        /* _TrackMouseEvent forwards to the W32A-5 entry */
        W32_TRACKMOUSEEVENT tme;
        memset(&tme, 0, sizeof tme);
        tme.cbSize = (uint32_t)sizeof tme;
        tme.dwFlags = W32_TME_LEAVE;
        tme.hwndTrack = parent;
        ok(_TrackMouseEvent(&tme) == 1, "TME_LEAVE arms on a live window");
        tme.hwndTrack = 0;
        ok(_TrackMouseEvent(&tme) == 0, "a NULL track window is refused");
    }
    printf("w32a8: %d checks, %d fails\n", checks, fails);
    return fails ? 1 : 0;
}
