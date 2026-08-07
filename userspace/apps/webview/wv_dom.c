/*
 * wv_dom.c — DOM builder for the AuraLite web view (WEBVIEW_PLAN W2).
 *
 * Iterative HTML tree construction over the token stream, following the
 * WHATWG "tree construction" rules for the subset the plan needs:
 * implicit closes for <p>/<li>/<td>/<th>/<tr>, void elements, and the
 * pop-until-match reconciliation of mismatched close tags.  Everything is
 * index arithmetic over flat arrays; there is no recursion and no heap
 * allocation beyond the caller-provided arena.
 *
 * Deliberate simplifications (decisions, not accidents):
 *   - The "in body" insertion mode only; no tables/frameset/select
 *     special-casing beyond the td/th/tr closes above.
 *   - Comments and DOCTYPEs are dropped from the tree (they do not affect
 *     the layout phases).
 *   - <head>/<body> are not implied: an HTML document without them gets a
 *     flat tree of whatever elements exist.  The layout phases do not
 *     need the implied structure; the plan's implied-structure list is
 *     exactly the p/li/td/tr rules implemented here.
 *   - Attribute values are copied verbatim (the tokeniser already decoded
 *     character references).
 */

#include <string.h>
#include "wv_dom.h"

/* ---- pool helpers (same shape as the tokeniser's) ---- */

static uint32_t wv_dom_pool_put(wv_dom_t *d, const char *buf, size_t len) {
    if (len + 1 > d->pool_cap - d->pool_used) {
        d->truncated = 1;
        return 0;
    }
    uint32_t off = (uint32_t)d->pool_used;
    memcpy(d->pool + d->pool_used, buf, len);
    d->pool[d->pool_used + len] = '\0';
    d->pool_used += len + 1;
    return off;
}

/* Append bytes to an existing text node.  When the text is at the very end
 * of the pool it is extended in place; otherwise the combined string is
 * re-stored (correct in both cases, cheap in the common one). */
static void wv_dom_text_append(wv_dom_t *d, uint32_t node,
                               const char *bytes, size_t n) {
    wv_dom_node_t *nd = &d->nodes[node];
    size_t old_len = nd->text_len;
    if (nd->text_off + old_len + 1 == d->pool_used) {
        if (old_len + n + 1 > d->pool_cap - d->pool_used) {
            d->truncated = 1;
            return;
        }
        memcpy(d->pool + nd->text_off + old_len, bytes, n);
        d->pool[nd->text_off + old_len + n] = '\0';
        d->pool_used += n;
        nd->text_len = (uint32_t)(old_len + n);
        return;
    }
    if (old_len + n + 1 > d->pool_cap - d->pool_used) {
        d->truncated = 1;
        return;
    }
    size_t base = d->pool_used;
    memcpy(d->pool + base, d->pool + nd->text_off, old_len);
    memcpy(d->pool + base + old_len, bytes, n);
    d->pool[base + old_len + n] = '\0';
    d->pool_used += old_len + n + 1;
    nd->text_off = (uint32_t)base;
    nd->text_len = (uint32_t)(old_len + n);
}

static uint32_t wv_dom_new_node(wv_dom_t *d, int type,
                                uint32_t name_off, uint32_t name_len,
                                uint32_t text_off, uint32_t text_len,
                                uint32_t attr_base, uint32_t attr_count) {
    if (d->node_count >= d->node_cap) { d->truncated = 1; return WV_NULL; }
    wv_dom_node_t *n = &d->nodes[d->node_count];
    n->parent = WV_NULL;
    n->first_child = WV_NULL;
    n->last_child = WV_NULL;
    n->next_sibling = WV_NULL;
    n->type = (uint8_t)type;
    n->flags = 0;
    n->_pad = 0;
    n->name_off = name_off;
    n->name_len = name_len;
    n->text_off = text_off;
    n->text_len = text_len;
    n->attr_base = attr_base;
    n->attr_count = attr_count;
    return (uint32_t)d->node_count++;
}

static void wv_dom_append_child(wv_dom_t *d, uint32_t parent, uint32_t child) {
    wv_dom_node_t *p = &d->nodes[parent];
    if (p->last_child == WV_NULL) {
        p->first_child = child;
        p->last_child = child;
    } else {
        d->nodes[p->last_child].next_sibling = child;
        p->last_child = child;
    }
    d->nodes[child].parent = parent;
}

/* Copy one token attribute into the DOM attribute array + pool. */
static void wv_dom_push_attr(wv_dom_t *d, const wv_attr_t *src,
                             const char *name, const char *value) {
    if (d->attr_count >= d->attr_cap) { d->truncated = 1; return; }
    wv_attr_t *a = &d->attrs[d->attr_count++];
    a->name_off = wv_dom_pool_put(d, name, src->name_len);
    a->name_len = src->name_len;
    if (src->value_len) {
        a->value_off = wv_dom_pool_put(d, value, src->value_len);
        a->value_len = src->value_len;
    } else {
        a->value_off = 0;
        a->value_len = 0;
    }
}

/* name helpers */

static int wv_node_name_is(const wv_dom_t *d, uint32_t node, const char *lit) {
    const wv_dom_node_t *n = &d->nodes[node];
    if (n->type != WV_N_ELEMENT) return 0;
    const char *s = wv_dom_str(d, n->name_off);
    size_t i = 0;
    while (lit[i]) {
        if (i >= n->name_len) return 0;
        if (s[i] != lit[i]) return 0;
        i++;
    }
    return i == n->name_len;
}

/* Compare an open-element node's name against a token name (both
 * lower-case). */
static int wv_node_name_equals(const wv_dom_t *d, uint32_t node,
                               const char *name, uint32_t len) {
    const wv_dom_node_t *n = &d->nodes[node];
    if (n->type != WV_N_ELEMENT || n->name_len != len) return 0;
    const char *s = wv_dom_str(d, n->name_off);
    for (uint32_t i = 0; i < len; i++)
        if (s[i] != name[i]) return 0;
    return 1;
}

/* Pop the open-element stack while its top matches any of the (up to 3)
 * names.  The nodes themselves stay in the tree — this only changes which
 * element future siblings attach to. */
static void wv_dom_pop_until(wv_dom_t *d, const char *a,
                             const char *b, const char *c) {
    while (d->stack_len > 1) {
        uint32_t top = d->stack[d->stack_len - 1];
        if (!wv_node_name_is(d, top, a) &&
            !(b && wv_node_name_is(d, top, b)) &&
            !(c && wv_node_name_is(d, top, c)))
            break;
        d->stack_len--;
    }
}

static int wv_is_void(const char *name, uint32_t len) {
    static const char *const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(voids) / sizeof(voids[0]); i++) {
        const char *v = voids[i];
        size_t j = 0;
        while (v[j]) {
            if (j >= len || name[j] != v[j]) break;
            j++;
        }
        if (v[j] == '\0' && j == len) return 1;
    }
    return 0;
}

void wv_dom_init(wv_dom_t *d,
                 wv_dom_node_t *nodes, size_t node_cap,
                 wv_attr_t *attrs, size_t attr_cap,
                 char *pool, size_t pool_cap,
                 uint32_t *stack, size_t stack_cap) {
    d->nodes = nodes; d->node_cap = node_cap; d->node_count = 0;
    d->attrs = attrs; d->attr_cap = attr_cap; d->attr_count = 0;
    d->pool = pool; d->pool_cap = pool_cap; d->pool_used = 0;
    d->stack = stack; d->stack_cap = stack_cap; d->stack_len = 0;
    d->truncated = 0;
    if (d->pool && d->pool_cap > 0) {
        d->pool[0] = '\0';
        d->pool_used = 1;
    }
}

const char *wv_dom_str(const wv_dom_t *d, uint32_t off) {
    if (!d || !d->pool) return "";
    if ((size_t)off >= d->pool_used) return "";
    return &d->pool[off];
}

uint32_t wv_dom_depth(const wv_dom_t *d, uint32_t node) {
    uint32_t depth = 0;
    uint32_t cur = node;
    while (cur != WV_NULL && cur < d->node_count) {
        uint32_t p = d->nodes[cur].parent;
        if (p == WV_NULL || p >= d->node_count || p == cur) break;
        depth++;
        cur = p;
    }
    return depth;
}

int wv_dom_build(wv_dom_t *d, const wv_arena_t *toks, size_t depth_cap) {
    if (!d || !toks || !toks->toks) return -1;
    wv_dom_init(d, d->nodes, d->node_cap, d->attrs, d->attr_cap,
                d->pool, d->pool_cap, d->stack, d->stack_cap);
    if (!d->nodes || !d->stack || d->stack_cap < 1) return -1;

    size_t eff_cap = depth_cap ? depth_cap : WV_DOM_DEFAULT_DEPTH;
    if (eff_cap > d->stack_cap) eff_cap = d->stack_cap;
    if (eff_cap < 1) eff_cap = 1;

    /* implicit document root at index 0 */
    uint32_t root = wv_dom_new_node(d, WV_N_DOCUMENT, 0, 0, 0, 0, 0, 0);
    if (root == WV_NULL) return -1;
    d->stack[0] = root;
    d->stack_len = 1;

    for (size_t i = 0; i < toks->tok_count; i++) {
        const wv_token_t *tk = &toks->toks[i];
        uint32_t parent = d->stack[d->stack_len - 1];

        switch (tk->type) {
        case WV_T_TEXT:
        case WV_T_CDATA: {
            const char *s = wv_tok_str(toks, tk->text_off);
            uint32_t last = d->nodes[parent].last_child;
            if (last != WV_NULL && d->nodes[last].type == WV_N_TEXT) {
                wv_dom_text_append(d, last, s, tk->text_len);
            } else {
                uint32_t off = wv_dom_pool_put(d, s, tk->text_len);
                uint32_t n = wv_dom_new_node(d, WV_N_TEXT, 0, 0,
                                             off, tk->text_len, 0, 0);
                if (n != WV_NULL) wv_dom_append_child(d, parent, n);
            }
            break;
        }

        case WV_T_START: {
            const char *name = wv_tok_str(toks, tk->name_off);

            /* implicit closes (W2 requirement) */
            if (tk->name_len == 1 && name[0] == 'p') {
                wv_dom_pop_until(d, "p", 0, 0);
                parent = d->stack[d->stack_len - 1];
            } else if (tk->name_len == 2 && name[0] == 'l' && name[1] == 'i') {
                wv_dom_pop_until(d, "li", 0, 0);
                parent = d->stack[d->stack_len - 1];
            } else if (tk->name_len == 2 &&
                       ((name[0] == 't' && name[1] == 'd') ||
                        (name[0] == 't' && name[1] == 'h'))) {
                /* <td>/<th> close an open cell, never the row itself */
                wv_dom_pop_until(d, "td", "th", 0);
                parent = d->stack[d->stack_len - 1];
            } else if (tk->name_len == 2 && name[0] == 't' &&
                       name[1] == 'r') {
                /* a new row closes the previous row and its cells */
                wv_dom_pop_until(d, "td", "th", "tr");
                parent = d->stack[d->stack_len - 1];
            }

            uint32_t name_off = wv_dom_pool_put(d, name, tk->name_len);
            uint32_t attr_base = (uint32_t)d->attr_count;
            for (uint32_t a = 0; a < tk->attr_count; a++) {
                const wv_attr_t *src = wv_tok_attr(toks, tk, a);
                if (src) {
                    wv_dom_push_attr(d, src,
                                     wv_tok_str(toks, src->name_off),
                                     wv_tok_str(toks, src->value_off));
                }
            }
            uint32_t n = wv_dom_new_node(d, WV_N_ELEMENT,
                                         name_off, tk->name_len,
                                         0, 0, attr_base, tk->attr_count);
            if (n == WV_NULL) break;
            wv_dom_append_child(d, parent, n);

            if (wv_is_void(name, tk->name_len) || tk->self_closing)
                break;                              /* never nests */
            if (d->stack_len >= eff_cap) {
                d->truncated = 1;
                break;                              /* depth cap: no nesting */
            }
            d->stack[d->stack_len++] = n;
            break;
        }

        case WV_T_END: {
            const char *name = wv_tok_str(toks, tk->name_off);
            /* reconcile: pop until the matching open element.  Elements
             * popped implicitly stay in the tree with their children —
             * only the open-element state is rewound.  When there is no
             * match the end tag is ignored (WHATWG parse error). */
            size_t j = d->stack_len;
            int found = 0;
            while (j > 1) {
                j--;
                if (wv_node_name_equals(d, d->stack[j], name, tk->name_len)) {
                    found = 1;
                    break;
                }
            }
            if (found) d->stack_len = j;
            break;
        }

        default:
            /* comments, doctypes and EOF are not part of the tree */
            break;
        }
    }

    return (int)d->node_count;
}
