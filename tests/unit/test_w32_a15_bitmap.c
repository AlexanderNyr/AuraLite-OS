/* test_w32_a15_bitmap.c — W32APP_PLAN.md phase W32A-15 host gate:
 * LoadBitmapW / w32_gdi_bitmap_from_dib, the packed-DIB -> device HBITMAP path.
 *
 * The in-guest QEMU gate (test_w32a15_7zip_fixture.sh) proves the whole 7-Zip
 * app slice end to end, LoadBitmapW walking a real PE RT_BITMAP included.  This
 * host test amalgamates the GDI engine (the a7 gate's proven inclusion + fake
 * compositor) and drives w32_gdi_bitmap_from_dib directly against hand-built
 * packed DIBs -- the exact on-disk shape of an RT_BITMAP resource -- to pin the
 * palette expansion that 7-Zip's 4bpp toolbars and its 1bpp marker glyph need:
 *
 *   - 4bpp (16-colour) bottom-up: each palette index expands to the right ARGB;
 *     rows flip bottom-up -> top-down; SelectObject + GetPixel round-trips.
 *   - 1bpp (monochrome, MSB-first) and 8bpp palettised expand identically.
 *   - 24bpp and 32bpp copy straight through (BGR / BGRA byte order).
 *   - a negative header (top-down source) keeps its orientation.
 *   - malformed DIBs (short buffer, bad compression, absurd size) fail clean
 *     with ERROR_INVALID_PARAMETER, never a fault.
 *
 * Same amalgamation style and sanitizers as the a5/a6/a7 gates.
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
/* CW-1: CreateWindowExW embeds a WS_CHILD in its parent's surface, so
 * every host harness that amalgamates user32_win.c must model it. */
int ag_window_set_parent(int wid,int p){if(!wid_ok(wid))return -1;(void)p;return 0;}
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

/* Build a packed DIB (BITMAPINFOHEADER + colour table + pixels) into buf. */
static uint32_t put_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; return 4; }
static uint32_t put_i32(uint8_t *p, int32_t v) { return put_u32(p, (uint32_t)v); }

static uint32_t build_hdr(uint8_t *b, int32_t w, int32_t h, int bpp, uint32_t clrused) {
    memset(b, 0, 40);
    put_u32(b+0, 40);             /* biSize             (offset  0) */
    put_i32(b+4, w);              /* biWidth            (offset  4) */
    put_i32(b+8, h);              /* biHeight           (offset  8) */
    b[12]=1; b[13]=0;             /* biPlanes = 1       (offset 12) */
    b[14]=(uint8_t)bpp; b[15]=0;  /* biBitCount         (offset 14) */
    put_u32(b+16, 0);             /* biCompression=RGB  (offset 16) */
    put_u32(b+20, 0);             /* biSizeImage        (offset 20) */
    put_i32(b+24, 2835);          /* biXPelsPerMeter    (offset 24) */
    put_i32(b+28, 2835);          /* biYPelsPerMeter    (offset 28) */
    put_u32(b+32, clrused);       /* biClrUsed          (offset 32) */
    put_u32(b+36, 0);             /* biClrImportant     (offset 36) */
    return 40;
}
/* one RGBQUAD (B,G,R,0) */
static void put_quad(uint8_t *p, uint8_t r, uint8_t g, uint8_t bl) { p[0]=bl; p[1]=g; p[2]=r; p[3]=0; }

static W32_DWORD px(W32_HDC dc, int x, int y) { return GetPixel(dc, x, y) & 0x00FFFFFFu; }

int main(void) {
    static uint8_t buf[8192];

    /* ===== 4bpp, 16-colour, bottom-up: 4x2 ==============================
     * palette: 0=black 1=red 2=green 3=blue; the rest grey.
     * bottom row (stored first) = 0 1 2 3 ; top row = 3 2 1 0.
     * After the bottom-up flip the DISPLAYED top row is 3 2 1 0. */
    {
        uint32_t o = build_hdr(buf, 4, 2, 4, 16);
        put_quad(buf+o+0*4,   0,   0,   0);
        put_quad(buf+o+1*4, 255,   0,   0);
        put_quad(buf+o+2*4,   0, 255,   0);
        put_quad(buf+o+3*4,   0,   0, 255);
        for (int i = 4; i < 16; i++) put_quad(buf+o+i*4, 128,128,128);
        uint8_t *pix = buf + o + 16*4;         /* stride = ((4*4+31)/32)*4 = 4 */
        pix[0] = 0x01; pix[1] = 0x23;          /* bottom row: 0,1,2,3 */
        pix[4] = 0x32; pix[5] = 0x10;          /* top row:    3,2,1,0 */
        uint32_t dibsz = o + 16*4 + 8;

        W32_HBITMAP hb = w32_gdi_bitmap_from_dib(buf, dibsz);
        CK(hb != NULL);
        W32_BITMAP bm; memset(&bm, 0, sizeof bm);
        CK(GetObjectW(hb, sizeof bm, &bm) == (int)sizeof bm);
        CK(bm.bmWidth == 4 && bm.bmHeight == 2);

        W32_HDC dc = CreateCompatibleDC(0);
        CK(dc != NULL);
        W32_HGDIOBJ old = SelectObject(dc, hb);
        /* displayed top row (y=0) is the source top row 3,2,1,0 =
         * blue,green,red,black.  GetPixel returns a COLORREF (0x00BBGGRR). */
        CK(px(dc, 0, 0) == 0x00FF0000u);   /* blue  = RGB(0,0,255)   */
        CK(px(dc, 1, 0) == 0x0000FF00u);   /* green = RGB(0,255,0)   */
        CK(px(dc, 2, 0) == 0x000000FFu);   /* red   = RGB(255,0,0)   */
        CK(px(dc, 3, 0) == 0x00000000u);   /* black                  */
        /* displayed bottom row (y=1) is source bottom row 0,1,2,3 */
        CK(px(dc, 0, 1) == 0x00000000u);   /* black */
        CK(px(dc, 3, 1) == 0x00FF0000u);   /* blue  */
        SelectObject(dc, old);
        DeleteObject(hb);
        DeleteDC(dc);
    }

    /* ===== 1bpp monochrome, MSB-first: 8x1 ============================= */
    {
        uint32_t o = build_hdr(buf, 8, 1, 1, 2);
        put_quad(buf+o+0*4,  10, 20, 30);      /* index 0 -> RGB(10,20,30)  */
        put_quad(buf+o+1*4, 200,210,220);      /* index 1 -> RGB(200,210,220)*/
        uint8_t *pix = buf + o + 2*4;          /* stride = 4 */
        pix[0] = 0xA5;                          /* 1010 0101, MSB = pixel 0  */
        uint32_t dibsz = o + 2*4 + 4;
        W32_HBITMAP hb = w32_gdi_bitmap_from_dib(buf, dibsz);
        CK(hb != NULL);
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HGDIOBJ old = SelectObject(dc, hb);
        W32_DWORD c0 = 10 | (20u<<8) | (30u<<16);
        W32_DWORD c1 = 200 | (210u<<8) | (220u<<16);
        int bits[8] = {1,0,1,0,0,1,0,1};
        for (int x = 0; x < 8; x++) CK(px(dc,x,0) == (bits[x] ? c1 : c0));
        SelectObject(dc, old); DeleteObject(hb); DeleteDC(dc);
    }

    /* ===== 8bpp palettised, bottom-up: 3x1 ============================= */
    {
        uint32_t o = build_hdr(buf, 3, 1, 8, 256);
        for (int i = 0; i < 256; i++) put_quad(buf+o+i*4, (uint8_t)i, (uint8_t)(i^0x55), (uint8_t)(i^0xAA));
        uint8_t *pix = buf + o + 256*4;        /* stride = ((3*8+31)/32)*4 = 4 */
        pix[0]=1; pix[1]=2; pix[2]=3;
        uint32_t dibsz = o + 256*4 + 4;
        W32_HBITMAP hb = w32_gdi_bitmap_from_dib(buf, dibsz);
        CK(hb != NULL);
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HGDIOBJ old = SelectObject(dc, hb);
        for (int i = 1; i <= 3; i++) {
            W32_DWORD want = (uint8_t)i | ((uint32_t)(uint8_t)(i^0x55)<<8) | ((uint32_t)(uint8_t)(i^0xAA)<<16);
            CK(px(dc, i-1, 0) == want);
        }
        SelectObject(dc, old); DeleteObject(hb); DeleteDC(dc);
    }

    /* ===== 24bpp BGR straight-through, bottom-up: 2x1 ================== */
    {
        uint32_t o = build_hdr(buf, 2, 1, 24, 0);
        uint8_t *pix = buf + o;                /* stride = ((2*24+31)/32)*4 = 8 */
        pix[0]=0x11; pix[1]=0x22; pix[2]=0x33; /* px0: B,G,R = 11,22,33 */
        pix[3]=0x44; pix[4]=0x55; pix[5]=0x66; /* px1: B,G,R = 44,55,66 */
        uint32_t dibsz = o + 8;
        W32_HBITMAP hb = w32_gdi_bitmap_from_dib(buf, dibsz);
        CK(hb != NULL);
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HGDIOBJ old = SelectObject(dc, hb);
        CK(px(dc,0,0) == (0x33u | (0x22u<<8) | (0x11u<<16)));
        CK(px(dc,1,0) == (0x66u | (0x55u<<8) | (0x44u<<16)));
        SelectObject(dc, old); DeleteObject(hb); DeleteDC(dc);
    }

    /* ===== 32bpp BGRA, TOP-DOWN (negative height): 1x2 ================= */
    {
        uint32_t o = build_hdr(buf, 1, -2, 32, 0);
        uint8_t *pix = buf + o;                /* stride = 4; 2 rows, top-down */
        pix[0]=0xAA; pix[1]=0xBB; pix[2]=0xCC; pix[3]=0xFF;  /* top row px */
        pix[4]=0x01; pix[5]=0x02; pix[6]=0x03; pix[7]=0xFF;  /* bottom row px */
        uint32_t dibsz = o + 8;
        W32_HBITMAP hb = w32_gdi_bitmap_from_dib(buf, dibsz);
        CK(hb != NULL);
        W32_BITMAP bm; memset(&bm,0,sizeof bm);
        CK(GetObjectW(hb, sizeof bm, &bm) == (int)sizeof bm);
        CK(bm.bmWidth == 1 && bm.bmHeight == 2);
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HGDIOBJ old = SelectObject(dc, hb);
        /* top-down: displayed top row is the first stored row */
        CK(px(dc,0,0) == (0xCCu | (0xBBu<<8) | (0xAAu<<16)));
        CK(px(dc,0,1) == (0x03u | (0x02u<<8) | (0x01u<<16)));
        SelectObject(dc, old); DeleteObject(hb); DeleteDC(dc);
    }

    /* ===== malformed DIBs fail clean (no fault) ======================== */
    {
        w32_set_last_error(0);
        CK(w32_gdi_bitmap_from_dib(buf, 12) == NULL);            /* too short */
        CK(w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER);

        build_hdr(buf, 4, 4, 4, 16);
        put_u32(buf+16, 1);                                      /* biCompression = BI_RLE8 */
        CK(w32_gdi_bitmap_from_dib(buf, 8192) == NULL);          /* not BI_RGB */

        build_hdr(buf, 100000, 100000, 32, 0);                   /* absurd size */
        CK(w32_gdi_bitmap_from_dib(buf, 8192) == NULL);

        build_hdr(buf, 4, 4, 7, 16);                             /* bpp not in {1,4,8,24,32} */
        CK(w32_gdi_bitmap_from_dib(buf, 8192) == NULL);
    }

    if (f15 == 0) printf("W32A15-BITMAP-OK (%d checks)\n", n15);
    else          printf("W32A15-BITMAP-FAIL (%d/%d failed)\n", f15, n15);
    return f15 ? 1 : 0;
}
