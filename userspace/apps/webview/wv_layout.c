/*
 * wv_layout.c — block layout for the AuraLite web view (WEBVIEW_PLAN W3).
 *
 * Iterative DOM → display list.  The tree walk is a (node, phase) stack;
 * block and inline contexts are explicit stacks.  No recursion, no heap:
 * every working structure is a caller-provided fixed array, so the 64 KiB
 * user stack and the memory budget are both safe by construction.
 *
 * UA stylesheet: the browser defaults that make bare HTML readable, until
 * W5's CSS can override them:
 *   body { margin: 8px }        p { margin: 16px 0 }
 *   h1..h6 { margin: 16px 0 8px; font-weight: bold }
 *   ul, ol { margin: 16px 0; padding-left: 32px }
 *   blockquote { margin: 16px 32px }
 *   pre { margin: 16px 0; preformatted }
 *   hr { margin: 8px 0; height: 2px; bg: dark grey }
 *   a { color: #00E; text-decoration: underline }
 *   b, strong { font-weight: bold }
 *   u { text-decoration: underline }
 *   head, title, style, script, meta, link, base, noscript { display: none }
 *
 * Whitespace handling follows HTML: runs of space/tab/newline collapse to
 * one space; leading whitespace on a line is dropped; <pre> preserves
 * everything except a single leading newline (per WHATWG).
 */

#include "wv_layout.h"
#include <string.h>

/* ---- name helpers ---- */

static int wv_name_is(const char *s, uint32_t len, const char *lit) {
    size_t i = 0;
    while (lit[i]) {
        if (i >= len || s[i] != lit[i]) return 0;
        i++;
    }
    return i == len;
}

static int wv_hidden(const char *s, uint32_t len) {
    static const char *const h[] = {
        "head", "title", "style", "script", "meta", "link", "base", "noscript"
    };
    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); i++)
        if (wv_name_is(s, len, h[i])) return 1;
    return 0;
}

static int wv_inline_tag(const char *s, uint32_t len) {
    static const char *const inl[] = {
        "a", "b", "i", "em", "strong", "span", "code", "u", "small",
        "big", "sub", "sup", "mark", "q", "abbr", "cite", "kbd", "samp",
        "var", "time", "label", "font", "br", "img"
    };
    for (size_t i = 0; i < sizeof(inl) / sizeof(inl[0]); i++)
        if (wv_name_is(s, len, inl[i])) return 1;
    return 0;
}

static int wv_void_inline(const char *s, uint32_t len) {
    return wv_name_is(s, len, "br") || wv_name_is(s, len, "img");
}

/* ---- UA stylesheet ---- */

typedef struct { int32_t l, t, r, b; } ua_box;

static void ua_margin(const char *s, uint32_t len, ua_box *m) {
    m->l = m->t = m->r = m->b = 0;
    if (wv_name_is(s, len, "body")) { m->l = m->t = m->r = m->b = 8; return; }
    if (wv_name_is(s, len, "p") ||
        wv_name_is(s, len, "ul") || wv_name_is(s, len, "ol") ||
        wv_name_is(s, len, "blockquote") || wv_name_is(s, len, "pre")) {
        m->t = 16; m->b = 16;
        if (wv_name_is(s, len, "blockquote")) { m->l = 32; m->r = 32; }
        return;
    }
    if (len == 2 && s[0] == 'h' && s[1] >= '1' && s[1] <= '6') {
        m->t = 16; m->b = 8;
        return;
    }
    if (wv_name_is(s, len, "hr")) { m->t = 8; m->b = 8; return; }
}

static void ua_padding(const char *s, uint32_t len, ua_box *p) {
    p->l = p->t = p->r = p->b = 0;
    if (wv_name_is(s, len, "ul") || wv_name_is(s, len, "ol")) {
        p->l = 32;
        return;
    }
}

static uint32_t ua_bg(const char *s, uint32_t len) {
    if (wv_name_is(s, len, "body")) return 0x00FFFFFFu;   /* page paper */
    if (wv_name_is(s, len, "hr"))   return 0x00404048u;   /* AG_DARK */
    return 0;
}

static int ua_bold(const char *s, uint32_t len) {
    if (wv_name_is(s, len, "b") || wv_name_is(s, len, "strong")) return 1;
    if (len == 2 && s[0] == 'h' && s[1] >= '1' && s[1] <= '6') return 1;
    return 0;
}

static int ua_underline(const char *s, uint32_t len) {
    return wv_name_is(s, len, "a") || wv_name_is(s, len, "u");
}

static uint32_t ua_color(const char *s, uint32_t len) {
    if (wv_name_is(s, len, "a")) return WV_BLUE_LINK;
    return 0xFF000000u;   /* default black; 0xFF000000 = "unset" marker */
}

/* ---- arena helpers ---- */

void wv_layout_init(wv_layout_t *L,
                    wv_disp_t *items, size_t item_cap,
                    char *pool, size_t pool_cap,
                    wv_blk_t *blks, size_t blk_cap,
                    wv_inl_t *inls, size_t inl_cap,
                    wv_walk_t *walk, size_t walk_cap) {
    L->items = items; L->item_cap = item_cap; L->item_count = 0;
    L->pool = pool; L->pool_cap = pool_cap; L->pool_used = 0;
    L->blks = blks; L->blk_cap = blk_cap;
    L->inls = inls; L->inl_cap = inl_cap;
    L->walk = walk; L->walk_cap = walk_cap;
    L->truncated = 0;
    L->viewport_w = 0;
    L->content_h = 0;
    if (L->pool && L->pool_cap > 0) {
        L->pool[0] = '\0';
        L->pool_used = 1;
    }
}

const char *wv_layout_str(const wv_layout_t *L, uint32_t off) {
    if (!L || !L->pool) return "";
    if ((size_t)off >= L->pool_used) return "";
    return &L->pool[off];
}

static uint32_t wv_layout_pool_put(wv_layout_t *L, const char *buf, size_t len) {
    if (len + 1 > L->pool_cap - L->pool_used) {
        L->truncated = 1;
        return 0;
    }
    uint32_t off = (uint32_t)L->pool_used;
    memcpy(L->pool + L->pool_used, buf, len);
    L->pool[L->pool_used + len] = '\0';
    L->pool_used += len + 1;
    return off;
}

static int wv_layout_add_item(wv_layout_t *L, const wv_disp_t *it) {
    if (L->item_count >= L->item_cap) { L->truncated = 1; return -1; }
    L->items[L->item_count++] = *it;
    return (int)L->item_count - 1;
}

static wv_disp_t *wv_layout_item(wv_layout_t *L, size_t idx) {
    if (idx >= L->item_count) return NULL;
    return &L->items[idx];
}

/* ---- working stacks (counts live in wv_layout_t) ---- */

/* Read a DOM attribute value as a positive integer; -1 when absent. */
static int wv_attr_int(const wv_dom_t *d, uint32_t node, const char *name) {
    const wv_dom_node_t *nd = &d->nodes[node];
    for (uint32_t i = 0; i < nd->attr_count; i++) {
        size_t idx = (size_t)nd->attr_base + i;
        if (idx >= d->attr_count) break;
        const wv_attr_t *at = &d->attrs[idx];
        const char *an = wv_dom_str(d, at->name_off);
        if (strcmp(an, name) != 0) continue;
        const char *v = wv_dom_str(d, at->value_off);
        int val = 0;
        for (size_t k = 0; k < at->value_len; k++) {
            if (v[k] < '0' || v[k] > '9') return -1;
            val = val * 10 + (v[k] - '0');
            if (val > 100000) return -1;
        }
        return val;
    }
    return -1;
}

/* ---- word layout ---- */

static void layout_word(wv_layout_t *L, wv_blk_t *b, const wv_inl_t *st,
                        const char *word, uint32_t wlen) {
    int32_t w = (int32_t)wlen * WV_GLYPH_W;
    int32_t right = b->content_x + b->content_w;

    /* A pending word-space is only kept when the word fits after it. */
    if (b->inl_has_text) {
        if (b->inl_x + WV_GLYPH_W + w > right) {
            b->inl_y += WV_LINE_H;              /* wrap */
            b->inl_x = b->content_x;
            b->inl_has_text = 0;
        } else {
            b->inl_x += WV_GLYPH_W;             /* the space between words */
        }
    }

    uint32_t off = wv_layout_pool_put(L, word, wlen);
    wv_disp_t it;
    memset(&it, 0, sizeof(it));
    it.type = WV_D_TEXT;
    it.x = b->inl_x;
    it.y = b->inl_y;
    it.w = (uint32_t)w;
    it.h = WV_LINE_H;
    it.fg = (st->color == 0xFF000000u) ? 0x00000000u : st->color;
    it.bold = (uint8_t)st->bold;
    it.underline = (uint8_t)st->underline;
    it.text_off = off;
    it.text_len = wlen;
    wv_layout_add_item(L, &it);

    b->inl_x += w;
    b->inl_has_text = 1;
}

static void layout_br(wv_layout_t *L, wv_blk_t *b) {
    (void)L;
    if (!b->inl_has_text) return;    /* leading breaks collapse */
    b->inl_y += WV_LINE_H;
    b->inl_x = b->content_x;
    b->inl_has_text = 0;
}

static void layout_text_node(wv_layout_t *L, const wv_dom_t *d, uint32_t node,
                             const wv_inl_t *st) {
    const wv_dom_node_t *nd = &d->nodes[node];
    if (nd->text_len == 0) return;
    const char *s = wv_dom_str(d, nd->text_off);
    uint32_t len = nd->text_len;
    wv_blk_t *b = &L->blks[L->blk_used - 1];

    if (b->preformatted) {
        /* split on '\n' only; a single leading newline is dropped */
        uint32_t i = 0;
        if (len > 0 && s[0] == '\n') i = 1;
        uint32_t start = i;
        for (; i <= len; i++) {
            if (i == len || s[i] == '\n') {
                if (i > start)
                    layout_word(L, b, st, s + start, i - start);
                if (i < len) layout_br(L, b);
                start = i + 1;
            }
        }
        return;
    }

    /* normal: collapse whitespace, wrap at word boundaries */
    char word[WV_MAX_WORD + 1];
    uint32_t wl = 0;
    uint32_t i = 0;
    while (i <= len) {
        unsigned char c = (i < len) ? (unsigned char)s[i] : 0;
        int space = (i == len) || c == ' ' || c == '\t' || c == '\n' ||
                    c == '\r' || c == '\f';
        if (!space) {
            if (wl >= WV_MAX_WORD) { L->truncated = 1; }
            else word[wl++] = (char)c;
            i++;
            continue;
        }
        /* whitespace (or end): flush the accumulated word */
        if (wl > 0) {
            layout_word(L, b, st, word, wl);
            wl = 0;
        }
        /* skip the whole whitespace run */
        while (i < len) {
            unsigned char c2 = (unsigned char)s[i];
            if (c2 != ' ' && c2 != '\t' && c2 != '\n' &&
                c2 != '\r' && c2 != '\f') break;
            i++;
        }
        if (i == len) break;   /* trailing whitespace: nothing to do */
    }
}

/* ---- block open/close ---- */

static void open_block(wv_layout_t *L, const wv_dom_t *d, uint32_t node) {
    const wv_dom_node_t *nd = &d->nodes[node];
    const char *name = wv_dom_str(d, nd->name_off);
    uint32_t nlen = nd->name_len;

    ua_box m, p;
    ua_margin(name, nlen, &m);
    ua_padding(name, nlen, &p);

    wv_blk_t *parent = &L->blks[L->blk_used - 1];
    int32_t mw = parent->content_w - m.l - m.r;
    if (mw < 0) mw = 0;

    /* <canvas> sizes itself from its attributes (W7 will back it with an
     * FBO); the UA default is 300x150. */
    uint32_t min_h = 0;
    if (wv_name_is(name, nlen, "canvas")) {
        int cw = wv_attr_int(d, node, "width");
        int chh = wv_attr_int(d, node, "height");
        if (cw > 0 && cw <= 4096) mw = cw;
        min_h = (uint32_t)(chh > 0 && chh <= 4096 ? chh : 150);
    } else if (wv_name_is(name, nlen, "hr")) {
        min_h = 2;
    }

    int32_t bx = parent->content_x + m.l;
    int32_t by = parent->cur_y + m.t;

    /* The box item is emitted now (backgrounds paint under the children)
     * with height 0; the close fills the real height in. */
    wv_disp_t it;
    memset(&it, 0, sizeof(it));
    it.type = WV_D_BOX;
    it.x = bx;
    it.y = by;
    it.w = (uint32_t)mw;
    it.h = 0;
    it.bg = ua_bg(name, nlen);
    int idx = wv_layout_add_item(L, &it);
    if (idx < 0) return;

    if (L->blk_used >= L->blk_cap) { L->truncated = 1; return; }
    wv_blk_t *b = &L->blks[L->blk_used++];
    b->node = node;
    b->content_x = bx + p.l;
    b->content_top = by + p.t;
    b->content_w = mw - p.l - p.r;
    if (b->content_w < 0) b->content_w = 0;
    b->cur_y = b->content_top;
    b->inl_x = b->content_x;
    b->inl_y = b->content_top;
    b->inl_has_text = 0;
    b->box_x = bx;
    b->box_y = by;
    b->box_w = (uint32_t)mw;
    b->box_h = 0;
    b->min_h = min_h;
    b->box_item = (uint32_t)idx;
    b->bg = ua_bg(name, nlen);
    b->m_l = m.l; b->m_t = m.t; b->m_r = m.r; b->m_b = m.b;
    b->p_l = p.l; b->p_t = p.t; b->p_r = p.r; b->p_b = p.b;
    b->preformatted = wv_name_is(name, nlen, "pre");
}

static void close_block(wv_layout_t *L) {
    if (L->blk_used < 2) return;    /* never close the document box */
    wv_blk_t *b = &L->blks[L->blk_used - 1];
    wv_blk_t *parent = &L->blks[L->blk_used - 2];

    /* finalise the inline flow, then the box height */
    int32_t ch = b->cur_y;
    if (b->inl_has_text && b->inl_y + WV_LINE_H > ch)
        ch = b->inl_y + WV_LINE_H;
    uint32_t bh = (uint32_t)((ch - b->content_top) + b->p_t + b->p_b);
    if (bh < b->min_h) bh = b->min_h;
    b->box_h = bh;
    wv_disp_t *it = wv_layout_item(L, b->box_item);
    if (it) it->h = b->box_h;

    /* advance the parent past this box (margin bottom included) */
    parent->cur_y = b->box_y + (int32_t)b->box_h + b->m_b;
    parent->inl_x = parent->content_x;
    parent->inl_y = parent->cur_y;
    parent->inl_has_text = 0;

    L->blk_used--;
}

/* ---- entry points ---- */

int wv_layout_run(wv_layout_t *L, const wv_dom_t *d, int32_t viewport_w) {
    if (!L || !d || !d->nodes || viewport_w < 1) return -1;
    wv_layout_init(L, L->items, L->item_cap, L->pool, L->pool_cap,
                   L->blks, L->blk_cap, L->inls, L->inl_cap,
                   L->walk, L->walk_cap);
    if (!L->items || !L->blks || !L->inls || !L->walk) return -1;
    if (L->blk_cap < 2 || L->walk_cap < 4) return -1;

    L->viewport_w = viewport_w;

    /* document box: the page itself */
    {
        wv_blk_t *root = &L->blks[0];
        root->content_x = 0;
        root->content_top = 0;
        root->content_w = viewport_w;
        root->cur_y = 0;
        root->inl_x = 0;
        root->inl_y = 0;
        root->inl_has_text = 0;
        root->box_x = 0;
        root->box_y = 0;
        root->box_w = (uint32_t)viewport_w;
        root->box_h = 0;
        root->box_item = 0;
        root->bg = 0;
        root->m_l = root->m_t = root->m_r = root->m_b = 0;
        root->p_l = root->p_t = root->p_r = root->p_b = 0;
        root->preformatted = 0;
        L->blk_used = 1;

        wv_disp_t it;
        memset(&it, 0, sizeof(it));
        it.type = WV_D_BOX;
        it.x = 0; it.y = 0;
        it.w = (uint32_t)viewport_w;
        it.h = 0;
        it.bg = 0;
        wv_layout_add_item(L, &it);
    }

    /* inline style stack: one default entry */
    {
        wv_inl_t *st = &L->inls[0];
        st->bold = 0;
        st->color = 0xFF000000u;
        st->underline = 0;
        L->inl_used = 1;
    }

    /* iterative pre/post-order walk */
    L->walk[0].node = 0;
    L->walk[0].phase = 0;
    L->walk_used = 1;

    while (L->walk_used > 0) {
        wv_walk_t *w = &L->walk[L->walk_used - 1];
        uint32_t node = w->node;
        if (node >= d->node_count) { L->walk_used--; continue; }
        const wv_dom_node_t *nd = &d->nodes[node];

        if (w->phase == 1) {
            /* exit */
            L->walk_used--;
            if (nd->type == WV_N_ELEMENT) {
                const char *name = wv_dom_str(d, nd->name_off);
                uint32_t nlen = nd->name_len;
                if (wv_hidden(name, nlen)) {
                    /* nothing was opened */
                } else if (wv_inline_tag(name, nlen) &&
                           !wv_void_inline(name, nlen)) {
                    if (L->inl_used > 1) L->inl_used--;
                } else if (!wv_inline_tag(name, nlen)) {
                    /* block: close it — but only if it was actually
                     * opened (guards against a full block stack) */
                    if (L->blk_used > 1 &&
                        L->blks[L->blk_used - 1].node == node)
                        close_block(L);
                }
            }
            /* push next sibling */
            uint32_t ns = nd->next_sibling;
            if (ns != WV_NULL && ns < d->node_count &&
                L->walk_used < L->walk_cap) {
                L->walk[L->walk_used].node = ns;
                L->walk[L->walk_used].phase = 0;
                L->walk_used++;
            }
            continue;
        }

        /* enter */
        w->phase = 1;

        if (nd->type == WV_N_DOCUMENT) {
            /* nothing to open; children walk below */
        } else if (nd->type == WV_N_TEXT) {
            if (L->blk_used >= 1) {
                wv_inl_t *st = &L->inls[L->inl_used - 1];
                layout_text_node(L, d, node, st);
            }
        } else { /* element */
            const char *name = wv_dom_str(d, nd->name_off);
            uint32_t nlen = nd->name_len;
            if (wv_hidden(name, nlen)) {
                /* skip children entirely */
                uint32_t ns = nd->next_sibling;
                if (ns != WV_NULL && ns < d->node_count &&
                    L->walk_used < L->walk_cap) {
                    /* replace this entry with the sibling (same slot) */
                    L->walk[L->walk_used - 1].node = ns;
                    L->walk[L->walk_used - 1].phase = 0;
                } else {
                    L->walk_used--;
                }
                continue;
            }
            if (wv_inline_tag(name, nlen)) {
                if (wv_void_inline(name, nlen)) {
                    /* <br> / <img>: act on the current line, no subtree */
                    if (wv_name_is(name, nlen, "br")) {
                        if (L->blk_used >= 1)
                            layout_br(L, &L->blks[L->blk_used - 1]);
                    } else { /* img: inline placeholder box */
                        if (L->blk_used >= 1) {
                            wv_blk_t *b = &L->blks[L->blk_used - 1];
                            wv_disp_t it;
                            memset(&it, 0, sizeof(it));
                            it.type = WV_D_BOX;
                            it.x = b->inl_x;
                            it.y = b->inl_y;
                            it.w = 16;
                            it.h = 16;
                            it.bg = 0x00E0E0E0u;
                            wv_layout_add_item(L, &it);
                            if (b->inl_has_text)
                                b->inl_x += WV_GLYPH_W;
                            b->inl_x += 16;
                            b->inl_has_text = 1;
                        }
                    }
                    /* skip children (void elements have none) */
                    uint32_t ns = nd->next_sibling;
                    if (ns != WV_NULL && ns < d->node_count &&
                        L->walk_used < L->walk_cap) {
                        L->walk[L->walk_used - 1].node = ns;
                        L->walk[L->walk_used - 1].phase = 0;
                    } else {
                        L->walk_used--;
                    }
                    continue;
                }
                /* styled inline element: push a modified style */
                if (L->inl_used < L->inl_cap) {
                    wv_inl_t *st = &L->inls[L->inl_used - 1];
                    wv_inl_t *nw = &L->inls[L->inl_used++];
                    *nw = *st;
                    if (ua_bold(name, nlen)) nw->bold = 1;
                    if (ua_underline(name, nlen)) nw->underline = 1;
                    if (ua_color(name, nlen) != 0xFF000000u)
                        nw->color = ua_color(name, nlen);
                } else {
                    L->truncated = 1;
                }
            } else {
                /* block element */
                if (L->blk_used >= L->blk_cap) {
                    L->truncated = 1;
                } else {
                    open_block(L, d, node);
                }
            }
        }

        /* push the first child */
        uint32_t fc = nd->first_child;
        if (fc != WV_NULL && fc < d->node_count &&
            L->walk_used < L->walk_cap) {
            L->walk[L->walk_used].node = fc;
            L->walk[L->walk_used].phase = 0;
            L->walk_used++;
        } else if (fc != WV_NULL && fc < d->node_count) {
            L->truncated = 1;
        }
    }

    /* close the document box */
    if (L->blk_used >= 1) {
        wv_blk_t *root = &L->blks[0];
        int32_t ch = root->cur_y;
        if (root->inl_has_text && root->inl_y + WV_LINE_H > ch)
            ch = root->inl_y + WV_LINE_H;
        root->box_h = (uint32_t)(ch > 0 ? ch : 0);
        wv_disp_t *it = wv_layout_item(L, root->box_item);
        if (it) it->h = root->box_h;
        L->content_h = (uint32_t)(ch > 0 ? ch : 0);
    }

    return (int)L->item_count;
}
