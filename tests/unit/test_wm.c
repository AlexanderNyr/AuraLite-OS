/*
 * test_wm.c — unit tests for window manager logic and widget sizing.
 *
 * Tests: widget bounds, title bar math, close button positions,
 * taskbar layout, color packing.
 * 25+ test cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int passed=0, failed=0, tn=0;
#define RUN(f) do{int b=failed; f(); tn++; if(failed==b)passed++;}while(0)
#define CHECK(c) do{if(!(c)){printf("  FAIL L%d: %s\n",__LINE__,#c);failed++;}}while(0)
#define CHECK_EQ(a,e) do{if((long)(a)!=(long)(e)){printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e));failed++;}}while(0)

/* WM constants (matching wm.h) */
#define TITLE_BAR_H 20
#define BORDER_W 2
#define TASKBAR_H 24

/* Content area helpers */
static uint32_t content_x(uint32_t win_x) { return win_x + BORDER_W; }
static uint32_t content_y(uint32_t win_y) { return win_y + TITLE_BAR_H + BORDER_W; }
static uint32_t content_w(uint32_t win_w) { return win_w - 2*BORDER_W; }
static uint32_t content_h(uint32_t win_h) { return win_h - TITLE_BAR_H - 2*BORDER_W; }

/* Close button position */
static uint32_t close_x(uint32_t win_x, uint32_t win_w) { return win_x + win_w - 16; }
static uint32_t close_y(uint32_t win_y) { return win_y + 4; }

/* Text pixel position */
static uint32_t text_px(uint32_t win_x, uint32_t col) {
    return win_x + BORDER_W + col * 8;
}
static uint32_t text_py(uint32_t win_y, uint32_t row) {
    return win_y + TITLE_BAR_H + BORDER_W + row * 8;
}

/* Color packing (0xRRGGBB) */
static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static uint8_t unpack_r(uint32_t c) { return (c >> 16) & 0xFF; }
static uint8_t unpack_g(uint32_t c) { return (c >> 8) & 0xFF; }
static uint8_t unpack_b(uint32_t c) { return c & 0xFF; }

/* ---- Content area tests ---- */
void t_content_x(void){CHECK_EQ(content_x(100),102);}
void t_content_y(void){CHECK_EQ(content_y(50),72);}
void t_content_w(void){CHECK_EQ(content_w(300),296);}
void t_content_h(void){CHECK_EQ(content_h(200),176);}

/* ---- Close button ---- */
void t_close_x(void){CHECK_EQ(close_x(100,300),384);}
void t_close_y(void){CHECK_EQ(close_y(50),54);}
void t_close_in_bounds(void){
    uint32_t cx=close_x(100,300);
    uint32_t cy=close_y(50);
    CHECK(cx >= 100 && cx < 400);
    CHECK(cy >= 50 && cy < 70);
}

/* ---- Text positioning ---- */
void t_text_col0(void){CHECK_EQ(text_px(100,0),102);}
void t_text_col5(void){CHECK_EQ(text_px(100,5),142);}
void t_text_row0(void){CHECK_EQ(text_py(50,0),72);}
void t_text_row3(void){CHECK_EQ(text_py(50,3),96);}

/* ---- Color packing ---- */
void t_pack_red(void){CHECK_EQ(pack_rgb(255,0,0),0xFF0000);}
void t_pack_green(void){CHECK_EQ(pack_rgb(0,255,0),0x00FF00);}
void t_pack_blue(void){CHECK_EQ(pack_rgb(0,0,255),0x0000FF);}
void t_pack_white(void){CHECK_EQ(pack_rgb(255,255,255),0xFFFFFF);}
void t_pack_black(void){CHECK_EQ(pack_rgb(0,0,0),0x000000);}
void t_unpack_r(void){CHECK_EQ(unpack_r(0xFF0000),255);}
void t_unpack_g(void){CHECK_EQ(unpack_g(0x00FF00),255);}
void t_unpack_b(void){CHECK_EQ(unpack_b(0x0000FF),255);}

/* ---- Title bar hit test ---- */
void t_title_hit_yes(void){
    uint32_t wx=100, wy=50, ww=300;
    int32_t mx=150, my=55; /* relative to screen, inside title */
    CHECK(mx >= (int32_t)wx);
    CHECK(mx < (int32_t)(wx + ww));
    CHECK(my >= (int32_t)wy);
    CHECK(my < (int32_t)(wy + TITLE_BAR_H));
}
void t_title_hit_no_below(void){
    uint32_t wy=50;
    int32_t my=80;
    CHECK(my >= (int32_t)(wy + TITLE_BAR_H));
}

/* ---- Taskbar ---- */
void t_taskbar_below_windows(void){
    uint32_t screen_h=800;
    uint32_t tb_y = screen_h - TASKBAR_H;
    CHECK_EQ(tb_y, 776);
}
void t_clock_format(void){
    uint64_t ticks=12345;
    uint64_t secs = ticks / 100;
    uint64_t mins = secs / 60;
    CHECK_EQ(mins, 2);
    CHECK_EQ(secs % 60, 3);
}

/* ---- Drag clamping ---- */
void t_drag_clamp_left(void){
    int32_t nx = -10;
    if (nx < 0) nx = 0;
    CHECK_EQ(nx, 0);
}
void t_drag_clamp_right(void){
    uint32_t screen_w = 1280;
    uint32_t win_w = 300;
    int32_t nx = 1100;
    if (nx + (int32_t)win_w > (int32_t)screen_w) nx = screen_w - win_w;
    CHECK_EQ(nx, 980);
}

/* ---- Widget sizing ---- */
void t_button_center(void){
    uint32_t bw=80, tw=16; /* "OK" = 2 chars * 8px */
    uint32_t tx = (bw - tw) / 2;
    CHECK_EQ(tx, 32);
}
void t_progress_fill(void){
    uint32_t pw=280, progress=50;
    uint32_t fill = (pw * progress) / 100;
    CHECK_EQ(fill, 140);
}
void t_progress_full(void){
    uint32_t pw=280, progress=100;
    uint32_t fill = (pw * progress) / 100;
    CHECK_EQ(fill, 280);
}
void t_progress_zero(void){
    uint32_t pw=280, progress=0;
    uint32_t fill = (pw * progress) / 100;
    CHECK_EQ(fill, 0);
}

/* ---- CW-1: compositor child embedding & clipping geometry ----
 * Mirrors kernel/gui/gui.c: a child stores parent-RELATIVE coords; its
 * absolute origin is the parent's content origin plus the child's x/y, and it
 * is drawn clipped to the intersection of its own rect with the parent's
 * content rectangle. */
typedef struct { int32_t x, y; int32_t w, h; } rect_t;

/* Parent content origin (decorated top-level): x+border, y+titlebar+border. */
static int32_t parent_content_x(int32_t px) { return px + BORDER_W; }
static int32_t parent_content_y(int32_t py) { return py + TITLE_BAR_H + BORDER_W; }

/* Child absolute origin from parent-relative coords. */
static int32_t child_abs_x(int32_t px, int32_t rel_x) { return parent_content_x(px) + rel_x; }
static int32_t child_abs_y(int32_t py, int32_t rel_y) { return parent_content_y(py) + rel_y; }

/* Intersect two rects; empty (w<=0||h<=0) means fully clipped. */
static rect_t rect_isect(rect_t a, rect_t b) {
    int32_t l = a.x > b.x ? a.x : b.x;
    int32_t t = a.y > b.y ? a.y : b.y;
    int32_t r = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t d = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_t out = { l, t, (r > l) ? r - l : 0, (d > t) ? d - t : 0 };
    return out;
}

static void t_child_abs_origin(void) {
    /* parent at (100,50); child at parent-relative (10,4). */
    CHECK_EQ(child_abs_x(100, 10), 100 + BORDER_W + 10);
    CHECK_EQ(child_abs_y(50, 4),   50 + TITLE_BAR_H + BORDER_W + 4);
}
static void t_child_moves_with_parent(void) {
    /* Same parent-relative coords, parent shifted by +40,+30: child abs shifts
     * by exactly the same delta (free propagation from relative storage). */
    int32_t x0 = child_abs_x(100, 10), y0 = child_abs_y(50, 4);
    int32_t x1 = child_abs_x(140, 10), y1 = child_abs_y(80, 4);
    CHECK_EQ(x1 - x0, 40);
    CHECK_EQ(y1 - y0, 30);
}
static void t_child_clip_inside(void) {
    /* A child fully inside the parent content clips to its own rect. */
    rect_t pc = { parent_content_x(100), parent_content_y(50),
                  content_w(300), content_h(200) };
    rect_t ch = { child_abs_x(100, 5), child_abs_y(50, 5), 80, 40 };
    rect_t cl = rect_isect(ch, pc);
    CHECK_EQ(cl.x, ch.x); CHECK_EQ(cl.y, ch.y);
    CHECK_EQ(cl.w, 80);   CHECK_EQ(cl.h, 40);
}
static void t_child_clip_overflow(void) {
    /* A child wider than the parent content is clipped to the content edge. */
    rect_t pc = { parent_content_x(100), parent_content_y(50),
                  content_w(300), content_h(200) };
    rect_t ch = { child_abs_x(100, 0), child_abs_y(50, 0), 1000, 1000 };
    rect_t cl = rect_isect(ch, pc);
    CHECK_EQ(cl.x, pc.x); CHECK_EQ(cl.y, pc.y);
    CHECK_EQ(cl.w, (int32_t)content_w(300));
    CHECK_EQ(cl.h, (int32_t)content_h(200));
}
static void t_child_clip_offscreen(void) {
    /* A child positioned entirely past the parent content is fully clipped. */
    rect_t pc = { parent_content_x(100), parent_content_y(50),
                  content_w(300), content_h(200) };
    rect_t ch = { child_abs_x(100, 10000), child_abs_y(50, 10000), 50, 50 };
    rect_t cl = rect_isect(ch, pc);
    CHECK(cl.w == 0 || cl.h == 0);
}
static void t_grandchild_nested_clip(void) {
    /* Grandchild clip = its rect ∩ child content ∩ parent content: never wider
     * than the innermost ancestor. */
    rect_t pc = { parent_content_x(100), parent_content_y(50), 100, 80 };
    rect_t ch = { pc.x + 10, pc.y + 10, 40, 30 };           /* child abs rect */
    rect_t child_clip = rect_isect(ch, pc);
    rect_t gc = { ch.x + 5, ch.y + 5, 1000, 1000 };         /* huge grandchild */
    rect_t g_clip = rect_isect(gc, child_clip);
    CHECK(g_clip.w <= child_clip.w && g_clip.h <= child_clip.h);
    CHECK(g_clip.x >= child_clip.x && g_clip.y >= child_clip.y);
}

int main(void){
    printf("=== Window Manager Tests ===\n\n");
    printf("--- content area ---\n");
    RUN(t_content_x);RUN(t_content_y);RUN(t_content_w);RUN(t_content_h);

    printf("--- close button ---\n");
    RUN(t_close_x);RUN(t_close_y);RUN(t_close_in_bounds);

    printf("--- text positioning ---\n");
    RUN(t_text_col0);RUN(t_text_col5);RUN(t_text_row0);RUN(t_text_row3);

    printf("--- colors ---\n");
    RUN(t_pack_red);RUN(t_pack_green);RUN(t_pack_blue);
    RUN(t_pack_white);RUN(t_pack_black);
    RUN(t_unpack_r);RUN(t_unpack_g);RUN(t_unpack_b);

    printf("--- hit testing ---\n");
    RUN(t_title_hit_yes);RUN(t_title_hit_no_below);

    printf("--- taskbar ---\n");
    RUN(t_taskbar_below_windows);RUN(t_clock_format);

    printf("--- drag clamping ---\n");
    RUN(t_drag_clamp_left);RUN(t_drag_clamp_right);

    printf("--- widgets ---\n");
    RUN(t_button_center);RUN(t_progress_fill);
    RUN(t_progress_full);RUN(t_progress_zero);

    printf("--- CW-1: child embedding & clipping ---\n");
    RUN(t_child_abs_origin);RUN(t_child_moves_with_parent);
    RUN(t_child_clip_inside);RUN(t_child_clip_overflow);
    RUN(t_child_clip_offscreen);RUN(t_grandchild_nested_clip);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",passed,tn,failed);
    return failed?1:0;
}
