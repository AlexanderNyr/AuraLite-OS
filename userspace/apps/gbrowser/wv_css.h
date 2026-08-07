/*
 * wv_css.h — inline CSS for the AuraLite web view (WEBVIEW_PLAN W5).
 *
 * The D4 subset, deliberately named: display (block/inline/none), color,
 * background-color, width, height, margin, padding, border, font-weight,
 * text-align.  Adding to that list is a decision, not a slope.
 *
 * Rules come from <style> blocks (tag / #id / .class selectors, comma
 * lists allowed, no combinators) and from style="..." attributes.
 * Cascade: later rules win, inline wins — no specificity computation
 * (per the plan: "no specificity cascade beyond 'later wins, inline
 * wins'").
 *
 * Error handling follows CSS: unknown properties are IGNORED, unknown
 * selectors are SKIPPED, and a malformed declaration never discards the
 * rest of its block — the one place in this plan where silently ignoring
 * input is correct, because the CSS error-handling rules require it.
 *
 * Colours: #rgb, #rrggbb and the 16 CSS named colours.  Lengths: integer
 * px (a bare number means px).  margin/padding accept 1, 2 or 4 values
 * (tr/bl/br ordering); border is <width>px [solid] [colour] (only the
 * width is honoured today; the colour and style keywords are parsed and
 * ignored as CSS requires).
 *
 * The resolver walks the DOM for style="..." attributes and the class/id
 * attributes for selector matching, so it needs the DOM, not the tokens.
 */

#ifndef AURALITE_WV_CSS_H
#define AURALITE_WV_CSS_H

#include <stddef.h>
#include <stdint.h>
#include "wv_dom.h"

/* ---- computed style (UA defaults are the base; CSS overrides) ---- */

#define WV_STYLE_UNSET (-1)

typedef struct {
    /* display: -1 = auto (UA behaviour), 0 = block, 1 = inline, 2 = none */
    int  display;
    /* colours: 0xFF000000 marker means "unset" (real black is 0x00000000
     * and real colours never have the alpha byte set in this pipeline) */
    uint32_t color;
    uint32_t bg;
    int32_t  width;     /* WV_STYLE_UNSET = unset */
    int32_t  height;
    int32_t  margin[4]; /* l, t, r, b — WV_STYLE_UNSET per side */
    int32_t  padding[4];
    int32_t  border;    /* border width px, 0 = none */
    int      bold;      /* font-weight: bold -> 1 */
    int      align;     /* text-align: 0 left, 1 center, 2 right */
} wv_style_t;

void wv_style_default(wv_style_t *s);

/* ---- parsed stylesheet ---- */

#define WV_CSS_MAX_RULES     256
#define WV_CSS_MAX_DECLS     1024
#define WV_CSS_MAX_SEL       127
#define WV_CSS_MAX_PROP      63
#define WV_CSS_MAX_VALUE     255
#define WV_CSS_POOL          32768

typedef struct {
    uint32_t sel_off;   /* selector text in the pool */
    uint32_t sel_len;
    uint32_t decl_base; /* first declaration index */
    uint32_t decl_count;
} wv_css_rule_t;

typedef struct {
    uint32_t prop_off;
    uint32_t prop_len;
    uint32_t value_off;
    uint32_t value_len;
} wv_css_decl_t;

typedef struct {
    wv_css_rule_t *rules;   size_t rule_cap;  size_t rule_count;
    wv_css_decl_t *decls;   size_t decl_cap;  size_t decl_count;
    char          *pool;    size_t pool_cap;  size_t pool_used;
    int            truncated;
} wv_css_t;

void wv_css_init(wv_css_t *css,
                 wv_css_rule_t *rules, size_t rule_cap,
                 wv_css_decl_t *decls, size_t decl_cap,
                 char *pool, size_t pool_cap);

/* Collect every <style> element's text from the DOM and parse it.  Also
 * parses the document's style="" attributes lazily at resolve time, so
 * this only needs the stylesheet text. */
int wv_css_build(wv_css_t *css, const wv_dom_t *d);

/* Resolve the computed style of a DOM element node: UA defaults, then
 * matching rules in order (later wins), then the style="" attribute
 * (inline wins).  Returns 0 on success, -1 on bad arguments. */
int wv_css_resolve(const wv_css_t *css, const wv_dom_t *d, uint32_t node,
                   wv_style_t *out);

/* Parse a raw style attribute / declaration block into the css arena and
 * return its declaration range (decl_base, decl_count).  Used both by
 * resolve (inline styles) and by tests. */
int wv_css_parse_decls(wv_css_t *css, const char *text, size_t len,
                       uint32_t *decl_base, uint32_t *decl_count);

/* Colour parsing: #rgb, #rrggbb, 16 named.  Returns 1 and sets *out, or 0
 * when the token is not a colour (then the declaration is ignored). */
int wv_css_parse_color(const char *s, size_t len, uint32_t *out);

const char *wv_css_str(const wv_css_t *css, uint32_t off);

#endif /* AURALITE_WV_CSS_H */
