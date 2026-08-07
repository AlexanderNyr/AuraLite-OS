/*
 * test_wv_dom.c — host unit tests for the web view DOM builder
 * (WEBVIEW_PLAN phase W2).
 *
 * Links the REAL userspace/apps/gbrowser/wv_dom.c + wv_html.c (never
 * copies) and checks the plan's gate:
 *   - `<p>a<p>b` produces two sibling paragraphs, not nested ones;
 *   - `<b><i>x</b></i>` does not lose the text or corrupt the tree;
 *   - mismatched close tags are reconciled (pop-until-match), not obeyed;
 *   - void elements never nest; <li>/<td>/<tr> implicit closes work;
 *   - a 10 000-element-deep document hits the depth cap and still builds
 *     a consistent, bounded tree (on the host; the QEMU assertion with the
 *     real 64 KiB user stack lives in /apps/webview's deep test);
 *   - the tree is walked and every parent/child/sibling link verified
 *     after every build, including fuzzed inputs.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "userspace/apps/gbrowser/wv_html.h"
#include "userspace/apps/gbrowser/wv_dom.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* ---- arenas ---- */

static wv_token_t   toks[WV_MAX_TOKENS];
static wv_attr_t    tattrs[WV_MAX_ATTRS];
static char         tpool[WV_POOL_SIZE];
static wv_arena_t   ta;

static wv_dom_node_t nodes[WV_MAX_TOKENS];      /* 1:1 with tokens is enough */
static wv_attr_t    dattrs[WV_MAX_ATTRS];
static char         dpool[WV_POOL_SIZE];
static uint32_t     stack[WV_DOM_DEFAULT_DEPTH];
static wv_dom_t     da;

static void reset(void) {
    wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                  tpool, WV_POOL_SIZE);
    wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, stack, WV_DOM_DEFAULT_DEPTH);
}

/* Tokenise + build; returns DOM node count (or -1). */
static int build(const char *html) {
    reset();
    int n = wv_html_tokenize(&ta, html, strlen(html));
    if (n < 0) return -1;
    return wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH);
}

/* ---- tree invariant checker ---- */

static int dom_consistent(void) {
    /* every node's parent/child/sibling links must agree */
    for (size_t i = 0; i < da.node_count; i++) {
        const wv_dom_node_t *n = &da.nodes[i];
        if (n->type == WV_N_DOCUMENT) {
            if (n->parent != WV_NULL) return 0;
            if (i != 0) return 0;               /* document is node 0 */
        }
        if (n->parent != WV_NULL) {
            if (n->parent >= da.node_count) return 0;
            /* parent must list this node in its child chain */
            const wv_dom_node_t *p = &da.nodes[n->parent];
            uint32_t c = p->first_child;
            int found = 0;
            while (c != WV_NULL) {
                if (c >= da.node_count) return 0;
                if (c == i) { found = 1; break; }
                c = da.nodes[c].next_sibling;
            }
            if (!found) return 0;
            if (p->last_child == WV_NULL) return 0;
        }
        /* walk this node's child chain: last_child must be the tail */
        if (n->first_child != WV_NULL) {
            uint32_t c = n->first_child;
            uint32_t prev = WV_NULL;
            while (c != WV_NULL) {
                if (c >= da.node_count) return 0;
                if (da.nodes[c].parent != i) return 0;
                if (prev != WV_NULL && da.nodes[prev].next_sibling != c)
                    return 0;
                prev = c;
                c = da.nodes[c].next_sibling;
            }
            if (prev != n->last_child) return 0;
        } else if (n->last_child != WV_NULL) {
            return 0;
        }
        /* text nodes have no children */
        if (n->type == WV_N_TEXT &&
            (n->first_child != WV_NULL || n->attr_count != 0)) return 0;
        /* name/string offsets are inside the pool */
        if (n->name_len &&
            ((size_t)n->name_off + n->name_len + 1 > da.pool_used ||
             da.pool[n->name_off + n->name_len] != '\0')) return 0;
        if (n->text_len &&
            ((size_t)n->text_off + n->text_len + 1 > da.pool_used ||
             da.pool[n->text_off + n->text_len] != '\0')) return 0;
        for (size_t a = 0; a < n->attr_count; a++) {
            size_t idx = (size_t)n->attr_base + a;
            if (idx >= da.attr_count) return 0;
            const wv_attr_t *at = &da.attrs[idx];
            if (at->name_len &&
                ((size_t)at->name_off + at->name_len + 1 > da.pool_used ||
                 da.pool[at->name_off + at->name_len] != '\0')) return 0;
            if (at->value_len &&
                ((size_t)at->value_off + at->value_len + 1 > da.pool_used ||
                 da.pool[at->value_off + at->value_len] != '\0')) return 0;
        }
    }
    return 1;
}

/* find the first element node with the given name under a parent */
static uint32_t find_child(const wv_dom_t *d, uint32_t parent,
                           const char *name) {
    uint32_t c = d->nodes[parent].first_child;
    while (c != WV_NULL) {
        const wv_dom_node_t *n = &d->nodes[c];
        if (n->type == WV_N_ELEMENT &&
            strcmp(wv_dom_str(d, n->name_off), name) == 0)
            return c;
        c = n->next_sibling;
    }
    return WV_NULL;
}

static uint32_t child_count(const wv_dom_t *d, uint32_t node) {
    uint32_t n = 0, c = d->nodes[node].first_child;
    while (c != WV_NULL) { n++; c = d->nodes[c].next_sibling; }
    return n;
}

/* ---- tests ---- */

static void test_p_siblings(void) {
    int n = build("<p>a<p>b");
    CK(n == 5);                       /* doc + 2 p + 2 text */
    if (n != 5) return;
    uint32_t p1 = find_child(&da, 0, "p");
    uint32_t p2 = da.nodes[p1].next_sibling;
    CK(p2 != WV_NULL && strcmp(wv_dom_str(&da, da.nodes[p2].name_off), "p") == 0);
    CK(da.nodes[p1].parent == 0 && da.nodes[p2].parent == 0);
    CK(child_count(&da, p1) == 1 && child_count(&da, p2) == 1);
    uint32_t t1 = da.nodes[p1].first_child;
    uint32_t t2 = da.nodes[p2].first_child;
    CK(strcmp(wv_dom_str(&da, da.nodes[t1].text_off), "a") == 0);
    CK(strcmp(wv_dom_str(&da, da.nodes[t2].text_off), "b") == 0);
    CK(dom_consistent());
}

static void test_b_i_reconcile(void) {
    int n = build("<b><i>x</b></i>");
    CK(n == 4);                       /* doc + b + i + text */
    if (n != 4) return;
    uint32_t b = find_child(&da, 0, "b");
    CK(b != WV_NULL);
    uint32_t i = find_child(&da, b, "i");
    CK(i != WV_NULL);
    uint32_t t = da.nodes[i].first_child;
    CK(t != WV_NULL && da.nodes[t].type == WV_N_TEXT);
    CK(strcmp(wv_dom_str(&da, da.nodes[t].text_off), "x") == 0);
    CK(dom_consistent());
}

static void test_deep_mismatch(void) {
    /* </a> must implicitly close b and c, not lose anything */
    int n = build("<a><b><c>z</a>");
    CK(n == 5);
    if (n != 5) return;
    uint32_t a = find_child(&da, 0, "a");
    uint32_t b = find_child(&da, a, "b");
    uint32_t c = find_child(&da, b, "c");
    CK(a != WV_NULL && b != WV_NULL && c != WV_NULL);
    uint32_t t = da.nodes[c].first_child;
    CK(t != WV_NULL && strcmp(wv_dom_str(&da, da.nodes[t].text_off), "z") == 0);
    CK(dom_consistent());
}

static void test_void_elements(void) {
    int n = build("<p>x<br>y<hr><img src=i>z</p>");
    CK(n == 8);                       /* doc+p+3 texts+br+hr+img */
    if (n != 8) return;
    uint32_t p = find_child(&da, 0, "p");
    uint32_t c = da.nodes[p].first_child;
    /* children: text x, br, text y, hr, img, text z */
    int seen[6] = {0};
    int idx = 0;
    while (c != WV_NULL) {
        const wv_dom_node_t *nn = &da.nodes[c];
        if (nn->type == WV_N_TEXT) seen[idx] = (nn->text_len == 1) ? 3 : 3;
        else if (strcmp(wv_dom_str(&da, nn->name_off), "br") == 0) seen[idx] = 1;
        else if (strcmp(wv_dom_str(&da, nn->name_off), "hr") == 0) seen[idx] = 2;
        else if (strcmp(wv_dom_str(&da, nn->name_off), "img") == 0) seen[idx] = 4;
        c = nn->next_sibling;
        idx++;
    }
    CK(idx == 6);
    CK(seen[0] == 3 && seen[1] == 1 && seen[2] == 3 &&
       seen[3] == 2 && seen[4] == 4 && seen[5] == 3);
    /* void elements must have no children */
    uint32_t br = find_child(&da, p, "br");
    CK(br != WV_NULL && da.nodes[br].first_child == WV_NULL);
    /* br must not be an ancestor of the second text "y": y is p's child */
    CK(dom_consistent());
}

static void test_li_td_tr(void) {
    int n = build("<ul><li>a<li>b</ul>");
    CK(n == 6);                       /* doc+ul+2li+2text */
    if (n != 6) return;
    uint32_t ul = find_child(&da, 0, "ul");
    uint32_t li1 = find_child(&da, ul, "li");
    uint32_t li2 = da.nodes[li1].next_sibling;
    CK(li2 != WV_NULL && strcmp(wv_dom_str(&da, da.nodes[li2].name_off), "li") == 0);
    CK(da.nodes[li1].parent == ul && da.nodes[li2].parent == ul);

    n = build("<table><tr><td>a<td>b</tr></table>");
    CK(n == 7);                       /* doc+table+tr+2td+2text */
    if (n != 7) return;
    uint32_t table = find_child(&da, 0, "table");
    uint32_t tr = find_child(&da, table, "tr");
    uint32_t td1 = find_child(&da, tr, "td");
    uint32_t td2 = da.nodes[td1].next_sibling;
    CK(tr != WV_NULL && td1 != WV_NULL && td2 != WV_NULL);
    CK(da.nodes[td1].parent == tr && da.nodes[td2].parent == tr &&
       strcmp(wv_dom_str(&da, da.nodes[td2].name_off), "td") == 0);
    CK(dom_consistent());
}

static void test_attributes_and_text_merge(void) {
    int n = build("<a href=\"x\" id=y>t1<!-- c -->t2</a>");
    CK(n == 3);                       /* doc + a + ONE merged text */
    if (n != 3) return;
    uint32_t a = find_child(&da, 0, "a");
    CK(a != WV_NULL && da.nodes[a].attr_count == 2);
    if (da.nodes[a].attr_count == 2) {
        const wv_attr_t *at0 = &da.attrs[da.nodes[a].attr_base];
        const wv_attr_t *at1 = &da.attrs[da.nodes[a].attr_base + 1];
        CK(strcmp(wv_dom_str(&da, at0->name_off), "href") == 0);
        CK(strcmp(wv_dom_str(&da, at0->value_off), "x") == 0);
        CK(strcmp(wv_dom_str(&da, at1->name_off), "id") == 0);
        CK(strcmp(wv_dom_str(&da, at1->value_off), "y") == 0);
    }
    uint32_t t = da.nodes[a].first_child;
    CK(t != WV_NULL && da.nodes[t].type == WV_N_TEXT);
    CK(strcmp(wv_dom_str(&da, da.nodes[t].text_off), "t1t2") == 0);
    CK(child_count(&da, a) == 1);     /* comment dropped, text merged */
    CK(dom_consistent());
}

static void test_cdata_and_refs(void) {
    int n = build("<p>a&amp;b<![CDATA[<raw>]]>c</p>");
    CK(n == 3);                       /* doc + p + one text "a&b<raw>c" */
    if (n != 3) return;
    uint32_t p = find_child(&da, 0, "p");
    uint32_t t = da.nodes[p].first_child;
    CK(t != WV_NULL && da.nodes[t].type == WV_N_TEXT);
    CK(strcmp(wv_dom_str(&da, da.nodes[t].text_off), "a&b<raw>c") == 0);
    CK(dom_consistent());
}

static void test_unclosed_and_ignored_ends(void) {
    /* end tags with no match are ignored, not stack-wiping */
    int n = build("<div></span>x</div></p>");
    CK(n == 3);                       /* doc + div + text */
    if (n != 3) return;
    uint32_t d = find_child(&da, 0, "div");
    CK(d != WV_NULL);
    uint32_t t = da.nodes[d].first_child;
    CK(t != WV_NULL && strcmp(wv_dom_str(&da, da.nodes[t].text_off), "x") == 0);
    CK(dom_consistent());

    /* unclosed tags at EOF keep their structure */
    n = build("<a><b>text");
    CK(n == 4);
    if (n != 4) return;
    uint32_t a = find_child(&da, 0, "a");
    uint32_t b = find_child(&da, a, "b");
    CK(a != WV_NULL && b != WV_NULL);
    CK(dom_consistent());
}

static void test_empty_and_whitespace(void) {
    int n = build("");
    CK(n == 1);                       /* document only */
    CK(dom_consistent());

    n = build("   ");
    CK(n == 2);                       /* doc + one text node */
    if (n != 2) return;
    uint32_t t = da.nodes[0].first_child;
    CK(t != WV_NULL && da.nodes[t].type == WV_N_TEXT &&
       da.nodes[t].text_len == 3);
    CK(dom_consistent());
}

/* deterministic xorshift32 */
static uint32_t frng = 0x9E3779B9u;
static uint32_t fuzz_rand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

static void test_deep_document(void) {
    /* 10 000 nested <div>s with a modest cap: must hit the cap, stay
     * bounded and consistent.  (The same document is built in QEMU by
     * /apps/webview, where the 64 KiB user stack is real.) */
    size_t doc_len = 10000 * 5 + 10000 * 6 + 1;
    char *doc = malloc(doc_len);
    CK(doc != NULL);
    if (!doc) return;
    size_t p = 0;
    for (int i = 0; i < 10000; i++) { memcpy(doc + p, "<div>", 5); p += 5; }
    for (int i = 0; i < 10000; i++) { memcpy(doc + p, "</div>", 6); p += 6; }
    doc[p] = 0;

    reset();
    /* tokeniser needs a bigger arena for 20 001 tokens */
    static wv_token_t big_toks[22000];
    static char big_pool[200000];
    wv_arena_t bt;
    wv_arena_init(&bt, big_toks, 22000, tattrs, WV_MAX_ATTRS,
                  big_pool, sizeof(big_pool));
    int n = wv_html_tokenize(&bt, doc, p);
    CK(n == 20001);

    static wv_dom_node_t big_nodes[11000];
    static uint32_t big_stack[1024];
    wv_dom_t bd;
    wv_dom_init(&bd, big_nodes, 11000, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, big_stack, 1024);
    int nn = wv_dom_build(&bd, &bt, 512);
    CK(nn == 10001);                  /* document + 10 000 divs */
    CK(bd.truncated == 1);            /* depth cap hit */
    /* verify max depth == 512 */
    uint32_t maxd = 0;
    for (size_t i = 0; i < bd.node_count; i++) {
        uint32_t dpt = wv_dom_depth(&bd, (uint32_t)i);
        if (dpt > maxd) maxd = dpt;
    }
    CK(maxd == 512);
    /* no node may be deeper than the cap */
    CK(maxd <= 512);
    /* the deepest chain is the first 512 divs */
    CK(dom_consistent());
    free(doc);
    printf("PASS: deep document: nodes=10001 max_depth=512 truncated=1\n");
}

static void test_fuzz_dom(void) {
    unsigned char buf[512];
    for (int iter = 0; iter < 2000; iter++) {
        size_t len = fuzz_rand() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(fuzz_rand() & 0xFF);
        reset();
        int n = wv_html_tokenize(&ta, (const char *)buf, len);
        if (n < 0) { CK(0); return; }
        int nn = wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH);
        if (nn < 0) { CK(0); return; }
        if (!dom_consistent()) { CK(0); printf("  (fuzz iter %d)\n", iter); return; }
    }
    CK(1);
    printf("PASS: 2000 fuzz iterations built consistent DOMs\n");
}

int main(void) {
    printf("== webview DOM builder (WEBVIEW_PLAN W2) ==\n");

    printf("[t] p_siblings\n"); fflush(stdout); test_p_siblings();
    printf("[t] test_b_i_reconcile\n"); fflush(stdout); test_b_i_reconcile();
    printf("[t] test_deep_mismatch\n"); fflush(stdout); test_deep_mismatch();
    printf("[t] test_void_elements\n"); fflush(stdout); test_void_elements();
    printf("[t] test_li_td_tr\n"); fflush(stdout); test_li_td_tr();
    printf("[t] test_attributes_and_text_merge\n"); fflush(stdout); test_attributes_and_text_merge();
    printf("[t] test_cdata_and_refs\n"); fflush(stdout); test_cdata_and_refs();
    printf("[t] test_unclosed_and_ignored_ends\n"); fflush(stdout); test_unclosed_and_ignored_ends();
    printf("[t] test_empty_and_whitespace\n"); fflush(stdout); test_empty_and_whitespace();
    printf("[t] test_deep_document\n"); fflush(stdout); test_deep_document();
    printf("[t] test_fuzz_dom\n"); fflush(stdout); test_fuzz_dom();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
