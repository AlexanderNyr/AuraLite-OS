/*
 * wv_dom.h — DOM builder for the AuraLite web view (WEBVIEW_PLAN W2).
 *
 * Design constraints from the plan (D3, §2):
 *   - The user stack is 64 KiB.  The builder is iterative: an explicit
 *     open-element stack (an index array inside the caller-provided arena)
 *     with a HARD DEPTH CAP.  When the cap is reached, deeper elements are
 *     appended without nesting and `truncated` is set — a 10 000-deep
 *     document must hit the cap, not the stack.
 *   - Nodes live in a FLAT ARRAY with index links (parent, first/last
 *     child, next sibling) — one allocation, and a tree walk that cannot
 *     run away.
 *   - Implicit close rules for the tags real pages omit: <p> closes an
 *     open <p>; <li> closes an open <li>; <td>/<th> close td/th/tr; <tr>
 *     closes td/th/tr; void elements never nest.
 *   - Mismatched close tags are RECONCILED against the stack (WHATWG
 *     "pop until a match"): `<b><i>x</b></i>` closes <i> implicitly when
 *     </b> arrives, so no text is lost and the tree stays well-formed.
 *
 * The builder consumes the token arena from wv_html (which must outlive
 * the DOM) and copies every string it keeps into its own pool, so the DOM
 * is self-contained once built.
 */

#ifndef AURALITE_WV_DOM_H
#define AURALITE_WV_DOM_H

#include <stddef.h>
#include <stdint.h>
#include "wv_html.h"

/* Sentinel index: "no such node". */
#define WV_NULL 0xFFFFFFFFu

typedef enum {
    WV_N_DOCUMENT = 0,   /* implicit root, node 0 */
    WV_N_ELEMENT,
    WV_N_TEXT
} wv_node_type;

typedef struct {
    uint32_t parent;        /* WV_NULL for the document */
    uint32_t first_child;   /* WV_NULL when childless */
    uint32_t last_child;
    uint32_t next_sibling;  /* WV_NULL for the last child */
    uint8_t  type;
    uint8_t  flags;         /* reserved */
    uint16_t _pad;
    uint32_t name_off;      /* element name in the DOM pool; 0 = none */
    uint32_t name_len;
    uint32_t text_off;      /* text content in the DOM pool; 0 = none */
    uint32_t text_len;
    uint32_t attr_base;     /* index into d->attrs[] */
    uint32_t attr_count;
} wv_dom_node_t;

/* Default nesting cap: deep enough for any real page, shallow enough that
 * the explicit stack fits in a small fixed array. */
#define WV_DOM_DEFAULT_DEPTH 512

typedef struct {
    wv_dom_node_t *nodes;   size_t node_cap;   size_t node_count;
    wv_attr_t     *attrs;   size_t attr_cap;   size_t attr_count;
    char          *pool;    size_t pool_cap;   size_t pool_used;
    uint32_t      *stack;   size_t stack_cap;  size_t stack_len;
    int            truncated;
} wv_dom_t;

void wv_dom_init(wv_dom_t *d,
                 wv_dom_node_t *nodes, size_t node_cap,
                 wv_attr_t     *attrs, size_t attr_cap,
                 char          *pool,  size_t pool_cap,
                 uint32_t      *stack, size_t stack_cap);

/* Build the tree from a token arena.  depth_cap is the maximum nesting
 * depth (0/absent = WV_DOM_DEFAULT_DEPTH, clamped to the stack array).
 * Returns the node count (including the implicit document), or -1 on
 * NULL arguments.  `truncated` is set when any arena limit or the depth
 * cap was hit; the tree is still usable. */
int wv_dom_build(wv_dom_t *d, const wv_arena_t *toks, size_t depth_cap);

/* Nesting depth of a node (0 = document, 1 = child of document, ...). */
uint32_t wv_dom_depth(const wv_dom_t *d, uint32_t node);

/* NUL-terminated string from the DOM pool ("" for out-of-range). */
const char *wv_dom_str(const wv_dom_t *d, uint32_t off);

#endif /* AURALITE_WV_DOM_H */
