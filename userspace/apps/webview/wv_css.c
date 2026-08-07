/*
 * wv_css.c — inline CSS for the AuraLite web view (WEBVIEW_PLAN W5).
 *
 * See wv_css.h.  Everything is bounded by the caller-provided arena; the
 * parser is iterative (no recursion), and malformed input is consumed to
 * the end of its block — a bad declaration never discards the rest.
 *
 * Property handling (the D4 list):
 *   display: none | block | inline
 *   color / background-color: #rgb | #rrggbb | 16 names
 *   width / height: <int>px
 *   margin / padding: 1, 2 or 4 <int>px values
 *   border: <int>px [solid|dashed|...] [<colour>]
 *   font-weight: bold | normal
 *   text-align: left | center | right
 * Unknown properties are ignored (CSS error recovery).
 */

#include <string.h>
#include "wv_css.h"

/* ---- helpers ---- */

static int wv_css_isspace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int wv_css_eq(const char *a, uint32_t alen, const char *lit) {
    size_t i = 0;
    while (lit[i]) {
        if (i >= alen || a[i] != lit[i]) return 0;
        i++;
    }
    return i == alen;
}

void wv_style_default(wv_style_t *s) {
    s->display = WV_STYLE_UNSET;
    s->color = 0xFF000000u;
    s->bg = 0xFF000000u;
    s->width = WV_STYLE_UNSET;
    s->height = WV_STYLE_UNSET;
    for (int i = 0; i < 4; i++) { s->margin[i] = WV_STYLE_UNSET; s->padding[i] = WV_STYLE_UNSET; }
    s->border = 0;
    s->bold = 0;
    s->align = 0;
}

void wv_css_init(wv_css_t *css,
                 wv_css_rule_t *rules, size_t rule_cap,
                 wv_css_decl_t *decls, size_t decl_cap,
                 char *pool, size_t pool_cap) {
    css->rules = rules; css->rule_cap = rule_cap; css->rule_count = 0;
    css->decls = decls; css->decl_cap = decl_cap; css->decl_count = 0;
    css->pool = pool; css->pool_cap = pool_cap; css->pool_used = 0;
    css->truncated = 0;
    if (css->pool && css->pool_cap > 0) {
        css->pool[0] = '\0';
        css->pool_used = 1;
    }
}

const char *wv_css_str(const wv_css_t *css, uint32_t off) {
    if (!css || !css->pool) return "";
    if ((size_t)off >= css->pool_used) return "";
    return &css->pool[off];
}

static uint32_t wv_css_pool_put(wv_css_t *css, const char *buf, size_t len) {
    if (len + 1 > css->pool_cap - css->pool_used) { css->truncated = 1; return 0; }
    uint32_t off = (uint32_t)css->pool_used;
    memcpy(css->pool + css->pool_used, buf, len);
    css->pool[css->pool_used + len] = '\0';
    css->pool_used += len + 1;
    return off;
}

/* ---- colour parsing ---- */

static const struct { const char *name; uint32_t rgb; } named_colours[] = {
    { "black", 0x000000 }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 },
    { "grey",  0x808080 }, { "white", 0xFFFFFF }, { "maroon", 0x800000 },
    { "red",   0xFF0000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "green", 0x008000 }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
    { "yellow", 0xFFFF00 }, { "navy", 0x000080 }, { "blue", 0x0000FF },
    { "teal", 0x008080 }, { "aqua", 0x00FFFF },
};

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wv_css_parse_color(const char *s, size_t len, uint32_t *out) {
    if (!s || !out) return 0;
    if (len == 0) return 0;
    if (s[0] == '#') {
        if (len == 4) {          /* #rgb — each nibble doubles: #f00 -> 0xff0000 */
            int r = hexval((unsigned char)s[1]);
            int g = hexval((unsigned char)s[2]);
            int b = hexval((unsigned char)s[3]);
            if (r < 0 || g < 0 || b < 0) return 0;
            *out = ((uint32_t)(r * 17) << 16) |
                   ((uint32_t)(g * 17) << 8) |
                   (uint32_t)(b * 17);
            return 1;
        }
        if (len == 7) {          /* #rrggbb */
            int r1 = hexval((unsigned char)s[1]), r2 = hexval((unsigned char)s[2]);
            int g1 = hexval((unsigned char)s[3]), g2 = hexval((unsigned char)s[4]);
            int b1 = hexval((unsigned char)s[5]), b2 = hexval((unsigned char)s[6]);
            if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return 0;
            *out = ((uint32_t)(r1 * 16 + r2) << 16) |
                   ((uint32_t)(g1 * 16 + g2) << 8) |
                   (uint32_t)(b1 * 16 + b2);
            return 1;
        }
        return 0;
    }
    for (size_t i = 0; i < sizeof(named_colours) / sizeof(named_colours[0]); i++) {
        if (wv_css_eq(s, (uint32_t)len, named_colours[i].name)) {
            *out = named_colours[i].rgb;
            return 1;
        }
    }
    return 0;
}

/* ---- length parsing: <int>px or <int> ---- */

static int parse_px(const char *s, uint32_t len, int32_t *out) {
    uint32_t i = 0;
    while (i < len && wv_css_isspace((unsigned char)s[i])) i++;
    if (i >= len || s[i] < '0' || s[i] > '9') return 0;
    int64_t v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 100000) return 0;
        i++;
    }
    while (i < len && wv_css_isspace((unsigned char)s[i])) i++;
    if (i < len) {
        if (s[i] == 'p' && i + 1 < len && s[i + 1] == 'x') i += 2;
        else return 0;
    }
    while (i < len && wv_css_isspace((unsigned char)s[i])) i++;
    if (i != len) return 0;
    *out = (int32_t)v;
    return 1;
}

/* split a value on whitespace into up to 4 tokens */
static int split_values(const char *v, uint32_t vlen, const char *tok[4],
                        uint32_t tokl[4], int max) {
    int n = 0;
    uint32_t i = 0;
    while (i < vlen && n < max) {
        while (i < vlen && wv_css_isspace((unsigned char)v[i])) i++;
        if (i >= vlen) break;
        uint32_t start = i;
        while (i < vlen && !wv_css_isspace((unsigned char)v[i])) i++;
        tok[n] = v + start;
        tokl[n] = i - start;
        n++;
    }
    return n;
}

/* ---- declaration parsing ---- */

static void store_decl(wv_css_t *css, const char *prop, uint32_t plen,
                       const char *val, uint32_t vlen) {
    if (css->decl_count >= css->decl_cap) { css->truncated = 1; return; }
    wv_css_decl_t *d = &css->decls[css->decl_count++];
    d->prop_off = wv_css_pool_put(css, prop, plen);
    d->prop_len = plen;
    d->value_off = wv_css_pool_put(css, val, vlen);
    d->value_len = vlen;
}

int wv_css_parse_decls(wv_css_t *css, const char *text, size_t len,
                       uint32_t *decl_base, uint32_t *decl_count) {
    if (!css || !text) return -1;
    uint32_t base = (uint32_t)css->decl_count;
    size_t i = 0;
    while (i < len) {
        /* skip whitespace and stray ';' */
        while (i < len && (wv_css_isspace((unsigned char)text[i]) || text[i] == ';'))
            i++;
        if (i >= len) break;
        /* property name (leading whitespace is skipped) */
        size_t pstart = i;
        while (i < len && text[i] != ':' && text[i] != ';')
            i++;
        size_t plen = i - pstart;
        while (plen > 0 && wv_css_isspace((unsigned char)text[pstart + plen - 1]))
            plen--;
        size_t p0 = 0;
        while (p0 < plen && wv_css_isspace((unsigned char)text[pstart + p0])) p0++;
        plen -= p0;
        pstart += p0;
        if (i >= len || text[i] != ':') {
            /* no colon: malformed declaration — skip to next ';' */
            while (i < len && text[i] != ';') i++;
            continue;
        }
        i++;   /* past ':' */
        /* value up to ';' or end (leading whitespace is skipped) */
        while (i < len && wv_css_isspace((unsigned char)text[i])) i++;
        size_t vstart = i;
        while (i < len && text[i] != ';') i++;
        size_t vlen = i - vstart;
        while (vlen > 0 && wv_css_isspace((unsigned char)text[vstart + vlen - 1]))
            vlen--;
        if (vlen > WV_CSS_MAX_VALUE) vlen = WV_CSS_MAX_VALUE;
        if (plen > 0 && vlen > 0)
            store_decl(css, text + pstart, (uint32_t)plen,
                       text + vstart, (uint32_t)vlen);
        if (i < len) i++;   /* past ';' */
    }
    if (decl_base) *decl_base = base;
    if (decl_count) *decl_count = css->decl_count - base;
    return 0;
}

/* ---- stylesheet: rules ---- */

/* Parse "selector { decls }" blocks from a <style> body. */
static void parse_stylesheet(wv_css_t *css, const char *text, size_t len) {
    (void)css;
    size_t i = 0;
    while (i < len) {
        while (i < len && (wv_css_isspace((unsigned char)text[i]) || text[i] == '}'))
            i++;
        if (i >= len) break;
        /* selector up to '{' or '}' */
        size_t sstart = i;
        while (i < len && text[i] != '{' && text[i] != '}')
            i++;
        size_t slen = i - sstart;
        while (slen > 0 && wv_css_isspace((unsigned char)text[sstart + slen - 1]))
            slen--;
        if (slen > WV_CSS_MAX_SEL) slen = WV_CSS_MAX_SEL;
        if (i >= len || text[i] != '{') {
            /* stray text without a block: skip to the next '{' */
            while (i < len && text[i] != '{') i++;
            continue;
        }
        i++;   /* past '{' */
        size_t dstart = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (text[i] == '{') depth++;
            else if (text[i] == '}') depth--;
            if (depth == 0) break;
            i++;
        }
        size_t dlen = i - dstart;
        if (slen > 0) {
            uint32_t decl_base = 0, decl_count = 0;
            wv_css_parse_decls(css, text + dstart, dlen, &decl_base, &decl_count);
            if (css->rule_count < css->rule_cap) {
                wv_css_rule_t *r = &css->rules[css->rule_count++];
                r->sel_off = wv_css_pool_put(css, text + sstart, slen);
                r->sel_len = (uint32_t)slen;
                r->decl_base = decl_base;
                r->decl_count = decl_count;
            } else {
                css->truncated = 1;
            }
        }
        if (i < len) i++;   /* past '}' */
    }
}

int wv_css_build(wv_css_t *css, const wv_dom_t *d) {
    if (!css || !d || !d->nodes) return -1;
    wv_css_init(css, css->rules, css->rule_cap, css->decls, css->decl_cap,
                css->pool, css->pool_cap);
    if (!css->rules) return -1;

    /* walk the DOM for <style> elements and parse each text child in
     * place — NO large stack buffers (the user stack is 64 KiB), so a
     * stylesheet is parsed per text node.  A rule whose braces straddle
     * two text nodes is not merged (documented simplification; real
     * <style> bodies are single text nodes). */
    for (size_t i = 0; i < d->node_count; i++) {
        const wv_dom_node_t *n = &d->nodes[i];
        if (n->type != WV_N_ELEMENT) continue;
        const char *name = wv_dom_str(d, n->name_off);
        if (!wv_css_eq(name, n->name_len, "style")) continue;
        uint32_t c = n->first_child;
        while (c != WV_NULL && c < d->node_count) {
            if (d->nodes[c].type == WV_N_TEXT && d->nodes[c].text_len > 0)
                parse_stylesheet(css, wv_dom_str(d, d->nodes[c].text_off),
                                 d->nodes[c].text_len);
            c = d->nodes[c].next_sibling;
        }
    }
    return 0;
}

/* ---- selector matching ---- */

/* Does an element match a (possibly comma-listed) selector? */
static int match_selector(const wv_dom_t *d,
                          uint32_t node, const char *sel, uint32_t sel_len) {
    const wv_dom_node_t *nd = &d->nodes[node];
    const char *name = wv_dom_str(d, nd->name_off);
    const char *id = 0; uint32_t id_len = 0;
    const char *cls = 0; uint32_t cls_len = 0;
    for (uint32_t a = 0; a < nd->attr_count; a++) {
        size_t idx = (size_t)nd->attr_base + a;
        if (idx >= d->attr_count) break;
        const wv_attr_t *at = &d->attrs[idx];
        const char *an = wv_dom_str(d, at->name_off);
        if (wv_css_eq(an, at->name_len, "id")) { id = wv_dom_str(d, at->value_off); id_len = at->value_len; }
        else if (wv_css_eq(an, at->name_len, "class")) { cls = wv_dom_str(d, at->value_off); cls_len = at->value_len; }
    }

    uint32_t i = 0;
    for (;;) {
        while (i < sel_len && wv_css_isspace((unsigned char)sel[i])) i++;
        if (i >= sel_len) break;                /* end of the list */
        uint32_t start = i;
        while (i < sel_len && sel[i] != ',') i++;
        uint32_t sl = i - start;
        while (sl > 0 && wv_css_isspace((unsigned char)sel[start + sl - 1])) sl--;
        uint32_t s0 = start;
        while (sl > 0 && wv_css_isspace((unsigned char)sel[s0])) { s0++; sl--; }
        int match = 0;
        if (sl > 0) {
            if (sel[s0] == '#') {
                match = id && sl - 1 == id_len &&
                        memcmp(sel + s0 + 1, id, id_len) == 0;
            } else if (sel[s0] == '.') {
                match = cls && sl - 1 == cls_len &&
                        memcmp(sel + s0 + 1, cls, cls_len) == 0;
            } else {
                /* tag, optionally combined with a class: "p.note" */
                uint32_t dot = 0;
                while (dot < sl && sel[s0 + dot] != '.') dot++;
                if (dot < sl) {
                    uint32_t tlen = dot;
                    uint32_t clen = sl - dot - 1;
                    match = tlen == nd->name_len &&
                            memcmp(sel + s0, name, tlen) == 0 &&
                            clen == cls_len &&
                            memcmp(sel + s0 + dot + 1, cls, clen) == 0;
                } else {
                    match = sl == nd->name_len &&
                            memcmp(sel + s0, name, nd->name_len) == 0;
                }
            }
        }
        if (match) return 1;
        if (i >= sel_len) break;                /* end of the list */
        i++;                                    /* past ',' */
    }
    return 0;
}

/* ---- applying declarations ---- */

static void apply_decl(wv_style_t *s, const char *prop, uint32_t plen,
                       const char *val, uint32_t vlen) {
    uint32_t color;
    int32_t px;
    if (wv_css_eq(prop, plen, "color") && wv_css_parse_color(val, vlen, &color))
        s->color = color;
    else if (wv_css_eq(prop, plen, "background-color") &&
             wv_css_parse_color(val, vlen, &color))
        s->bg = color;
    else if (wv_css_eq(prop, plen, "display")) {
        if (wv_css_eq(val, vlen, "none")) s->display = 2;
        else if (wv_css_eq(val, vlen, "block")) s->display = 0;
        else if (wv_css_eq(val, vlen, "inline")) s->display = 1;
    } else if (wv_css_eq(prop, plen, "width") && parse_px(val, vlen, &px))
        s->width = px;
    else if (wv_css_eq(prop, plen, "height") && parse_px(val, vlen, &px))
        s->height = px;
    else if (wv_css_eq(prop, plen, "font-weight")) {
        if (wv_css_eq(val, vlen, "bold")) s->bold = 1;
        else if (wv_css_eq(val, vlen, "normal")) s->bold = 0;
    } else if (wv_css_eq(prop, plen, "text-align")) {
        if (wv_css_eq(val, vlen, "center")) s->align = 1;
        else if (wv_css_eq(val, vlen, "right")) s->align = 2;
        else s->align = 0;
    } else if (wv_css_eq(prop, plen, "margin") ||
               wv_css_eq(prop, plen, "padding")) {
        const char *tok[4]; uint32_t tokl[4];
        int32_t vals[4];
        int n = split_values(val, vlen, tok, tokl, 4);
        int ok = 1;
        for (int i = 0; i < n; i++)
            if (!parse_px(tok[i], tokl[i], &vals[i])) { ok = 0; break; }
        if (!ok || n == 0) return;
        int l, t, r, b;
        if (n == 1) { l = t = r = b = vals[0]; }
        else if (n == 2) { l = r = vals[0]; t = b = vals[1]; }
        else if (n == 3) { t = vals[0]; l = r = vals[1]; b = vals[2]; }
        else { t = vals[0]; r = vals[1]; b = vals[2]; l = vals[3]; }
        int32_t *dst = wv_css_eq(prop, plen, "margin") ? s->margin : s->padding;
        dst[0] = l; dst[1] = t; dst[2] = r; dst[3] = b;
    } else if (wv_css_eq(prop, plen, "border")) {
        /* border: <w>px [style] [colour] — width honoured, rest parsed */
        const char *tok[4]; uint32_t tokl[4];
        int n = split_values(val, vlen, tok, tokl, 4);
        int32_t w = 0;
        int got_w = 0;
        for (int i = 0; i < n; i++) {
            if (parse_px(tok[i], tokl[i], &px)) { w = px; got_w = 1; break; }
        }
        if (got_w) s->border = w;
    }
}

static void apply_decls(wv_style_t *s, const wv_css_t *css,
                        uint32_t base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = base + i;
        if (idx >= css->decl_count) break;
        const wv_css_decl_t *d = &css->decls[idx];
        apply_decl(s, wv_css_str(css, d->prop_off), d->prop_len,
                   wv_css_str(css, d->value_off), d->value_len);
    }
}

int wv_css_resolve(const wv_css_t *css, const wv_dom_t *d, uint32_t node,
                   wv_style_t *out) {
    if (!css || !d || !out || node >= d->node_count) return -1;
    const wv_dom_node_t *nd = &d->nodes[node];
    if (nd->type != WV_N_ELEMENT) return -1;

    wv_style_default(out);

    /* rules in order: later wins */
    for (size_t i = 0; i < css->rule_count; i++) {
        const wv_css_rule_t *r = &css->rules[i];
        const char *sel = wv_css_str(css, r->sel_off);
        if (match_selector(d, node, sel, r->sel_len))
            apply_decls(out, css, r->decl_base, r->decl_count);
    }

    /* inline style="" attribute: inline wins */
    for (uint32_t a = 0; a < nd->attr_count; a++) {
        size_t idx = (size_t)nd->attr_base + a;
        if (idx >= d->attr_count) break;
        const wv_attr_t *at = &d->attrs[idx];
        const char *an = wv_dom_str(d, at->name_off);
        if (wv_css_eq(an, at->name_len, "style") && at->value_len > 0) {
            uint32_t base, count;
            wv_css_parse_decls((wv_css_t *)css,
                               wv_dom_str(d, at->value_off), at->value_len,
                               &base, &count);
            apply_decls(out, css, base, count);
        }
    }
    return 0;
}
