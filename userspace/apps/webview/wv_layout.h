/*
 * wv_layout.h — block layout for the AuraLite web view (WEBVIEW_PLAN W3).
 *
 * A DOM → a list of positioned boxes (a display list).  Nothing is
 * rasterised here: painting (W4) consumes this list, which is what makes
 * both sides testable — a layout failure says *what* moved, in text.
 *
 * Design constraints from the plan:
 *   - The user stack is 64 KiB.  The tree walk is ITERATIVE (an explicit
 *     walk stack of (node, phase) pairs inside the caller-provided
 *     arena), and block/inline contexts live in caller arrays with a
 *     depth cap — layout recursion, the classic way to hit the limit, is
 *     structurally impossible here.
 *   - Bounded memory: display items, the string pool and the working
 *     stacks are caller-provided fixed arrays; exceeding any of them sets
 *     `truncated` while the walk still completes.
 *
 * Box model (what W5's CSS will feed): every block box carries margin,
 * padding, width and background; children lay out inside the content box
 * (padding excluded).  Until W5 lands, the values come from a small UA
 * stylesheet (body margin, p margins, h1-h6 bold, ul/ol list indent,
 * blockquote margins, hr height, a blue+underline) — the browser defaults
 * that make bare HTML readable.
 *
 * Inline flow: text is split into words, collapsed like HTML whitespace
 * (leading whitespace on a line is dropped, runs collapse to one space),
 * and wrapped at the content edge.  <br> forces a line break; <pre>
 * preserves whitespace and newlines.  Inline elements push/pop a style
 * stack: <b>/<strong>/<h1-h6> bold, <a> blue + underline, <u> underline.
 * <img> is an inline placeholder box (16×16) — real images are a later
 * phase.  <canvas> becomes a block box sized by its width/height
 * attributes (W7 will back it with an FBO).
 *
 * Hidden elements (head, title, style, script, meta, link, base,
 * noscript) produce no boxes and their text is not laid out.
 */

#ifndef AURALITE_WV_LAYOUT_H
#define AURALITE_WV_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include "wv_dom.h"
#include "wv_css.h"

/* ---- display list ---- */

#define WV_MAX_ITEMS        8192
#define WV_GLYPH_W          8       /* PSF 8×16 monospace */
#define WV_LINE_H           16
#define WV_MAX_WORD         2047    /* longest single word laid out */
#define WV_BLUE_LINK        0x000000EEu

typedef enum {
    WV_D_BOX = 1,   /* filled rectangle (background); h may grow after
                     * children are laid out */
    WV_D_TEXT        /* one word at (x, y), drawn with the PSF glyphs */
} wv_disp_type;

typedef struct {
    uint8_t  type;      /* WV_D_BOX / WV_D_TEXT */
    uint8_t  bold;      /* synthesised bold (double-strike) */
    uint8_t  underline; /* underline (links, <u>) */
    uint8_t  _pad;
    int32_t  x, y;      /* page coordinates of the top-left corner */
    uint32_t w, h;
    uint32_t fg;        /* text colour (WV_D_TEXT) */
    uint32_t bg;        /* box fill, 0 = transparent (WV_D_BOX) */
    uint32_t text_off;  /* string in the layout pool */
    uint32_t text_len;
    uint16_t border;    /* border width px, 0 = none (WV_D_BOX) */
    uint16_t _pad2;
    uint32_t border_color;
    uint32_t link_off;  /* href in the DOM pool (0 = not a link) */
} wv_disp_t;

/* ---- working stacks (caller-provided, so no heap and no big locals) ---- */

typedef struct {
    uint32_t node;          /* DOM node this box was opened for */
    int32_t content_x;      /* left edge of the content box */
    int32_t content_top;    /* top edge of the content box */
    int32_t content_w;      /* content width (padding excluded) */
    int32_t cur_y;          /* next block child's y within content */
    int32_t inl_x, inl_y;   /* inline cursor */
    int     inl_has_text;   /* anything on the current line? */
    int32_t box_x, box_y;   /* margin box position */
    uint32_t box_w;         /* margin-box width */
    uint32_t box_h;         /* filled in when the block closes */
    uint32_t min_h;         /* forced minimum height (hr, canvas) */
    uint32_t box_item;      /* index of the box's WV_D_BOX item */
    uint32_t bg;            /* background colour (0 = transparent) */
    int32_t m_l, m_t, m_r, m_b;
    int32_t p_l, p_t, p_r, p_b;
    int     preformatted;
    int     align;          /* 0 left, 1 center, 2 right (W5 text-align) */
    size_t  inl_mark;       /* inline-style stack depth at block open */
    int32_t line_start;     /* first display item of the current line */
    int32_t line_w;         /* width occupied on the current line */
} wv_blk_t;

typedef struct {
    int      bold;
    uint32_t color;
    int      underline;
    uint32_t link_off;   /* href in the DOM pool (0 = none) */
} wv_inl_t;

typedef struct {
    uint32_t node;
    int      phase;         /* 0 = enter, 1 = exit */
} wv_walk_t;

/* ---- layout result ---- */

typedef struct {
    wv_disp_t *items;
    size_t     item_cap;
    size_t     item_count;
    char      *pool;
    size_t     pool_cap;
    size_t     pool_used;
    wv_blk_t  *blks;   size_t blk_cap;  size_t blk_used;
    wv_inl_t  *inls;   size_t inl_cap;  size_t inl_used;
    wv_walk_t *walk;   size_t walk_cap; size_t walk_used;
    const wv_css_t *css;   /* stylesheet, may be NULL */
    int        truncated;
    int32_t    viewport_w;
    uint32_t   content_h;   /* total laid-out height of the page */
} wv_layout_t;

void wv_layout_init(wv_layout_t *L,
                    wv_disp_t *items, size_t item_cap,
                    char *pool, size_t pool_cap,
                    wv_blk_t *blks, size_t blk_cap,
                    wv_inl_t *inls, size_t inl_cap,
                    wv_walk_t *walk, size_t walk_cap);

/* Lay out a DOM into the display list.  css may be NULL (UA styles only).
 * Returns item_count or -1 on NULL args / bad viewport.  `truncated` is
 * set when any cap was hit; the result is still usable. */
int wv_layout_run(wv_layout_t *L, const wv_dom_t *d, int32_t viewport_w,
                  const wv_css_t *css);

const char *wv_layout_str(const wv_layout_t *L, uint32_t off);

#endif /* AURALITE_WV_LAYOUT_H */
