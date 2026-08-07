/*
 * wv_canvas.h — <canvas> rendering for the AuraLite web view
 * (WEBVIEW_PLAN phase W7).
 *
 * The ONLY place in the web view where OpenGL is used (plan D1): a
 * <canvas data-scene="cube"> box renders through libgl into an FBO, and
 * the result is composited into the page like any other box.  There is no
 * JavaScript (D5), so a page can only ask for one of the handful of built-
 * in scenes.
 *
 * The render is performed ONCE when the page loads (the scene is static —
 * nothing drives it per-frame), so GL is NOT on the paint critical path:
 * a page without a canvas costs exactly what it did in W4, and a page
 * with one pays the render cost once, then only a clipped blit per frame.
 *
 * The per-render cost is measured and recorded (the number the plan's §1
 * table was missing: what a GL canvas costs inside a page).
 */

#ifndef AURALITE_WV_CANVAS_H
#define AURALITE_WV_CANVAS_H

#include <stdint.h>

/* Render the built-in "cube" scene into out (w*h XRGB8888 pixels,
 * top-left origin, no alpha).  wid is only used to create the GL
 * context.  Returns 0 on success; on failure out is untouched and -1 is
 * returned.  render_us receives the measured render+readback time. */
int wv_canvas_render_cube(uint32_t *out, int w, int h, int wid,
                          long *render_us);

/* Composite a rendered canvas into a page buffer at (x, y) with the page
 * scrolled by scroll_y (canvas top-left moves to y - scroll_y).  Clips to
 * the page; boxes entirely off-screen cost a bounds check only. */
void wv_canvas_blit(uint32_t *page, int pw, int ph,
                    const uint32_t *cv, int cw, int ch,
                    int x, int y, int scroll_y);

#endif /* AURALITE_WV_CANVAS_H */
