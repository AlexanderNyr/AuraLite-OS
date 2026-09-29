/* test_w32_a16_layout_icon.c — W32APP_PLAN.md phase W32A-16 host gate:
 * the two REAL personality slices the Notepad++ app gate added, driven
 * directly under ASan/UBSan.
 *
 *   1. DeferWindowPos family (user32_win.c): BeginDeferWindowPos /
 *      DeferWindowPos / EndDeferWindowPos.  Notepad++ moves and resizes its
 *      docked panels, splitters, tab bar and edit view in one flush.  The
 *      test batches several real windows, proves the batch is deferred (no
 *      geometry moves until EndDeferWindowPos), that the flush applies every
 *      entry, that the HDWP is consumed, and that a bad child frees the HDWP
 *      and returns NULL (the Win32 contract).
 *
 *   2. CreateIconIndirect / GetIconInfo (w32_gdi.c): the icon<->bitmap
 *      bridge.  The test proves a lossless icon -> GetIconInfo ->
 *      CreateIconIndirect -> icon round-trip for opaque/transparent pixels,
 *      the colour bitmap reads back through GetPixel, the mask carries the
 *      alpha, the no-mask path treats the colour bitmap as opaque, and the
 *      malformed inputs fail clean with ERROR_INVALID_PARAMETER.
 *
 * The in-guest QEMU gate (test_w32a16_npp_fixture.sh) drives the same names
 * end to end against real notepad++.exe imports.  Same amalgamation style
 * and sanitizers as the a5/a7/a15 gates.
 * SPDX-License-Identifier: Apache-2.0 */

#define _POSIX_C_SOURCE 200809L
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
#include "w32/w32_teb.h"
#include "w32/user32.h"
#include "w32/gdi32.h"

/* The W32A-15 checks live below the amalgamation (CK / main). */

/* ---------------------------------------------------------------
 * Host primitives (tid, ticks, TEB, events, sleep) -- the a6 set.
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
 * The fake compositor (a5/a6 gate shape).  The W32A-7 additions: the
 * windows have real content pixels (window-DC draws raster into a temp
 * and hand it to ag_blit_alpha -- here that lands in the window's own
 * buffer so GetPixel/ag_get_pixel round-trips), and the theme DPI is a
 * knob the DPI fixture turns (the guest path reads the real theme).
 *
 * Everything below is defined BEFORE the amalgamated sources with the
 * exact signatures their host blocks declare; the two mirror-struct
 * calls (ag_theme_get, ag_poll_event) are defined AFTER the includes,
 * when the mirror typedefs exist -- the a6 gate's ordering.
 * ===================================================================== */
#define FAKE_WINS 64
typedef struct {
    int in_use;
    int32_t x, y;
    uint32_t w, h;
    uint32_t px[128 * 128];             /* content pixels, ARGB */
    struct { uint32_t type; int32_t x, y; uint32_t key;
             uint8_t buttons, mods; uint16_t data; } evq[32];
    int evq_head, evq_tail;
} fake_win_t;
static fake_win_t fw[FAKE_WINS];

static int blit_alpha_calls;
static int32_t blit_last[4];
static uint32_t theme_dpi = 96;

int ag_window_create(int32_t x, int32_t y, uint32_t w, uint32_t h,
                     const char *t, uint32_t fl) {
    (void)x; (void)y; (void)t; (void)fl;
    for (int i = 0; i < FAKE_WINS; i++)
        if (!fw[i].in_use) {
            memset(&fw[i], 0, sizeof fw[i]);
            fw[i].in_use = 1;
            if (w > 128) w = 128;
            if (h > 128) h = 128;
            fw[i].x = x; fw[i].y = y;
            fw[i].w = w; fw[i].h = h;
            return i + 1;
        }
    return -1;
}
static int wid_ok(int wid) { return wid >= 1 && wid <= FAKE_WINS && fw[wid-1].in_use; }
int ag_window_show(int wid){return wid_ok(wid)?0:-1;}
int ag_window_hide(int wid){return wid_ok(wid)?0:-1;}
int ag_window_destroy(int wid){if(!wid_ok(wid))return -1;fw[wid-1].in_use=0;return 0;}
int ag_window_focus(int wid){return wid_ok(wid)?0:-1;}
int ag_window_minimize(int wid){return wid_ok(wid)?0:-1;}
int ag_window_maximize(int wid){return wid_ok(wid)?0:-1;}
int ag_window_restore(int wid){return wid_ok(wid)?0:-1;}
int ag_window_move(int wid,int32_t x,int32_t y){if(!wid_ok(wid))return -1;fw[wid-1].x=x;fw[wid-1].y=y;return 0;}
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
int ag_window_get_pos(int wid,int32_t*x,int32_t*y){if(!wid_ok(wid))return -1;if(x)*x=fw[wid-1].x;if(y)*y=fw[wid-1].y;return 0;}
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
uint32_t w32_gdi_host_dpi(void){return theme_dpi;}

/* ===================================================================== *
 * The personality, amalgamated (a5/a6 gate style).
 * ===================================================================== */
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/user32_win.c"
#include "../../w32/src/user32.c"
#include "../../w32/src/w32_gdi.c"      /* W32A-7: the GDI engine */

/* mirror-struct calls: defined after the includes, like the a6 gate */
int ag_theme_get(ui_theme_t *out){if(out)memset(out,0,sizeof(ui_theme_t));return 0;}
int ag_poll_event(int wid,ui_event_t *out){
    if(wid<1||wid>FAKE_WINS||!out)return -1;
    fake_win_t*w=&fw[wid-1];
    if(w->evq_head==w->evq_tail)return 0;
    memcpy(out,&w->evq[w->evq_head],sizeof w->evq[0]);
    w->evq_head=(w->evq_head+1)%32;
    return 1;
}

/* ---- the suite's own checks ------------------------------------------ */
static int n15, f15;
#define CK(x) do { ++n15; if (!(x)) { ++f15; \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); } } while (0)

/* GetPixel returns a COLORREF (0x00BBGGRR); mask off the top byte. */
static W32_DWORD px(W32_HDC dc, int x, int y) { return GetPixel(dc, x, y) & 0x00FFFFFFu; }

/* A minimal top-level window class + window so the layout calls have real
 * HWNDs to move (WS_POPUP: the zeroed theme gives it no decoration, so
 * GetWindowRect is exactly pos..pos+size). */
static const uint16_t CLS16[6] = { 'A','1','6','W', 'n', 0 };
static W32_HWND mkwin(int32_t x, int32_t y, int32_t w, int32_t h) {
    return CreateWindowExW(0, CLS16, CLS16, W32_WS_POPUP, x, y, w, h,
                           0, 0, 0, 0);
}
static void rect_of(W32_HWND hw, W32_RECT *r) { GetWindowRect(hw, r); }


int main(void) {
    /* Register the popup class once. */
    W32_WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = CLS16;
    CK(RegisterClassExW(&wc) != 0);

    /* =====================================================================
     * 1. DeferWindowPos: batch move+resize, deferred until End.
     * ===================================================================== */
    {
        W32_HWND a = mkwin(0,   0,   100, 40);
        W32_HWND b = mkwin(10,  10,  120, 50);
        W32_HWND c = mkwin(20,  20,  90,  60);
        CK(a && b && c);

        W32_HDWP h = BeginDeferWindowPos(3);
        CK(h != 0);
        W32_UINT fl = W32_SWP_NOZORDER | W32_SWP_NOACTIVATE;
        CK(DeferWindowPos(h, a, 0, 200, 100, 110, 80, fl) == h);
        CK(DeferWindowPos(h, b, 0, 210, 110, 120, 90, fl) == h);
        CK(DeferWindowPos(h, c, 0, 220, 120, 100, 70, fl) == h);

        /* deferred: nothing has moved yet */
        W32_RECT r;
        rect_of(a, &r);
        CK(r.left == 0 && r.top == 0 && r.right == 100 && r.bottom == 40);
        rect_of(c, &r);
        CK(r.left == 20 && r.top == 20 && r.right == 110 && r.bottom == 80);

        /* flush the batch */
        CK(EndDeferWindowPos(h) == W32_TRUE);

        rect_of(a, &r);
        CK(r.left == 200 && r.top == 100 && r.right == 310 && r.bottom == 180);
        rect_of(b, &r);
        CK(r.left == 210 && r.top == 110 && r.right == 330 && r.bottom == 200);
        rect_of(c, &r);
        CK(r.left == 220 && r.top == 120 && r.right == 320 && r.bottom == 190);

        /* the HDWP is consumed: a second End fails clean */
        w32_set_last_error(0);
        CK(EndDeferWindowPos(h) == W32_FALSE);
        CK(w32_get_last_error_raw() == W32_ERROR_INVALID_HANDLE);

        /* GetClientRect agrees with the resize (size half of the contract) */
        W32_RECT cr;
        GetClientRect(a, &cr);
        CK(cr.right == 110 && cr.bottom == 80);

        DestroyWindow(a); DestroyWindow(b); DestroyWindow(c);
    }

    /* a bad child frees the HDWP and returns NULL */
    {
        W32_HWND a = mkwin(0, 0, 10, 10);
        W32_HDWP h = BeginDeferWindowPos(2);
        CK(h != 0);
        CK(DeferWindowPos(h, a, 0, 1, 1, 5, 5, W32_SWP_NOZORDER) == h);
        w32_set_last_error(0);
        CK(DeferWindowPos(h, (W32_HWND)(uintptr_t)0xDEAD, 0, 0, 0, 1, 1,
                          W32_SWP_NOZORDER) == 0);
        CK(w32_get_last_error_raw() == W32_ERROR_INVALID_HANDLE);
        /* freed already: End reports the invalid handle */
        CK(EndDeferWindowPos(h) == W32_FALSE);
        DestroyWindow(a);
    }

    /* BeginDeferWindowPos on a NULL/garbage handle fails clean */
    {
        w32_set_last_error(0);
        CK(EndDeferWindowPos((W32_HDWP)0) == W32_FALSE);
        CK(DeferWindowPos((W32_HDWP)0, 0, 0, 0, 0, 1, 1, 0) == 0);
    }

    /* GetComboBoxInfo is documented fail-clean (no COMBOBOX control) */
    {
        W32_HWND a = mkwin(0, 0, 10, 10);
        W32_COMBOBOXINFO cbi; memset(&cbi, 0, sizeof cbi);
        cbi.cbSize = sizeof cbi;
        w32_set_last_error(0);
        CK(GetComboBoxInfo(a, &cbi) == W32_FALSE);
        CK(w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER);
        /* bad window handle path */
        CK(GetComboBoxInfo((W32_HWND)(uintptr_t)0xDEAD, &cbi) == W32_FALSE);
        DestroyWindow(a);
    }

    /* =====================================================================
     * 2. CreateIconIndirect / GetIconInfo round-trip.
     * ===================================================================== *
     * 4x2 ARGB: a mix of opaque (alpha 0xFF) and transparent (alpha 0x00)
     * pixels with distinct RGB, the only two alphas an AND-mask can carry. */
    {
        uint32_t argb[8] = {
            0xFF112233u, 0x00445566u, 0xFF778899u, 0x00AABBCCu,
            0x00010203u, 0xFF0A0B0Cu, 0xFFFFFFFFu, 0x00000000u,
        };
        W32_HICON ico = w32_gdi_icon_from_argb(4, 2, argb);
        CK(ico != 0);

        W32_ICONINFO ii; memset(&ii, 0, sizeof ii);
        CK(GetIconInfo(ico, &ii) == W32_TRUE);
        CK(ii.fIcon == W32_TRUE);
        CK(ii.xHotspot == 2 && ii.yHotspot == 1);
        CK(ii.hbmColor != 0 && ii.hbmMask != 0);

        /* colour bitmap dimensions + a pixel read-back through GetPixel.
         * pixel (0,0) is 0xFF112233 -> COLORREF 0x00332211 (R/B swapped). */
        W32_BITMAP bm; memset(&bm, 0, sizeof bm);
        CK(GetObjectW(ii.hbmColor, sizeof bm, &bm) == (int)sizeof bm);
        CK(bm.bmWidth == 4 && bm.bmHeight == 2);
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HGDIOBJ old = SelectObject(dc, ii.hbmColor);
        CK(px(dc, 0, 0) == 0x00332211u);
        CK(px(dc, 2, 0) == 0x00998877u);
        SelectObject(dc, old);

        /* mask read-back: white where transparent, black where opaque. */
        W32_HGDIOBJ oldm = SelectObject(dc, ii.hbmMask);
        CK(px(dc, 0, 0) == 0x00000000u);   /* pixel 0 opaque   -> black */
        CK(px(dc, 1, 0) == 0x00FFFFFFu);   /* pixel 1 transp.  -> white */
        SelectObject(dc, oldm);
        DeleteDC(dc);

        /* rebuild and prove a lossless round-trip for {0x00,0xFF} alpha */
        W32_HICON ico2 = CreateIconIndirect(&ii);
        CK(ico2 != 0);
        int32_t rw = 0, rh = 0;
        const uint32_t *back = w32_gdi_icon_pixels(ico2, &rw, &rh);
        CK(back != 0 && rw == 4 && rh == 2);
        int same = 1;
        for (int i = 0; i < 8; i++) if (back[i] != argb[i]) same = 0;
        CK(same);

        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }

    /* no-mask CreateIconIndirect: the colour bitmap's alpha stands (and a
     * 0-alpha colour pixel is taken opaque, as Win32 does for 24bpp). */
    {
        uint32_t src[4] = { 0xFF010203u, 0xFF040506u, 0x00070809u, 0xFF0A0B0Cu };
        W32_HBITMAP hb = CreateBitmap(2, 2, 1, 32, src);
        CK(hb != 0);
        W32_ICONINFO ii; memset(&ii, 0, sizeof ii);
        ii.fIcon = W32_TRUE; ii.hbmColor = hb; ii.hbmMask = 0;
        W32_HICON ico = CreateIconIndirect(&ii);
        CK(ico != 0);
        int32_t rw = 0, rh = 0;
        const uint32_t *back = w32_gdi_icon_pixels(ico, &rw, &rh);
        CK(back != 0 && rw == 2 && rh == 2);
        CK(back[0] == 0xFF010203u);
        CK(back[1] == 0xFF040506u);
        CK(back[2] == 0xFF070809u);   /* 0-alpha source forced opaque */
        CK(back[3] == 0xFF0A0B0Cu);
        DeleteObject(hb);
    }

    /* malformed inputs fail clean */
    {
        w32_set_last_error(0);
        CK(CreateIconIndirect(0) == 0);
        CK(w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER);

        W32_ICONINFO ii; memset(&ii, 0, sizeof ii);
        ii.hbmColor = (W32_HBITMAP)(uintptr_t)0xDEAD;  /* not a bitmap */
        CK(CreateIconIndirect(&ii) == 0);

        CK(GetIconInfo(0, &ii) == W32_FALSE);
        W32_HICON ico = w32_gdi_icon_from_argb(2, 2, (uint32_t[]){1,2,3,4});
        CK(GetIconInfo(ico, 0) == W32_FALSE);
    }

    if (f15 == 0) printf("W32A16-LAYOUT-ICON-OK (%d checks)\n", n15);
    else          printf("W32A16-LAYOUT-ICON-FAIL (%d/%d failed)\n", f15, n15);
    return f15 ? 1 : 0;
}
