/* test_w32_a7.c — W32APP_PLAN.md phase W32A-7 host gate: the GDI engine.
 *
 * What this gate proves, and how it is honest:
 *
 *   1. PIXEL OUTPUT, not call counts.  A memory-DC scene (fills, shapes,
 *      text, blits with raster ops, alpha blend, clipping, palette) is
 *      rendered by the engine and byte-compared against a REFERENCE
 *      raster written inside this file from the documented rules
 *      (w32_gdi.c's header + gdi32.h): span fills, the 4-neighbour
 *      edge predicate, even-odd polygon scanline, Bresenham, PSF2 glyph
 *      decode, brush anchoring, the ROP table, the blend formula.  The
 *      reference does not share code with the engine; where they
 *      disagree, one of them is wrong and the gate names the pixel.
 *
 *   2. FONT METRICS AGAINST THE SHIPPED FILE.  The test parses
 *      drivers/framebuffer/psf2_default_font.inc itself (its own little
 *      PSF2 reader -- no shared parser) and asserts every metric the
 *      personality reports against the file's own header fields, and
 *      the rendered glyphs against the file's own bits.
 *
 *   3. DPI IS A FIXTURE, NOT A CONSTANT.  w32_gdi_set_dpi_aware(0) must
 *      yield LOGPIXELSX/Y == 96 whatever the theme says (the Windows
 *      compatibility rule); aware + theme 144 must yield 144, clamped
 *      to [48, 480].  The integration gate re-proves the theme half
 *      against gtheme --dpi with a reboot between.
 *
 *   4. THE OBJECT MODEL.  Stock objects, SelectObject round-trips,
 *      SaveDC/RestoreDC, GetObjectA/W verbatim LOGFONTs, regions
 *      (combine + clip + readback), DIBs (formats, orientations),
 *      palettes on 8bpp surfaces, icon decode + cache + draw, and the
 *      printing family refusing by name.
 *
 * Same inclusion style as the a5/a6 gates: amalgamate the personality
 * sources, supply the compositor.  Sanitizers match a3-a6.
 */

#define _DEFAULT_SOURCE 1
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

W32ABI void SetLastError(W32_DWORD c){ w32_set_last_error(c); }

/* the enumeration callback (file scope: the engine calls it by pointer) */
static int a7_enum_count;
static int W32ABI a7_enum_cb(const W32_ENUMLOGFONTEXW *elf,
                             const W32_TEXTMETRICW *tm, W32_DWORD d,
                             W32_LPARAM lp) {
    (void)d; (void)lp; (void)tm;
    if (a7_enum_count == 0) {
        /* first (and only) call: the face is the shipped one */
        if (elf->elfLogFont.lfFaceName[0] != 'V' ||
            elf->elfLogFont.lfFaceName[1] != 'G' ||
            elf->elfLogFont.lfFaceName[2] != 'A')
            return 0;
    }
    a7_enum_count++;
    return 1;
}

/* ===================================================================== *
 * The REFERENCE raster: written from the documented rules, sharing no
 * code with the engine.  64x48 ARGB (0x00RRGGBB) buffer.
 * ===================================================================== */
#define RW 64
#define RH 48
static uint32_t ref[RW * RH];

static void ref_put(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= RW || y >= RH) return;
    ref[y * RW + x] = c & 0x00FFFFFFu;
}
static uint32_t ref_get(int x, int y) {
    if (x < 0 || y < 0 || x >= RW || y >= RH) return 0;
    return ref[y * RW + x] & 0x00FFFFFFu;
}

/* COLORREF 0x00BBGGRR -> 0x00RRGGBB */
static uint32_t cref_to_ag(W32_DWORD cr) {
    return ((cr & 0xFF) << 16) | (cr & 0xFF00) | ((cr >> 16) & 0xFF);
}

/* hatch rows, MSB first, as documented in the engine's header */
static const uint8_t ref_hatch[6][8] = {
    {0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24},
    {0x99,0x99,0x99,0x99,0x99,0x99,0x99,0x99},
    {0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81},
    {0x18,0x24,0x42,0x81,0x81,0x42,0x24,0x18},
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C},
    {0x99,0x42,0x24,0x99,0x99,0x24,0x42,0x99},
};

/* reference brush: solid colour or hatched with DC-bk gaps */
static uint32_t ref_brush_at(int x, int y, int solid, W32_DWORD color,
                             int hatch, int borg_x, int borg_y,
                             uint32_t bk_ag, int bk_opaque) {
    if (solid) return cref_to_ag(color);
    int lx = x - borg_x, ly = y - borg_y;
    if (ref_hatch[hatch][((ly % 8) + 8) % 8] & (0x80u >> (((lx % 8) + 8) % 8)))
        return cref_to_ag(color);
    if (bk_opaque) return bk_ag;
    return 0x80000000u;                    /* skip */
}

static void ref_span_brush(int x0, int x1, int y, int solid, W32_DWORD color,
                           int hatch, int bx, int by, uint32_t bk, int opq) {
    for (int x = x0; x <= x1; x++) {
        uint32_t c = ref_brush_at(x, y, solid, color, hatch, bx, by, bk, opq);
        if (c != 0x80000000u) ref_put(x, y, c);
    }
}

static int ref_in_ell(int x, int y, int l, int t, int r, int b) {
    if (r - l < 2 || b - t < 2) return 0;
    double cx = (l + r - 1) / 2.0, cy = (t + b - 1) / 2.0;
    double rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    double dx = (x - cx) / (rx + 0.5), dy = (y - cy) / (ry + 0.5);
    return dx * dx + dy * dy <= 1.0;
}
static int ref_ell_edge(int x, int y, int l, int t, int r, int b) {
    return ref_in_ell(x, y, l, t, r, b) &&
           (!ref_in_ell(x-1, y, l, t, r, b) || !ref_in_ell(x+1, y, l, t, r, b) ||
            !ref_in_ell(x, y-1, l, t, r, b) || !ref_in_ell(x, y+1, l, t, r, b));
}

static int ref_in_rrect(int x, int y, int l, int t, int r, int b, int ew, int eh) {
    if (x < l || x >= r || y < t || y >= b) return 0;
    int rx = ew / 2, ry = eh / 2;
    if (rx <= 0 || ry <= 0) return 1;
    int xl = l + rx, xr = r - rx, yt = t + ry, yb = b - ry;
    if (x >= xl && x < xr) return 1;
    if (y >= yt && y < yb) return 1;
    int cx = (x < xl) ? xl : xr, cy = (y < yt) ? yt : yb;
    double dx = (double)(x - cx) / (double)(rx + 0.5);
    double dy = (double)(y - cy) / (double)(ry + 0.5);
    return dx * dx + dy * dy <= 1.0;
}
static int ref_rrect_edge(int x, int y, int l, int t, int r, int b, int ew, int eh) {
    return ref_in_rrect(x, y, l, t, r, b, ew, eh) &&
           (!ref_in_rrect(x-1, y, l, t, r, b, ew, eh) ||
            !ref_in_rrect(x+1, y, l, t, r, b, ew, eh) ||
            !ref_in_rrect(x, y-1, l, t, r, b, ew, eh) ||
            !ref_in_rrect(x, y+1, l, t, r, b, ew, eh));
}

static void ref_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
    int err = dx - dy, x = x0, y = y0;
    for (;;) {
        ref_put(x, y, c);
        if (x == x1 && y == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

/* PSF2 decode, from the shipped file, by this test's own reader.  The
 * blob itself is already embedded by w32_gdi.c (same TU); extern-declare
 * it rather than including the .inc twice. */
extern const unsigned char psf2_default_font_data[];
/* PSF2_DEFAULT_FONT_SIZE comes with the blob (w32_gdi.c included it) */
static const uint8_t *ref_glyph_bits;
static uint32_t ref_fw, ref_fh, ref_fbytes, ref_fglyphs, ref_fhdr;
static void ref_font_init(void) {
    const uint8_t *b = psf2_default_font_data;
    ref_fhdr = b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24);
    ref_fglyphs = b[16] | (b[17] << 8) | (b[18] << 16) | (b[19] << 24);
    ref_fbytes = b[20] | (b[21] << 8) | (b[22] << 16) | (b[23] << 24);
    ref_fh = b[24] | (b[25] << 8) | (b[26] << 16) | (b[27] << 24);
    ref_fw = b[28] | (b[29] << 8) | (b[30] << 16) | (b[31] << 24);
    ref_glyph_bits = b + ref_fhdr;
}
static void ref_glyph2(int x, int y, uint8_t ch, uint32_t fg, uint32_t bk, int opq) {
    const uint8_t *g = ref_glyph_bits + (size_t)(ch % ref_fglyphs) * ref_fbytes;
    uint32_t bpr = (ref_fw + 7) / 8;
    for (uint32_t ry = 0; ry < ref_fh; ry++)
        for (uint32_t rx = 0; rx < ref_fw; rx++) {
            int on = g[ry * bpr + (rx >> 3)] & (0x80u >> (rx & 7));
            if (on) ref_put(x + (int)rx, y + (int)ry, fg);
            else if (opq) ref_put(x + (int)rx, y + (int)ry, bk);
        }
}

/* the engine's ROP set, from the codes in gdi32.h */
static uint32_t ref_rop(uint32_t rop, uint32_t s, uint32_t d, uint32_t p) {
    switch (rop) {
    case 0x00000042u: return 0;
    case 0x00FF0062u: return 0x00FFFFFFu;
    case 0x00550009u: return d ^ 0x00FFFFFFu;
    case 0x00330008u: return s ^ 0x00FFFFFFu;
    case 0x00CC0020u: return s;
    case 0x008800C6u: return s & d;
    case 0x00660046u: return s ^ d;
    case 0x00EE0086u: return s | d;
    case 0x00F00021u: return p;
    case 0x005A0049u: return p ^ d;
    }
    return 0;
}

/* ===================================================================== *
 * Scene helpers: run the same scene through the engine (memory DC) and
 * the reference, then compare byte-for-byte.
 * ===================================================================== */
static uint32_t engine_px[RW * RH];

static void engine_snapshot(W32_HDC dc) {
    W32_BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = 40;
    bi.bmiHeader.biWidth = RW;
    bi.bmiHeader.biHeight = -RH;            /* top-down: row 0 first */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = 0;
    W32_HBITMAP bmp = (W32_HBITMAP)GetCurrentObject(dc, W32_OBJ_BITMAP);
    ok(GetDIBits(dc, bmp, 0, RH, engine_px, &bi, 0) == RH, "snapshot GetDIBits");
}

static void scene_compare(const char *what) {
    int bad = -1;
    for (int i = 0; i < RW * RH; i++)
        if ((engine_px[i] & 0x00FFFFFFu) != (ref[i] & 0x00FFFFFFu)) { bad = i; break; }
    if (bad < 0) { ok(1, what); return; }
    printf("FAIL - %s: first diff at (%d,%d): engine %06x ref %06x\n",
           what, bad % RW, bad / RW,
           engine_px[bad] & 0x00FFFFFFu, ref[bad] & 0x00FFFFFFu);
    fails++; checks++;
}

static uint64_t fnv1a(const uint32_t *p, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        uint32_t v = p[i] & 0x00FFFFFFu;
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (b * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    return h;
}

int main(void) {
    ref_font_init();

    /* ---- 1. the shipped font, measured by this test ------------------- */
    {
        ok(psf2_default_font_data[0] == 0x72 &&
           psf2_default_font_data[1] == 0xB5 &&
           psf2_default_font_data[2] == 0x4A &&
           psf2_default_font_data[3] == 0x86, "shipped font is PSF2");
        ok(ref_fw == 8 && ref_fh == 16, "shipped font is 8x16 (%ux%u)",
           ref_fw, ref_fh);
        ok(ref_fglyphs == 256, "shipped font has 256 glyphs");
        ok(ref_fbytes == ref_fh * ((ref_fw + 7) / 8),
           "glyph size matches dimensions");
        ok(ref_fhdr + ref_fbytes * ref_fglyphs <= PSF2_DEFAULT_FONT_SIZE,
           "glyph data fits the blob");
    }

    /* ---- 2. memory DC + bitmap, the A-7 core scene -------------------- */
    {
        W32_HDC dc = CreateCompatibleDC(0);
        ok(dc != 0, "CreateCompatibleDC");
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, RW, RH);
        ok(bmp != 0, "CreateCompatibleBitmap 64x48");
        ok(SelectObject(dc, bmp) != 0, "SelectObject(bitmap)");
        ok(w32_gdi_obj_type((void *)GetCurrentObject(dc, W32_OBJ_BITMAP)) != 0,
           "GetCurrentObject(OBJ_BITMAP) is a table handle");

        /* background: white via PatBlt PATCOPY with the stock white brush */
        W32_HBRUSH white = (W32_HBRUSH)GetStockObject(W32_WHITE_BRUSH);
        ok((uintptr_t)white >= 0x6D700000u &&
           (uintptr_t)white <  0x6D700000u + 512u,
           "GetStockObject returns a table handle (got %p)", white);
        SelectObject(dc, white);
        ok(PatBlt(dc, 0, 0, RW, RH, W32_PATCOPY), "PatBlt PATCOPY");
        memset(ref, 0xFF, sizeof ref);          /* reference: all white */
        for (int i = 0; i < RW * RH; i++) ref[i] = 0x00FFFFFFu;

        /* opaque black rect with white pen border */
        W32_HBRUSH black = (W32_HBRUSH)GetStockObject(W32_BLACK_BRUSH);
        W32_HPEN pen = CreatePen(W32_PS_SOLID, 1, W32_RGB(255, 0, 0));
        SelectObject(dc, black);
        SelectObject(dc, pen);
        ok(Rectangle(dc, 4, 4, 24, 16), "Rectangle");
        for (int y = 4; y < 16; y++)
            for (int x = 4; x < 24; x++)
                if (y == 4 || y == 15 || x == 4 || x == 23)
                    ref_put(x, y, cref_to_ag(W32_RGB(255, 0, 0)));
                else
                    ref_put(x, y, 0);

        /* hatch-filled ellipse, transparent bk */
        SetBkMode(dc, W32_TRANSPARENT);
        W32_HBRUSH hatch = CreateHatchBrush(W32_HS_FDIAGONAL, W32_RGB(0, 0, 255));
        SelectObject(dc, hatch);
        ok(Ellipse(dc, 28, 2, 58, 20), "Ellipse hatched");
        for (int y = 2; y < 20; y++)
            for (int x = 28; x < 58; x++) {
                if (!ref_in_ell(x, y, 28, 2, 58, 20)) continue;
                if (ref_ell_edge(x, y, 28, 2, 58, 20)) {
                    /* the DC's pen is the red one selected above */
                    ref_put(x, y, cref_to_ag(W32_RGB(255, 0, 0)));
                } else {
                    uint32_t c = ref_brush_at(x, y, 0, W32_RGB(0, 0, 255),
                                              W32_HS_FDIAGONAL, 0, 0, 0, 0);
                    if (c != 0x80000000u) ref_put(x, y, c);
                }
            }

        /* roundrect with opaque bk */
        SetBkMode(dc, W32_OPAQUE);
        SetBkColor(dc, W32_RGB(0, 255, 0));
        W32_HBRUSH solid = CreateSolidBrush(W32_RGB(255, 255, 0));
        SelectObject(dc, solid);
        ok(RoundRect(dc, 6, 22, 40, 42, 10, 8), "RoundRect");
        for (int y = 22; y < 42; y++)
            for (int x = 6; x < 40; x++) {
                if (!ref_in_rrect(x, y, 6, 22, 40, 42, 10, 8)) continue;
                if (ref_rrect_edge(x, y, 6, 22, 40, 42, 10, 8))
                    ref_put(x, y, cref_to_ag(W32_RGB(255, 0, 0)));
                else
                    ref_put(x, y, cref_to_ag(W32_RGB(255, 255, 0)));
            }

        /* polygon, even-odd fill + pen outline */
        {
            W32_POINT pts[5] = {{44,24},{58,28},{50,34},{46,44},{42,32}};
            SelectObject(dc, black);
            ok(Polygon(dc, pts, 5), "Polygon");
            /* reference: even-odd scanline spans with the black brush */
            for (int y = 24; y <= 44; y++) {
                int xs[8], nx = 0;
                for (int i = 0, j = 4; i < 5; j = i++) {
                    int yi = pts[i].y, yj = pts[j].y;
                    if ((yi > y) != (yj > y)) {
                        int xint = pts[j].x + (y - yj) * (pts[i].x - pts[j].x) / (yi - yj);
                        if (nx < 8) xs[nx++] = xint;
                    }
                }
                for (int a = 1; a < nx; a++) {
                    int v = xs[a], b2 = a - 1;
                    while (b2 >= 0 && xs[b2] > v) { xs[b2+1] = xs[b2]; b2--; }
                    xs[b2+1] = v;
                }
                for (int a = 0; a + 1 < nx; a += 2)
                    ref_span_brush(xs[a], xs[a+1], y, 1, 0, 0, 0, 0, 0, 0);
            }
            for (int i = 0, j = 4; i < 5; j = i++)
                ref_line(pts[j].x, pts[j].y, pts[i].x, pts[i].y,
                         cref_to_ag(W32_RGB(255, 0, 0)));
        }

        /* text: opaque white box behind 'Ab' black glyphs, TA_LEFT|TA_TOP */
        SetTextColor(dc, W32_RGB(0, 0, 0));
        SetBkColor(dc, W32_RGB(255, 255, 255));
        ok(TextOutA(dc, 8, 44 - 16 + 2, "Ab", 2), "TextOutA"); /* y=30 */
        ref_glyph2(8, 30, 'A', 0, 0x00FFFFFFu, 1);
        ref_glyph2(8 + 8, 30, 'b', 0, 0x00FFFFFFu, 1);

        /* alignment: TA_RIGHT|TA_BOTTOM at a known anchor */
        SetTextAlign(dc, W32_TA_RIGHT | W32_TA_BOTTOM);
        TextOutA(dc, 60, 46, "C", 1);
        ref_glyph2(60 - 8, 46 - 16, 'C', 0, 0x00FFFFFFu, 1);
        SetTextAlign(dc, W32_TA_LEFT | W32_TA_TOP);

        /* clip region: exclude a band, then flood a solid rect over it */
        ok(ExcludeClipRect(dc, 0, 0, RW, 4) != W32_RGN_ERROR, "ExcludeClipRect");
        SelectObject(dc, solid);
        ok(Rectangle(dc, 0, 0, RW, 4), "Rectangle under clip");  /* pen is red */
        /* reference: rows 0..3 excluded => only the pen's top/bottom rows
         * outside the excluded area survive; the excluded band is [0,4) so
         * nothing at all is drawn (the whole rect was inside) */
        SelectClipRgn(dc, 0);                    /* unclip for the rest */

        /* SetPixel + GetPixel round trip */
        SetPixel(dc, 2, 2, W32_RGB(12, 34, 56));
        ref_put(2, 2, cref_to_ag(W32_RGB(12, 34, 56)));
        ok(GetPixel(dc, 2, 2) == W32_RGB(12, 34, 56), "GetPixel round trip");
        ok(GetPixel(dc, -5, -5) == W32_CLR_INVALID, "GetPixel outside");

        engine_snapshot(dc);
        scene_compare("A-7 core scene, byte-exact vs reference raster");
        printf("scene hash: %016llx\n",
               (unsigned long long)fnv1a(engine_px, RW * RH));

        /* ---- BitBlt ROPs, src = the scene itself ---------------------- */
        {
            /* save the scene as the source, then paint ROP rects into
             * row band y=44..47 of the destination */
            W32_HDC sdc = CreateCompatibleDC(dc);
            W32_HBITMAP sbmp = CreateCompatibleBitmap(dc, 16, 4);
            SelectObject(sdc, sbmp);
            ok(BitBlt(sdc, 0, 0, 16, 4, dc, 20, 20, W32_SRCCOPY),
               "BitBlt SRCCOPY from scene");
            /* reference source = engine's (20,20)..(36,24) */
            uint32_t src[16 * 4];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 16; x++)
                    src[y * 16 + x] = ref_get(20 + x, 20 + y);

            struct { uint32_t rop; const char *name; } rops[] = {
                { W32_BLACKNESS, "BLACKNESS" }, { W32_WHITENESS, "WHITENESS" },
                { W32_DSTINVERT, "DSTINVERT" }, { W32_NOTSRCCOPY, "NOTSRCCOPY" },
                { W32_SRCAND, "SRCAND" },       { W32_SRCINVERT, "SRCINVERT" },
                { W32_SRCPAINT, "SRCPAINT" },   { W32_PATINVERT, "PATINVERT" },
            };
            for (unsigned i = 0; i < sizeof rops / sizeof rops[0]; i++) {
                int x0 = (int)(i * 8);
                ok(BitBlt(dc, x0, 44, 8, 4, sdc, 0, 0, rops[i].rop),
                   "BitBlt %s", rops[i].name);
                SelectObject(dc, white);        /* PATINVERT's pattern */
                for (int y = 44; y < 48; y++)
                    for (int x = x0; x < x0 + 8; x++) {
                        uint32_t d = ref_get(x, y);
                        uint32_t p = 0x00FFFFFFu;
                        uint32_t s = src[(y - 44) * 16 + (x - x0)];
                        uint32_t o = ref_rop(rops[i].rop, s, d, p);
                        ref_put(x, y, o);
                    }
            }
            /* PATCOPY via PatBlt over the last cells */
            SelectObject(dc, hatch);
            ok(PatBlt(dc, 56, 44, 8, 4, W32_PATCOPY), "PatBlt PATCOPY hatch");
            for (int y = 44; y < 48; y++)
                for (int x = 56; x < 64; x++) {
                    /* bk mode is OPAQUE and bk colour white (set for the
                     * text pass): hatch gaps paint white, not skip */
                    uint32_t c = ref_brush_at(x, y, 0, W32_RGB(0, 0, 255),
                                              W32_HS_FDIAGONAL, 0, 0,
                                              0x00FFFFFFu, 1);
                    if (c != 0x80000000u) ref_put(x, y, c);
                }
            engine_snapshot(dc);
            scene_compare("ROP band, byte-exact vs reference");
        }

        /* ---- unsupported ROP refuses ---------------------------------- */
        {
            ok(!BitBlt(dc, 0, 0, 4, 4, dc, 0, 0, 0x00C000CAu /*MERGECOPY*/),
               "unsupported ROP refuses");
            ok(GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
               "unsupported ROP names ERROR_CALL_NOT_IMPLEMENTED");
        }

        /* ---- GdiAlphaBlend -------------------------------------------- */
        {
            W32_HDC sdc = CreateCompatibleDC(dc);
            W32_HBITMAP sbmp = CreateCompatibleBitmap(dc, 8, 4);
            SelectObject(sdc, sbmp);
            /* source: solid blue via PatBlt */
            SelectObject(sdc, (W32_HBRUSH)GetStockObject(W32_WHITE_BRUSH));
            /* paint source blue by SetPixel (deterministic) */
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++)
                    SetPixel(sdc, x, y, W32_RGB(0, 0, 255));
            W32_BLENDFUNCTION bf = { 0, 0, 128, 0 };
            ok(GdiAlphaBlend(dc, 0, 44, 8, 4, sdc, 0, 0, 8, 4, bf),
               "GdiAlphaBlend 50% blue");
            for (int y = 44; y < 48; y++)
                for (int x = 0; x < 8; x++) {
                    uint32_t d = ref_get(x, y);
                    uint32_t s = 0x000000FFu;   /* AG blue */
                    uint32_t out = 0;
                    for (int ch = 0; ch < 3; ch++) {
                        uint32_t sc = (s >> (ch * 8)) & 0xFF;
                        uint32_t dc2 = (d >> (ch * 8)) & 0xFF;
                        out |= ((sc * 128 + dc2 * 127) / 255) << (ch * 8);
                    }
                    ref_put(x, y, out);
                }
            engine_snapshot(dc);
            scene_compare("alpha blend, byte-exact vs reference");
        }

        DeleteDC(dc);
    }

    /* ---- 3. font metrics vs the shipped file -------------------------- */
    {
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, 4, 4);
        SelectObject(dc, bmp);
        W32_TEXTMETRICA tm;
        ok(GetTextMetricsA(dc, &tm), "GetTextMetricsA");
        ok(tm.tmHeight == (int32_t)ref_fh, "tmHeight == file height (%d)",
           tm.tmHeight);
        ok(tm.tmAscent == (int32_t)ref_fh - 2, "tmAscent == height-2");
        ok(tm.tmDescent == 2, "tmDescent == 2");
        ok(tm.tmAveCharWidth == (int32_t)ref_fw, "tmAveCharWidth == 8");
        ok(tm.tmMaxCharWidth == (int32_t)ref_fw, "tmMaxCharWidth == 8");
        ok(tm.tmFirstChar == 0 && tm.tmLastChar == 255,
           "charset range 0..255");
        ok(tm.tmPitchAndFamily & 0x01, "fixed pitch");
        W32_TEXTMETRICW tmw;
        ok(GetTextMetricsW(dc, &tmw), "GetTextMetricsW");
        ok(tmw.tmHeight == tm.tmHeight && tmw.tmAveCharWidth == tm.tmAveCharWidth,
           "A/W metrics agree");

        W32_SIZE sz;
        ok(GetTextExtentPoint32A(dc, "Hello", 5, &sz), "GetTextExtentPoint32A");
        ok(sz.cx == 5 * 8 && sz.cy == 16, "extent 5 chars = 40x16");
        GetTextExtentPoint32A(dc, "Hello", -1, &sz);
        ok(sz.cx == 5 * 8, "extent with NUL-terminated len=-1");
        int32_t fit = -1, dx[8];
        ok(GetTextExtentExPointA(dc, "Hello", 5, 20, &fit, dx, &sz),
           "GetTextExtentExPointA");
        ok(fit == 2, "max extent 20 fits 2 glyphs (got %d)", fit);
        ok(dx[0] == 8 && dx[1] == 8, "dx array is per-glyph advance");
        int32_t cw[4];
        ok(GetCharWidth32A(dc, 'A', 'D', cw), "GetCharWidth32A");
        ok(cw[0] == 8 && cw[3] == 8, "char widths are 8");
        W32_ABCFLOAT abc[2];
        ok(GetCharABCWidthsFloatA(dc, 'A', 'B', abc), "GetCharABCWidthsFloatA");
        ok(abc[0].abcfA == 0 && abc[0].abcfB == 8 && abc[0].abcfC == 0,
           "ABC widths A=0 B=8 C=0");

        /* outline metrics: size query then fill */
        W32_UINT need = GetOutlineTextMetricsA(dc, 0, 0);
        ok(need > sizeof(W32_OUTLINETEXTMETRICA), "OTM size query");
        W32_OUTLINETEXTMETRICA *otm = malloc(need);
        ok(GetOutlineTextMetricsA(dc, need, otm) == need, "OTM fill");
        ok(otm->otmTextMetrics.tmHeight == 16, "OTM height");
        ok(strcmp((char *)otm + otm->otmFaceName, "VGA 8x16") == 0,
           "OTM face name is the shipped face");
        free(otm);

        /* enumeration: exactly one face, named */
        a7_enum_count = 0;
        ok(EnumFontFamiliesExW(dc, 0, (void *)a7_enum_cb, 0, 0) == 1,
           "EnumFontFamiliesExW returns the callback's value");
        ok(a7_enum_count == 1, "exactly one face enumerated");

        /* GCP */
        W32_GCP_RESULTSW g;
        uint16_t wbuf[8]; int32_t dxs[8]; uint16_t glyphs[8];
        for (int i = 0; i < 4; i++) wbuf[i] = (uint16_t)"ABCD"[i];
        memset(&g, 0, sizeof g);
        g.lpDx = dxs; g.lpGlyphs = glyphs; g.nGlyphs = 8;
        W32_DWORD r = GetCharacterPlacementW(dc, wbuf, 4, 100, &g, 0);
        ok(g.nGlyphs == 4 && g.nMaxFit == 4, "GCP placed 4 glyphs");
        ok(g.lpGlyphs[0] == 'A' && g.lpDx[3] == 8, "GCP identity mapping");
        ok(r != 0, "GCP returns flags");

        W32_CHARSETINFO csi;
        memset(&csi, 0, sizeof csi);
        ok(TranslateCharsetInfo(0, &csi, 0) &&
           csi.ciCharset == W32_ANSI_CHARSET && csi.ciACP == 1252,
           "TCI flags=0 defaults to ANSI/1252");
        W32_DWORD cp = 1252;
        ok(TranslateCharsetInfo(&cp, &csi, W32_TCI_SRCCODEPAGE) &&
           csi.ciCharset == W32_ANSI_CHARSET,
           "TCI codepage 1252 -> ANSI");
        W32_DWORD oem = W32_OEM_CHARSET;
        ok(TranslateCharsetInfo(&oem, &csi, W32_TCI_SRCCHARSET) &&
           csi.ciACP == 437,
           "TCI OEM charset -> CP437");
        DeleteDC(dc);
    }

    /* ---- 4. DPI: manifest awareness gates the theme value ------------- */
    {
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, 4, 4);
        SelectObject(dc, bmp);
        w32_gdi_set_dpi_aware(0);
        theme_dpi = 192;
        ok(GetDeviceCaps(dc, W32_LOGPIXELSX) == 96,
           "unaware app sees 96 regardless of theme");
        ok(GetDeviceCaps(dc, W32_LOGPIXELSY) == 96,
           "LOGPIXELSY matches LOGPIXELSX");
        w32_gdi_set_dpi_aware(1);
        theme_dpi = 144;
        ok(GetDeviceCaps(dc, W32_LOGPIXELSX) == 144,
           "aware app sees the theme DPI");
        theme_dpi = 1000;
        ok(GetDeviceCaps(dc, W32_LOGPIXELSX) == 480, "DPI clamps at 480");
        theme_dpi = 10;
        ok(GetDeviceCaps(dc, W32_LOGPIXELSX) == 48, "DPI clamps at 48");
        theme_dpi = 96;
        ok(GetDeviceCaps(dc, W32_HORZRES) == 640 && GetDeviceCaps(dc, W32_VERTRES) == 480,
           "screen geometry");
        ok(GetDeviceCaps(dc, W32_TECHNOLOGY) == W32_DT_RASDISPLAY,
           "technology is RASDISPLAY");
        ok(GetDeviceCaps(dc, W32_NUMFONTS) == 1, "one face");
        DeleteDC(dc);
    }

    /* ---- 5. the object model ------------------------------------------ */
    {
        /* SelectObject round-trips return the previous object */
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, 8, 8);
        SelectObject(dc, bmp);
        W32_HPEN p1 = CreatePen(W32_PS_SOLID, 2, W32_RGB(1, 2, 3));
        W32_HPEN p2 = CreatePen(W32_PS_DOT, 1, W32_RGB(4, 5, 6));
        ok(SelectObject(dc, p1) == 0, "first pen select returns NULL stock");
        ok(SelectObject(dc, p2) == p1, "second select returns the first pen");
        ok(GetCurrentObject(dc, W32_OBJ_PEN) == p2, "GetCurrentObject pen");
        W32_LOGPEN lp;
        ok(GetObjectW(p2, sizeof lp, &lp) == (int32_t)sizeof lp, "GetObjectW pen size");
        ok(lp.lopnStyle == W32_PS_DOT && lp.lopnColor == W32_RGB(4, 5, 6),
           "LOGPEN round trip");
        ok(lp.lopnWidth.x == 1, "pen width recorded");

        /* brush */
        W32_HBRUSH hb = CreateHatchBrush(W32_HS_CROSS, W32_RGB(9, 8, 7));
        W32_LOGBRUSH lb;
        ok(GetObjectA(hb, sizeof lb, &lb) == (int32_t)sizeof lb, "GetObjectA brush");
        ok(lb.lbStyle == 2 /*BS_HATCHED*/ && lb.lbColor == W32_RGB(9, 8, 7) &&
           lb.lbHatch == W32_HS_CROSS, "LOGBRUSH round trip");

        /* font: the LOGFONT comes back verbatim (the documented contract
         * for a bitmap-font personality) */
        W32_LOGFONTW lf;
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -12; lf.lfWeight = 700; lf.lfCharSet = W32_ANSI_CHARSET;
        lf.lfFaceName[0] = 'V'; lf.lfFaceName[1] = 'G'; lf.lfFaceName[2] = 'A';
        W32_HFONT hf = CreateFontIndirectW(&lf);
        W32_LOGFONTW lf2;
        ok(GetObjectW(hf, sizeof lf2, &lf2) == (int32_t)sizeof lf2,
           "GetObjectW font size");
        ok(lf2.lfHeight == -12 && lf2.lfWeight == 700 &&
           lf2.lfFaceName[0] == 'V' && lf2.lfFaceName[3] == 0,
           "LOGFONTW verbatim");
        W32_LOGFONTA lfa;
        ok(GetObjectA(hf, sizeof lfa, &lfa) == (int32_t)sizeof lfa,
           "GetObjectA font converts face");
        ok(lfa.lfHeight == -12 && lfa.lfFaceName[0] == 'V' &&
           lfa.lfFaceName[3] == 0, "LOGFONTA face truncated at NUL");

        /* stocks: every id the header carries mints a usable object */
        int stock_ids[] = { W32_WHITE_BRUSH, W32_LTGRAY_BRUSH, W32_GRAY_BRUSH,
                            W32_DKGRAY_BRUSH, W32_BLACK_BRUSH, W32_NULL_BRUSH,
                            W32_WHITE_PEN, W32_BLACK_PEN, W32_NULL_PEN,
                            W32_OEM_FIXED_FONT, W32_ANSI_FIXED_FONT,
                            W32_ANSI_VAR_FONT, W32_SYSTEM_FONT,
                            W32_DEVICE_DEFAULT_FONT, W32_SYSTEM_FIXED_FONT,
                            W32_DEFAULT_GUI_FONT, W32_DEFAULT_PALETTE };
        int n_stocks = (int)(sizeof stock_ids / sizeof stock_ids[0]);
        int all_ok = 1;
        for (int i = 0; i < n_stocks; i++)
            if (!GetStockObject(stock_ids[i])) all_ok = 0;
        ok(all_ok, "all %d stock objects mint", n_stocks);
        ok(!GetStockObject(999), "bogus stock id fails");

        /* SaveDC / RestoreDC */
        SetTextColor(dc, 0x111111);
        int level = SaveDC(dc);
        ok(level >= 1, "SaveDC returns the saved level");
        SetTextColor(dc, 0x222222);
        SetBkMode(dc, W32_TRANSPARENT);
        ok(RestoreDC(dc, level), "RestoreDC to the saved level");
        /* no GetTextColor export in the union; the restore is observable
         * through SetTextColor's return (the previous colour) */
        ok(SetTextColor(dc, 0) == 0x111111, "text colour restored");
        SetBkMode(dc, W32_OPAQUE);
        SaveDC(dc); SaveDC(dc);
        ok(RestoreDC(dc, -1), "RestoreDC(-1) pops one level");
        ok(!RestoreDC(dc, 99), "RestoreDC past the stack fails");

        /* UnrealizeObject is a no-op that succeeds */
        ok(UnrealizeObject(hb), "UnrealizeObject brush");

        /* DeleteObject frees; double delete still succeeds (Win32 allows
         * it for stock-shaped flows, and A-5 made it a no-op) */
        ok(DeleteObject(p1), "DeleteObject pen");
        ok(DeleteObject(p1), "DeleteObject again is a no-op success");

        DeleteDC(dc);
    }

    /* ---- 6. regions ----------------------------------------------------- */
    {
        W32_HRGN a = CreateRectRgn(0, 0, 10, 10);
        W32_HRGN b = CreateRectRgn(5, 5, 15, 15);
        W32_HRGN r = CreateRectRgn(0, 0, 1, 1);
        ok(a && b && r, "CreateRectRgn x3");
        ok(CombineRgn(r, a, b, W32_RGN_AND) == W32_SIMPLEREGION,
           "AND of two rects is simple");
        /* read the clip back through a DC: SelectClipRgn + GetClipRgn */
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, 32, 32);
        SelectObject(dc, bmp);
        ok(SelectClipRgn(dc, r) == W32_SIMPLEREGION, "SelectClipRgn");
        W32_HRGN back = CreateRectRgn(0, 0, 1, 1);
        ok(GetClipRgn(dc, back) == 1, "GetClipRgn one rect");
        W32_HRGN probe = CreateRectRgn(0, 0, 2, 2);
        ok(CombineRgn(probe, back, probe, W32_RGN_XOR) != W32_RGN_ERROR,
           "XOR combine works");
        /* membership via RectVisible (the observable contract) */
        W32_RECT inside = { 6, 6, 9, 9 }, outside = { 0, 0, 4, 4 };
        ok(RectVisible(dc, &inside), "rect inside clip is visible");
        ok(!RectVisible(dc, &outside), "rect outside clip is not visible");
        /* ExcludeClipRect punches a hole */
        ok(ExcludeClipRect(dc, 6, 6, 8, 8) != W32_RGN_ERROR, "ExcludeClipRect");
        W32_RECT hole = { 6, 6, 8, 8 };
        ok(!RectVisible(dc, &hole), "excluded band is not visible");
        /* IntersectClipRect narrows */
        SelectClipRgn(dc, 0);
        ok(IntersectClipRect(dc, 0, 0, 5, 5) == W32_SIMPLEREGION,
           "IntersectClipRect");
        W32_RECT far_away = { 20, 20, 25, 25 };
        ok(!RectVisible(dc, &far_away), "outside the intersect is hidden");
        SelectClipRgn(dc, 0);
        ok(RectVisible(dc, &far_away), "NULL clip region shows everything");
        /* empty region */
        W32_HRGN empty = CreateRectRgn(5, 5, 5, 5);
        ok(CombineRgn(r, empty, empty, W32_RGN_COPY) == W32_NULLREGION,
           "empty rect region is NULLREGION");
        ok(CreateRectRgn(9, 9, 3, 3) != 0, "inverted rect still creates");
        DeleteDC(dc);
    }

    /* ---- 7. DIBs: formats, orientations, round trips ------------------- */
    {
        W32_HDC dc = CreateCompatibleDC(0);
        /* 24bpp bottom-up DIB section */
        W32_BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = 4; bi.bmiHeader.biHeight = 4;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
        void *bits = 0;
        W32_HBITMAP d24 = CreateDIBSection(dc, &bi, 0, &bits, 0, 0);
        ok(d24 && bits, "CreateDIBSection 24bpp");
        /* bottom-up: row 0 of the buffer is the BOTTOM row of the image */
        uint8_t *b24 = (uint8_t *)bits;
        memset(b24, 0xAA, 4 * 4 * 3);
        b24[0] = 1;                          /* bottom-left pixel blue */
        SelectObject(dc, d24);
        ok(GetPixel(dc, 0, 3) == W32_RGB(0xAA, 0xAA, 0x01),
           "24bpp bottom-up row order (got %06x)", GetPixel(dc, 0, 3));

        /* top-down 32bpp section + GetDIBits round trip */
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = 4;
        bi.bmiHeader.biHeight = -4; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        uint32_t *b32 = 0;
        W32_HBITMAP d32 = CreateDIBSection(dc, &bi, 0, (void **)&b32, 0, 0);
        ok(d32 && b32, "CreateDIBSection 32bpp top-down");
        SelectObject(dc, d32);
        SetPixel(dc, 1, 2, W32_RGB(0x11, 0x22, 0x33));
        /* top-down: y=2 is buffer row 2; DIB byte order B,G,R,X */
        ok(b32[2 * 4 + 1] == 0xFF112233u, "DIB section pixel layout");
        /* GetDIBits as 32bpp bottom-up: the pixel moves rows */
        uint32_t out[16];
        W32_BITMAPINFO q;
        memset(&q, 0, sizeof q);
        q.bmiHeader.biSize = 40; q.bmiHeader.biWidth = 4; q.bmiHeader.biHeight = 4;
        q.bmiHeader.biPlanes = 1; q.bmiHeader.biBitCount = 32;
        ok(GetDIBits(dc, d32, 0, 4, out, &q, 0) == 4, "GetDIBits bottom-up");
        ok(out[(4 - 1 - 2) * 4 + 1] == 0xFF112233u, "GetDIBits flips rows");
        /* SetDIBits writes the same pixel back through the other order */
        uint32_t src[16];
        memset(src, 0, sizeof src);
        src[1 * 4 + 1] = 0x00112233u;         /* bottom-up: row 1 = y 2;
                                               * BGRX: the X byte is ignored */
        ok(SetDIBits(dc, d32, 0, 4, src, &q, 0) == 4, "SetDIBits");
        ok(b32[2 * 4 + 1] == 0xFF112233u, "SetDIBits round trip");

        /* header-only GetDIBits reports geometry */
        memset(&q, 0, sizeof q);
        q.bmiHeader.biSize = 40;
        ok(GetDIBits(dc, d32, 0, 0, 0, &q, 0) == 4, "header-only GetDIBits");
        ok(q.bmiHeader.biWidth == 4 && q.bmiHeader.biHeight == 4 &&
           q.bmiHeader.biBitCount == 32, "geometry reported");

        /* CreateBitmap 1bpp + pattern brush from it.  1bpp rows are
         * DWORD-aligned: 8 px = 1 byte of bits + 3 pad, 8 rows = 32 bytes. */
        uint8_t mono[32];
        memset(mono, 0, sizeof mono);
        for (int r = 0; r < 8; r++) mono[r * 4] = 0xAA;
        W32_HBITMAP mb = CreateBitmap(8, 8, 1, 1, mono);
        ok(mb != 0, "CreateBitmap 1bpp");
        W32_HBRUSH pat = CreatePatternBrush(mb);
        ok(pat != 0, "CreatePatternBrush");
        W32_BITMAP bm;
        ok(GetObjectW(mb, sizeof bm, &bm) == (int32_t)sizeof bm,
           "GetObject bitmap");
        ok(bm.bmWidth == 8 && bm.bmHeight == 8 && bm.bmBitsPixel == 1,
           "BITMAP geometry");
        DeleteDC(dc);
    }

    /* ---- 8. palettes ---------------------------------------------------- */
    {
        /* LOGPALETTE's palPalEntry[1] is the Win32 flexible-array
         * convention: allocate the header plus (n-1) extra entries. */
        union {
            W32_LOGPALETTE lp;
            uint8_t raw[sizeof(W32_LOGPALETTE) + 3 * sizeof(W32_PALETTEENTRY)];
        } u;
        memset(&u, 0, sizeof u);
        u.lp.palVersion = 0x300;
        u.lp.palNumEntries = 4;
        W32_PALETTEENTRY *pe = u.lp.palPalEntry;
        pe[0].peRed = 255;                                   /* red */
        pe[1].peGreen = 255;                                 /* green */
        pe[2].peBlue = 255;                                  /* blue */
        pe[3].peRed = 0; pe[3].peGreen = 0; pe[3].peBlue = 0;/* black */
        W32_HPALETTE pal = CreatePalette(&u.lp);
        ok(pal != 0, "CreatePalette 4 entries");

        /* an 8bpp DIB section realizes into its colour table */
        W32_HDC dc = CreateCompatibleDC(0);
        W32_BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = 4;
        bi.bmiHeader.biHeight = -4; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 8;
        uint8_t *b8 = 0;
        W32_HBITMAP d8 = CreateDIBSection(dc, &bi, 0, (void **)&b8, 0, 0);
        ok(d8 && b8, "CreateDIBSection 8bpp");
        SelectObject(dc, d8);
        b8[0] = 2;                            /* top-left = palette index 2 */
        ok(SelectPalette(dc, pal, 0) != 0, "SelectPalette returns previous");
        W32_UINT n = RealizePalette(dc);
        ok(n == 4, "RealizePalette rewrites the 8bpp table (got %u)", n);
        /* index 2 is now blue: GetPixel reads the realized table */
        ok(GetPixel(dc, 0, 0) == W32_RGB(0, 0, 255),
           "8bpp pixel reads through the realized palette");
        /* SetPaletteEntries + unrealize + realize again */
        W32_PALETTEENTRY e = { 255, 255, 0, 0 };
        ok(SetPaletteEntries(pal, 2, 1, &e) == 1, "SetPaletteEntries");
        RealizePalette(dc);
        ok(GetPixel(dc, 0, 0) == W32_RGB(255, 255, 0),
           "re-realized palette changes the pixel");
        ok(UpdateColors(dc), "UpdateColors succeeds");
        ok(UnrealizeObject(pal), "UnrealizeObject palette");
        DeleteDC(dc);
    }

    /* ---- 9. icons: decode, cache, draw ---------------------------------- */
    {
        /* a 4x4 32bpp icon: directory + one image */
        static const uint8_t ico_dir[] = {
            0x00, 0x00, 0x01, 0x00, 0x01, 0x00,          /* ICONDIR */
            /* entry: 4x4, 32bpp, image 120 bytes at offset 22 */
            0x04, 0x04, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
            0x78, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00,
            /* image at entry offset 22: a 40-byte BITMAPINFOHEADER with
             * biHeight = 8 (XOR + AND halves), 32bpp, size 120 bytes */
            0x28, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            /* XOR rows, bottom-up, BGRA: 32bpp icons carry their
             * transparency in the alpha byte (the AND mask is ignored
             * for 32bpp, same as Windows), so the border pixels get
             * A=0 and the centre 2x2 gets A=255 */
            0x00, 0x00, 0xFF, 0x00,   0x00, 0x00, 0xFF, 0xFF,
            0x00, 0x00, 0xFF, 0xFF,   0x00, 0x00, 0xFF, 0x00,
            0x00, 0x00, 0xFF, 0x00,   0x00, 0x00, 0xFF, 0xFF,
            0x00, 0x00, 0xFF, 0xFF,   0x00, 0x00, 0xFF, 0x00,
            0x00, 0x00, 0xFF, 0x00,   0x00, 0x00, 0xFF, 0xFF,
            0x00, 0x00, 0xFF, 0xFF,   0x00, 0x00, 0xFF, 0x00,
            0x00, 0x00, 0xFF, 0x00,   0x00, 0x00, 0xFF, 0xFF,
            0x00, 0x00, 0xFF, 0xFF,   0x00, 0x00, 0xFF, 0x00,
            /* AND rows (1bpp), all opaque: present for format fidelity,
             * ignored for 32bpp like Windows ignores it */
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        W32_HICON icon = w32_gdi_icon_decode(ico_dir, sizeof ico_dir);
        ok(icon != 0, "icon directory decodes");
        /* decode is cached: same blob -> same handle */
        ok(w32_gdi_icon_decode(ico_dir, sizeof ico_dir) == icon,
           "icon decode is cached per blob");
        /* garbage refuses */
        static const uint8_t junk[64] = { 0 };
        ok(w32_gdi_icon_decode(junk, sizeof junk) == 0, "junk refuses");
        ok(GetLastError() == W32_ERROR_INVALID_PARAMETER,
           "junk names INVALID_PARAMETER");

        /* draw into a memory DC: centre 2x2 opaque red, corners skipped */
        W32_HDC dc = CreateCompatibleDC(0);
        W32_HBITMAP bmp = CreateCompatibleBitmap(dc, 8, 8);
        SelectObject(dc, bmp);
        PatBlt(dc, 0, 0, 8, 8, W32_BLACKNESS);      /* black base */
        ok(w32_gdi_draw_icon(dc, 2, 2, icon, 4, 4), "draw icon 4x4");
        ok(GetPixel(dc, 3, 3) == W32_RGB(255, 0, 0),
           "icon centre is opaque red (got %06x)", GetPixel(dc, 3, 3));
        ok(GetPixel(dc, 2, 2) == 0, "transparent corner leaves the base");
        /* scaled draw to 8x8: centre 4x4 red */
        ok(w32_gdi_draw_icon(dc, 0, 0, icon, 8, 8), "draw icon scaled 8x8");
        ok(GetPixel(dc, 3, 3) == W32_RGB(255, 0, 0), "scaled centre red");
        ok(GetPixel(dc, 0, 0) == 0, "scaled corner still transparent");
        ok(!w32_gdi_draw_icon(dc, 0, 0, 0, 0, 0), "null icon refuses");
        DeleteDC(dc);
    }

    /* ---- 10. printing refuses by name ----------------------------------- */
    {
        W32_HDC dc = CreateCompatibleDC(0);
        W32_DOCINFOW di;
        memset(&di, 0, sizeof di);
        ok(StartDocW(dc, &di) == W32_SP_ERROR, "StartDocW refuses");
        ok(GetLastError() == W32_ERROR_CALL_NOT_IMPLEMENTED,
           "StartDocW names the refusal");
        ok(StartPage(dc) == W32_SP_ERROR, "StartPage refuses");
        ok(EndPage(dc) == W32_SP_ERROR, "EndPage refuses");
        ok(EndDoc(dc) == W32_SP_ERROR, "EndDoc refuses");
        DeleteDC(dc);
    }

    /* ---- 11. window DC path: raster through the compositor -------------- */
    {
        /* the a5 gate's class dance, minimally: register, create, show */
        static uint16_t clsname[] = {'A','7','W','I','N',0};
        W32_WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hbrBackground = 0;
        wc.lpszClassName = clsname;
        ok(RegisterClassExW(&wc) != 0, "RegisterClassExW for the draw");
        W32_HWND w = CreateWindowExW(0, clsname, clsname, 0,
                                     10, 10, 40, 30, 0, 0, 0, 0);
        ok(w != 0, "CreateWindowExW");
        ShowWindow(w, 5 /*SW_SHOW*/);
        W32_HDC dc = GetDC(w);
        ok(dc != 0, "GetDC on a real window");
        /* a shape routes through win_raster_blit -> one ag_blit_alpha */
        int before = blit_alpha_calls;
        W32_HBRUSH red = CreateSolidBrush(W32_RGB(255, 0, 0));
        W32_RECT rr = { 2, 2, 20, 20 };
        ok(FillRect(dc, &rr, red) == 1, "FillRect on a window DC");
        ok(blit_alpha_calls == before + 1,
           "the fill reached the compositor (%d blits)",
           blit_alpha_calls - before);
        ok(GetPixel(dc, 5, 5) == W32_RGB(255, 0, 0),
           "GetPixel reads the window back (got %06x)", GetPixel(dc, 5, 5));
        /* BitBlt from a memory DC onto the window DC: the same absolute-
         * coordinate blit path FillRect used, now with a real source */
        {
            W32_HDC mem = CreateCompatibleDC(dc);
            ok(mem != 0, "memory DC for the window blit");
            W32_HDC bmp = CreateCompatibleBitmap(dc, 4, 4);
            ok(bmp != 0, "bitmap for the window blit");
            ok(SelectObject(mem, bmp) != 0, "select the bitmap");
            W32_HBRUSH blue = CreateSolidBrush(W32_RGB(0, 0, 255));
            W32_RECT r2 = { 0, 0, 4, 4 };
            ok(FillRect(mem, &r2, blue) == 1, "fill the source blue");
            blit_alpha_calls = 0;
            ok(BitBlt(dc, 10, 4, 4, 4, mem, 0, 0, W32_SRCCOPY) != 0,
               "BitBlt SRCCOPY onto the window DC");
            ok(blit_alpha_calls == 1,
               "window BitBlt is exactly one compositor blit (got %d)",
               blit_alpha_calls);
            ok(GetPixel(dc, 11, 5) == W32_RGB(0, 0, 255),
               "window BitBlt pixel readback (got %06x)", GetPixel(dc, 11, 5));
            ok(DeleteObject(bmp) != 0, "delete the source bitmap");
            ok(DeleteDC(mem) != 0, "delete the source DC");
            DeleteObject(blue);
        }
        ReleaseDC(w, dc);
        DestroyWindow(w);
    }

    printf("== A7: %d checks, %d failures ==\n", checks, fails);
    return fails ? 1 : 0;
}
