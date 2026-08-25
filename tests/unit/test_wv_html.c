/*
 * test_wv_html.c — host unit tests for the web view HTML tokeniser
 * (WEBVIEW_PLAN phase W1).
 *
 * Links the REAL userspace/apps/gbrowser/wv_html.c (never a copy) and checks:
 *   - well-formed HTML tokenises to the exact expected token stream;
 *   - character references: the five named ones plus &#NN; / &#xNN;, in
 *     text and in attribute values; unknown and unterminated references
 *     stay literal;
 *   - every malformed case from the plan's gate terminates and produces
 *     something usable: unclosed tag at EOF, a quote that never closes,
 *     "<", "<<>>", a 10 KB attribute value, a NUL byte mid-tag;
 *   - limits: a 100 000-byte text run and a 10 000-tag document hit the
 *     arena caps, set truncated=1 and still terminate;
 *   - fuzzing with random bytes (including NULs): no crash, no hang,
 *     bounded memory — asserted by walking every produced token and
 *     checking every offset against the arena bounds.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "userspace/apps/gbrowser/wv_html.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* ---- arena storage for the tests (static: bounded by construction) ---- */

static wv_token_t toks[WV_MAX_TOKENS];
static wv_attr_t  attrs[WV_MAX_ATTRS];
static char       pool[WV_POOL_SIZE];
static wv_arena_t arena;

static void reset(void) {
    wv_arena_init(&arena, toks, WV_MAX_TOKENS, attrs, WV_MAX_ATTRS,
                  pool, WV_POOL_SIZE);
}

/* Tokenise and return tok_count (never asserts inside). */
static int tok(const char *html) {
    reset();
    return wv_html_tokenize(&arena, html, strlen(html));
}

/* Tokenise a byte buffer with an explicit length (for embedded NULs). */
static int tok_buf(const char *buf, size_t len) {
    reset();
    return wv_html_tokenize(&arena, buf, len);
}

static const wv_token_t *t(int i) { return &arena.toks[i]; }
static const char *tname(int i)  { return wv_tok_str(&arena, t(i)->name_off); }
static const char *ttext(int i)  { return wv_tok_str(&arena, t(i)->text_off); }

/* Check token i against expectations; returns 1 on match. */
static int expect(int i, int type, const char *name, const char *text,
                  unsigned attr_count, int self_closing) {
    if ((int)arena.tok_count <= i) return 0;
    const wv_token_t *tk = t(i);
    if ((int)tk->type != type) return 0;
    if (name && strcmp(tname(i), name) != 0) return 0;
    if (text && strcmp(ttext(i), text) != 0) return 0;
    if (tk->attr_count != attr_count) return 0;
    if ((int)tk->self_closing != self_closing) return 0;
    return 1;
}

/* Walk every token/attr and verify every offset is inside the pool with a
 * NUL terminator exactly where the length says it is. */
static int arena_is_consistent(void) {
    if (arena.tok_count > arena.tok_cap) return 0;
    if (arena.attr_count > arena.attr_cap) return 0;
    if (arena.pool_used > arena.pool_cap) return 0;
    for (size_t i = 0; i < arena.tok_count; i++) {
        const wv_token_t *tk = &arena.toks[i];
        if (tk->name_len) {
            if ((size_t)tk->name_off + tk->name_len + 1 > arena.pool_used) return 0;
            if (arena.pool[tk->name_off + tk->name_len] != '\0') return 0;
        }
        if (tk->text_len) {
            if ((size_t)tk->text_off + tk->text_len + 1 > arena.pool_used) return 0;
            if (arena.pool[tk->text_off + tk->text_len] != '\0') return 0;
        }
        if (tk->attr_base + tk->attr_count > arena.attr_cap) return 0;
        for (size_t j = 0; j < tk->attr_count; j++) {
            const wv_attr_t *at = wv_tok_attr(&arena, tk, j);
            if (!at) return 0;
            if (at->name_len &&
                ((size_t)at->name_off + at->name_len + 1 > arena.pool_used ||
                 arena.pool[at->name_off + at->name_len] != '\0')) return 0;
            if (at->value_len &&
                ((size_t)at->value_off + at->value_len + 1 > arena.pool_used ||
                 arena.pool[at->value_off + at->value_len] != '\0')) return 0;
        }
    }
    return 1;
}

/* deterministic pseudo-random bytes (xorshift32) */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_null_args(void) {
    reset();
    CK(wv_html_tokenize(NULL, "x", 1) == -1);
    CK(wv_html_tokenize(&arena, NULL, 0) == -1);
    CK(wv_tok_str(&arena, 999999u)[0] == '\0');   /* out-of-range reads are safe */
    CK(wv_tok_attr(&arena, t(0), 0) == NULL);
}

static void test_well_formed(void) {
    int n = tok("<html><head><title>T</title></head>"
                "<body class=\"main\" id=x>Hello &amp; bye<br/></body></html>");
    CK(n == 12);   /* 11 + EOF */
    if (n != 12) { printf("  (got %d tokens)\n", n); return; }

    CK(expect(0, WV_T_START, "html", 0, 0, 0));
    CK(expect(1, WV_T_START, "head", 0, 0, 0));
    CK(expect(2, WV_T_START, "title", 0, 0, 0));
    CK(expect(3, WV_T_TEXT, 0, "T", 0, 0));
    CK(expect(4, WV_T_END, "title", 0, 0, 0));
    CK(expect(5, WV_T_END, "head", 0, 0, 0));

    CK(expect(6, WV_T_START, "body", 0, 2, 0));
    CK(strcmp(tname(6), "body") == 0);
    if (t(6)->attr_count == 2) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(6), 0);
        const wv_attr_t *a1 = wv_tok_attr(&arena, t(6), 1);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->name_off), "class") == 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "main") == 0);
        CK(a1 && strcmp(wv_tok_str(&arena, a1->name_off), "id") == 0);
        CK(a1 && strcmp(wv_tok_str(&arena, a1->value_off), "x") == 0);
    }

    CK(expect(7, WV_T_TEXT, 0, "Hello & bye", 0, 0));
    CK(expect(8, WV_T_START, "br", 0, 0, 1));       /* self-closing */
    CK(expect(9, WV_T_END, "body", 0, 0, 0));
    CK(expect(10, WV_T_END, "html", 0, 0, 0));
    CK(expect(11, WV_T_EOF, 0, 0, 0, 0));
    CK(arena_is_consistent());
}

static void test_char_refs(void) {
    int n = tok("&amp; &lt; &gt; &quot; &apos;");
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "& < > \" '", 0, 0));
    CK(expect(1, WV_T_EOF, 0, 0, 0, 0));

    /* &#0; -> U+FFFD, any code > 0xFF -> U+FFFD (8-bit font ceiling) */
    n = tok("&#65;&#x41;&#0;&#128512;&#x10FFFF;&#x110000;");
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "AA????", 0, 0));

    /* unknown / unterminated references stay literal */
    n = tok("a &unknown; b &amp c");
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "a &unknown; b &amp c", 0, 0));

    /* Live-page named refs: &nbsp; is a space, &copy; is CP1251 0xA9. */
    n = tok("x&nbsp;&copy;2026");
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "x \xA9""2026", 0, 0));

    /* UTF-8 "Почта" maps to the CP1251 glyph bytes. */
    {
        static const char u8[] = "\xD0\x9F\xD0\xBE\xD1\x87\xD1\x82\xD0\xB0";
        n = tok_buf(u8, sizeof(u8) - 1);
        CK(n == 2);
        CK(expect(0, WV_T_TEXT, 0, "\xCF\xEE\xF7\xF2\xE0", 0, 0));
    }

    /* Google.ru: meta claims UTF-8 but the text is windows-1251. */
    {
        static const char lie[] =
            "<meta charset=UTF-8>\xCF\xEE\xF7\xF2\xE0";
        n = tok_buf(lie, sizeof(lie) - 1);
        CK(n == 3);   /* START meta, TEXT Почта, EOF */
        CK(expect(1, WV_T_TEXT, 0, "\xCF\xEE\xF7\xF2\xE0", 0, 0));
    }

    /* refs inside attribute values */
    n = tok("<a t=\"a&amp;b\" u='&#x21;' v=x&amp;y>");
    CK(n == 2);
    if (t(0)->attr_count == 3) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        const wv_attr_t *a1 = wv_tok_attr(&arena, t(0), 1);
        const wv_attr_t *a2 = wv_tok_attr(&arena, t(0), 2);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "a&b") == 0);
        CK(a1 && strcmp(wv_tok_str(&arena, a1->value_off), "!") == 0);
        CK(a2 && strcmp(wv_tok_str(&arena, a2->value_off), "x&y") == 0);
    }
    CK(arena_is_consistent());
}

static void test_malformed(void) {
    /* lone "<" at EOF becomes text */
    int n = tok("<");
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "<", 0, 0));
    CK(expect(1, WV_T_EOF, 0, 0, 0, 0));

    /* "<<>>" — WHATWG: each inner '<' emits '<' as its own text token,
     * then the two '>'s form a second text run */
    n = tok("<<>>");
    CK(n == 3);
    CK(expect(0, WV_T_TEXT, 0, "<", 0, 0));
    CK(expect(1, WV_T_TEXT, 0, ">>", 0, 0));
    CK(expect(2, WV_T_EOF, 0, 0, 0, 0));

    /* unclosed end tag at EOF emits the token */
    n = tok("</div");
    CK(n == 2);
    CK(expect(0, WV_T_END, "div", 0, 0, 0));

    /* unclosed start tag with attribute at EOF */
    n = tok("<div class=\"x");
    CK(n == 2);
    CK(expect(0, WV_T_START, "div", 0, 1, 0));
    if (t(0)->attr_count == 1) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->name_off), "class") == 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "x") == 0);
    }

    /* a quote that never closes swallows to EOF but keeps the tag */
    n = tok("<a href=\"foo>bar");
    CK(n == 2);
    CK(expect(0, WV_T_START, "a", 0, 1, 0));
    if (t(0)->attr_count == 1) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "foo>bar") == 0);
    }

    /* an unclosed comment keeps its text */
    n = tok("<!-- hello");
    CK(n == 2);
    CK(expect(0, WV_T_COMMENT, 0, " hello", 0, 0));

    /* empty input: just EOF */
    n = tok("");
    CK(n == 1);
    CK(expect(0, WV_T_EOF, 0, 0, 0, 0));

    /* "</>" — WHATWG: the '>' in end-tag-open is a parse error dropped;
     * nothing else was accumulated, so only EOF remains */
    n = tok("</>");
    CK(n == 1);
    CK(expect(0, WV_T_EOF, 0, 0, 0, 0));
    CK(arena_is_consistent());
}

static void test_10k_attr_value(void) {
    /* a 10 000-byte attribute value: capped at WV_MAX_ATTR_VALUE, scan
     * continues to the closing quote, truncated is set */
    size_t cap = 10000 + 16;
    char *html = malloc(cap);
    CK(html != NULL);
    if (!html) return;
    size_t p = 0;
    html[p++] = '<'; html[p++] = 'a'; html[p++] = ' ';
    html[p++] = 'h'; html[p++] = 'r'; html[p++] = 'e'; html[p++] = 'f';
    html[p++] = '='; html[p++] = '"';
    for (int i = 0; i < 10000; i++) html[p++] = 'q';
    html[p++] = '"'; html[p++] = '>';
    html[p++] = 'x';

    reset();
    int n = wv_html_tokenize(&arena, html, p);
    CK(n == 3);                       /* START a, TEXT x, EOF */
    CK(arena.truncated == 1);
    CK(expect(0, WV_T_START, "a", 0, 1, 0));
    if (t(0)->attr_count == 1) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        CK(a0 && a0->value_len == WV_MAX_ATTR_VALUE);
        CK(a0 && wv_tok_str(&arena, a0->value_off)[WV_MAX_ATTR_VALUE] == '\0');
    }
    CK(expect(1, WV_T_TEXT, 0, "x", 0, 0));
    CK(arena_is_consistent());
    free(html);
}

static void test_nul_bytes(void) {
    /* NUL mid-tag: replaced with '?' inside the name */
    char t1[] = { '<', 'd', 'i', 0, 'v', '>', 'x' };
    int n = tok_buf(t1, sizeof(t1));
    CK(n == 3);
    CK(expect(0, WV_T_START, "di?v", 0, 0, 0));
    CK(expect(1, WV_T_TEXT, 0, "x", 0, 0));

    /* NUL in text */
    char t2[] = { 'a', 0, 'b' };
    n = tok_buf(t2, sizeof(t2));
    CK(n == 2);
    CK(expect(0, WV_T_TEXT, 0, "a?b", 0, 0));

    /* NUL in attribute value */
    char t3[] = { '<', 'a', ' ', 'x', '=', '"', 'a', 0, 'b', '"', '>' };
    n = tok_buf(t3, sizeof(t3));
    CK(n == 2);
    if (t(0)->attr_count == 1) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "a?b") == 0);
    }
    CK(arena_is_consistent());
}

static void test_decl_and_comment(void) {
    int n = tok("<!DOCTYPE html><p>x");
    CK(n == 4);
    CK(expect(0, WV_T_DOCTYPE, 0, " html", 0, 0));
    CK(expect(1, WV_T_START, "p", 0, 0, 0));
    CK(expect(2, WV_T_TEXT, 0, "x", 0, 0));

    n = tok("<!-- a <!-- nested --><p>y");
    CK(n == 4);
    CK(expect(0, WV_T_COMMENT, 0, " a <!-- nested ", 0, 0));
    CK(expect(1, WV_T_START, "p", 0, 0, 0));
    CK(expect(2, WV_T_TEXT, 0, "y", 0, 0));

    n = tok("<![CDATA[<b>raw & stuff]]>");
    CK(n == 2);
    CK(expect(0, WV_T_CDATA, 0, "<b>raw & stuff", 0, 0));

    /* bogus declarations become comments up to '>' */
    n = tok("<!foo bar><p>z");
    CK(n == 4);
    CK(expect(0, WV_T_COMMENT, 0, "foo bar", 0, 0));
    CK(arena_is_consistent());
}

static void test_misc(void) {
    /* case-insensitive names, attribute without a value */
    int n = tok("<DIV CLASS=\"x\" DISABLED>t");
    CK(n == 3);
    CK(expect(0, WV_T_START, "div", 0, 2, 0));
    if (t(0)->attr_count == 2) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        const wv_attr_t *a1 = wv_tok_attr(&arena, t(0), 1);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->name_off), "class") == 0);
        CK(a1 && strcmp(wv_tok_str(&arena, a1->name_off), "disabled") == 0);
        CK(a1 && a1->value_len == 0);          /* no value */
    }

    /* spaces and '=' spacing */
    n = tok("<a href = 'v' >t");
    CK(n == 3);
    CK(expect(0, WV_T_START, "a", 0, 1, 0));
    if (t(0)->attr_count == 1) {
        const wv_attr_t *a0 = wv_tok_attr(&arena, t(0), 0);
        CK(a0 && strcmp(wv_tok_str(&arena, a0->value_off), "v") == 0);
    }

    /* <p>a<p>b — two sibling paragraphs (the W2 test's raw material) */
    n = tok("<p>a<p>b");
    CK(n == 5);
    CK(expect(0, WV_T_START, "p", 0, 0, 0));
    CK(expect(1, WV_T_TEXT, 0, "a", 0, 0));
    CK(expect(2, WV_T_START, "p", 0, 0, 0));
    CK(expect(3, WV_T_TEXT, 0, "b", 0, 0));

    /* attributes after self-closing slash: "a / b >" style errors stay sane */
    n = tok("<br/ >x");
    CK(n >= 2);   /* <br/> with a space before '>' -> br with attr "/"? */
    CK(arena_is_consistent());
}

static void test_limit_behaviour(void) {
    /* 100 000 'a's: text runs are capped, truncated is set, no hang */
    size_t big = 100000;
    char *html = malloc(big + 1);
    CK(html != NULL);
    if (!html) return;
    memset(html, 'a', big);
    html[big] = 0;
    reset();
    int n = wv_html_tokenize(&arena, html, big);
    CK(n > 0);
    CK(arena.truncated == 1);
    CK(arena_is_consistent());
    free(html);

    /* 10 000 "<" bytes: token cap is hit, scan still drains, no hang */
    size_t caps = 10000;
    char *lt = malloc(caps + 1);
    CK(lt != NULL);
    if (!lt) return;
    memset(lt, '<', caps);
    lt[caps] = 0;
    reset();
    n = wv_html_tokenize(&arena, lt, caps);
    CK(n == (int)arena.tok_cap);       /* every slot filled with text tokens */
    CK(arena.truncated == 1);
    CK(arena_is_consistent());
    free(lt);

    /* 10 000 attributes on one tag: capped per tag, scan drains */
    size_t attrcap = 0;
    {
        size_t need = 10 + 10000 * 6 + 2;
        char *a = malloc(need);
        CK(a != NULL);
        if (a) {
            size_t p = 0;
            a[p++] = '<'; a[p++] = 'a';
            for (int i = 0; i < 10000; i++) {
                a[p++] = ' '; a[p++] = 'x';
                a[p++] = '='; a[p++] = '1';
            }
            a[p++] = '>'; a[p++] = 'z';
            reset();
            int nn = wv_html_tokenize(&arena, a, p);
            CK(nn == 3);
            CK(arena.truncated == 1);
            CK(t(0)->attr_count == WV_MAX_ATTR_PER_TAG);
            CK(expect(1, WV_T_TEXT, 0, "z", 0, 0));
            CK(arena_is_consistent());
            attrcap = p;
            free(a);
        }
    }
    (void)attrcap;
}

static void test_fuzz(void) {
    unsigned char buf[512];
    for (int iter = 0; iter < 3000; iter++) {
        size_t len = rng() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(rng() & 0xFF);
        reset();
        int n = wv_html_tokenize(&arena, (const char *)buf, len);
        if (n < 0) { CK(0); printf("  (fuzz iter %d returned %d)\n", iter, n); return; }
        if (!arena_is_consistent()) {
            CK(0);
            printf("  (fuzz iter %d len %zu: arena inconsistent)\n", iter, len);
            return;
        }
    }
    CK(1);
    printf("PASS: 3000 fuzz iterations produced consistent arenas\n");

    /* one large fuzz blob */
    size_t big = 65536;
    unsigned char *b = malloc(big);
    CK(b != NULL);
    if (b) {
        for (size_t i = 0; i < big; i++) b[i] = (unsigned char)(rng() & 0xFF);
        reset();
        int n = wv_html_tokenize(&arena, (const char *)b, big);
        CK(n > 0);
        CK(arena_is_consistent());
        free(b);
        printf("PASS: 64 KiB fuzz blob tokenised (n=%d)\n", n);
    }
}

int main(void) {
    printf("== webview HTML tokeniser (WEBVIEW_PLAN W1) ==\n");

    test_null_args();
    test_well_formed();
    test_char_refs();
    test_malformed();
    test_10k_attr_value();
    test_nul_bytes();
    test_decl_and_comment();
    test_misc();
    test_limit_behaviour();
    test_fuzz();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
