/*
 * wv_html.c — HTML tokeniser for the AuraLite web view (WEBVIEW_PLAN W1).
 *
 * A table-driven state machine over the byte stream, following the WHATWG
 * HTML tokeniser states with deliberate simplifications documented in
 * wv_html.h.  Everything is iterative; the only "buffers" are fixed-size
 * locals (a few KB total) and the caller-provided arena, so neither the
 * 64 KiB user stack nor memory as a whole can be exhausted by input.
 *
 * Tolerance rules honoured throughout:
 *   - EOF in any state terminates with whatever was accumulated (a token
 *     is emitted for it) plus a WV_T_EOF token — never a hang, never a
 *     crash, never an empty result when input existed.
 *   - A malformed construct degrades to the closest useful token: "<"
 *     alone becomes text, "<<>>" becomes text, an unclosed quote keeps
 *     the tag and everything before the quote, an unclosed comment keeps
 *     the comment text.
 *   - When an internal limit is hit the scan continues to EOF and the
 *     data is dropped; arena->truncated tells the caller.
 */

#include "wv_html.h"

/* ---- tiny character helpers (no libc, so host and guest agree) ---- */

static int wv_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static unsigned char wv_to_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c + 32);
    return c;
}

static int wv_is_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int wv_eq_ci(const char *s, size_t pos, size_t len, const char *lit) {
    size_t i = 0;
    while (lit[i]) {
        if (pos + i >= len) return 0;
        if (wv_to_lower((unsigned char)s[pos + i]) != (unsigned char)lit[i]) return 0;
        i++;
    }
    return 1;
}

/* ---- arena helpers ---- */

void wv_arena_init(wv_arena_t *a, wv_token_t *toks, size_t tok_cap,
                   wv_attr_t *attrs, size_t attr_cap,
                   char *pool, size_t pool_cap) {
    a->toks = toks; a->tok_cap = tok_cap; a->tok_count = 0;
    a->attrs = attrs; a->attr_cap = attr_cap; a->attr_count = 0;
    a->pool = pool; a->pool_cap = pool_cap; a->pool_used = 0;
    a->truncated = 0;
    if (a->pool && a->pool_cap > 0) {
        a->pool[0] = '\0';      /* offset 0 is always the empty string */
        a->pool_used = 1;
    }
}

const char *wv_tok_str(const wv_arena_t *a, uint32_t off) {
    if (!a || !a->pool) return "";
    if ((size_t)off >= a->pool_used) return "";
    return &a->pool[off];
}

const wv_attr_t *wv_tok_attr(const wv_arena_t *a, const wv_token_t *t, size_t i) {
    if (!a || !t) return NULL;
    if (i >= t->attr_count) return NULL;
    size_t idx = (size_t)t->attr_base + i;
    if (idx >= a->attr_cap) return NULL;
    return &a->attrs[idx];
}

/* Append bytes + NUL to the pool.  Returns the offset, or 0 when the pool
 * is full (truncated is set; the scan continues, the data is dropped). */
static uint32_t wv_pool_put(wv_arena_t *a, const char *buf, size_t len) {
    if (len + 1 > a->pool_cap - a->pool_used) {
        a->truncated = 1;
        return 0;
    }
    uint32_t off = (uint32_t)a->pool_used;
    for (size_t i = 0; i < len; i++) a->pool[a->pool_used + i] = buf[i];
    a->pool[a->pool_used + len] = '\0';
    a->pool_used += len + 1;
    return off;
}

static void wv_push_attr(wv_arena_t *a, const char *name, size_t name_len,
                         const char *val, size_t val_len, int has_value) {
    if (a->attr_count >= a->attr_cap) { a->truncated = 1; return; }
    wv_attr_t *at = &a->attrs[a->attr_count++];
    at->name_off = wv_pool_put(a, name, name_len);
    at->name_len = (uint32_t)name_len;
    if (has_value) {
        at->value_off = wv_pool_put(a, val, val_len);
        at->value_len = (uint32_t)val_len;
    } else {
        at->value_off = 0;
        at->value_len = 0;
    }
}

static void wv_push_token(wv_arena_t *a, int type,
                          uint32_t name_off, uint32_t name_len,
                          uint32_t text_off, uint32_t text_len,
                          size_t attr_base, size_t attr_count,
                          int self_closing, uint32_t line, uint32_t col) {
    if (a->tok_count >= a->tok_cap) { a->truncated = 1; return; }
    wv_token_t *t = &a->toks[a->tok_count++];
    t->type = (wv_tok_type)type;
    t->name_off = name_off; t->name_len = name_len;
    t->text_off = text_off; t->text_len = text_len;
    t->attr_base = (uint32_t)attr_base;
    t->attr_count = (uint32_t)attr_count;
    t->self_closing = self_closing ? 1u : 0u;
    t->line = line; t->col = col;
}

/* ---- character references ----
 * Tries to consume a reference at s[*i] (which must be '&').  On success
 * writes the decoded byte to out, advances *i past the ';' and returns 1.
 * On failure returns 0 and leaves *i untouched — the '&' is literal text.
 * A reference must end in ';' to be consumed (documented simplification).
 */
static int wv_try_char_ref(const char *s, size_t len, size_t *i,
                           char *out, size_t out_cap, size_t *out_len) {
    if (out_cap < 1) return 0;
    size_t p = *i;
    if (p + 1 >= len || s[p] != '&') return 0;

    if (s[p + 1] == '#') {
        size_t j = p + 2;
        int hex = 0;
        if (j < len && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
        uint32_t val = 0;
        size_t digits = 0;
        while (j < len && digits < 8) {
            unsigned char d = (unsigned char)s[j];
            uint32_t dv;
            if (d >= '0' && d <= '9') dv = (uint32_t)(d - '0');
            else if (hex && d >= 'a' && d <= 'f') dv = (uint32_t)(d - 'a' + 10);
            else if (hex && d >= 'A' && d <= 'F') dv = (uint32_t)(d - 'A' + 10);
            else break;
            val = val * (hex ? 16u : 10u) + dv;
            digits++; j++;
        }
        if (digits == 0) return 0;
        if (j >= len || s[j] != ';') return 0;      /* must end in ';' */
        if (val > 0x10FFFFu) val = 0xFFFDu;          /* out of range -> U+FFFD */
        if (val == 0 || val > 0xFFu) out[0] = '?';   /* 8-bit font ceiling */
        else out[0] = (char)val;
        *i = j + 1;
        *out_len = 1;
        return 1;
    }

    /* The five named references. */
    static const struct { const char *name; char ch; } named[] = {
        { "amp",  '&' }, { "lt", '<' }, { "gt", '>' },
        { "quot", '"' }, { "apos", '\'' },
    };
    for (size_t k = 0; k < 5; k++) {
        size_t nl = 0;
        while (named[k].name[nl]) nl++;
        if (p + 1 + nl >= len) continue;
        int match = 1;
        for (size_t m = 0; m < nl; m++)
            if (s[p + 1 + m] != named[k].name[m]) { match = 0; break; }
        if (!match) continue;
        if (s[p + 1 + nl] != ';') continue;
        out[0] = named[k].ch;
        *i = p + 1 + nl + 1;
        *out_len = 1;
        return 1;
    }
    return 0;
}

/* ---- tokeniser states ---- */
enum {
    ST_TEXT = 0,
    ST_TAG_OPEN,
    ST_END_TAG_OPEN,
    ST_TAG_NAME,
    ST_BEFORE_ATTR_NAME,
    ST_ATTR_NAME,
    ST_AFTER_ATTR_NAME,
    ST_BEFORE_ATTR_VALUE,
    ST_ATTR_VALUE_DQ,
    ST_ATTR_VALUE_SQ,
    ST_ATTR_VALUE_UNQ,
    ST_AFTER_ATTR_VALUE,
    ST_SELF_CLOSING_START,
    ST_MARKUP_DECL,
    ST_COMMENT,
    ST_BOGUS_COMMENT,
    ST_DOCTYPE,
    ST_CDATA
};

int wv_html_tokenize(wv_arena_t *a, const char *html, size_t len) {
    if (!a || !html) return -1;
    wv_arena_init(a, a->toks, a->tok_cap, a->attrs, a->attr_cap,
                  a->pool, a->pool_cap);

    size_t pos = 0;
    int state = ST_TEXT;
    uint32_t line = 1, col = 1;

    /* current tag being scanned */
    char tag_name[WV_MAX_NAME + 1];
    size_t tag_name_len = 0;
    int tag_name_overflow = 0;
    int is_end = 0;
    int self_closing = 0;
    size_t attr_base = 0, attr_count_cur = 0;

    /* current attribute being scanned */
    char attr_name[WV_MAX_NAME + 1];
    size_t attr_name_len = 0;
    char attr_val[WV_MAX_ATTR_VALUE + 1];
    size_t attr_val_len = 0;
    int attr_val_overflow = 0;
    int attr_has_value = 0;

    /* current text / comment / doctype / cdata run */
    char text[WV_MAX_TEXT + 1];
    size_t text_len = 0;
    int text_overflow = 0;
    char comment[WV_MAX_COMMENT + 1];
    size_t comment_len = 0;
    int comment_overflow = 0;

    uint32_t tok_line = 1, tok_col = 1;   /* start of the current token */

#define ADV() do { \
        if (html[pos] == '\n') { line++; col = 1; } else { col++; } \
        pos++; \
    } while (0)

#define ADV_N(n) do { \
        size_t _n = (n); \
        for (size_t _k = 0; _k < _n; _k++) { \
            if (html[pos] == '\n') { line++; col = 1; } else { col++; } \
            pos++; \
        } \
    } while (0)

/* Advance line/col over bytes [from, to) WITHOUT moving pos — used after a
 * character reference, which already advanced pos itself. */
#define COUNT_ADV(from, to) do { \
        size_t _f = (from), _t = (to); \
        for (size_t _k = _f; _k < _t; _k++) { \
            if (html[_k] == '\n') { line++; col = 1; } else { col++; } \
        } \
    } while (0)

#define TXT_PUSH(c) do { \
        if (text_len >= WV_MAX_TEXT) text_overflow = 1; \
        else text[text_len++] = (char)(c); \
    } while (0)

#define ATTRN_PUSH(c) do { \
        if (attr_name_len >= WV_MAX_NAME) { /* drop, scan continues */ } \
        else attr_name[attr_name_len++] = (char)(c); \
    } while (0)

#define ATTRV_PUSH(c) do { \
        if (attr_val_len >= WV_MAX_ATTR_VALUE) attr_val_overflow = 1; \
        else attr_val[attr_val_len++] = (char)(c); \
    } while (0)

#define TAGN_PUSH(c) do { \
        if (tag_name_len >= WV_MAX_NAME) tag_name_overflow = 1; \
        else tag_name[tag_name_len++] = (char)(c); \
    } while (0)

#define CMNT_PUSH(c) do { \
        if (comment_len >= WV_MAX_COMMENT) comment_overflow = 1; \
        else comment[comment_len++] = (char)(c); \
    } while (0)

    /* flush accumulated text into a WV_T_TEXT token */
#define FLUSH_TEXT() do { \
        if (text_len > 0) { \
            uint32_t off = wv_pool_put(a, text, text_len); \
            wv_push_token(a, WV_T_TEXT, 0, 0, off, (uint32_t)text_len, \
                          0, 0, 0, tok_line, tok_col); \
            if (text_overflow) a->truncated = 1; \
            text_len = 0; text_overflow = 0; \
        } \
    } while (0)

    /* finish the attribute being scanned, if any */
#define FLUSH_ATTR() do { \
        if (attr_name_len > 0 || attr_has_value) { \
            if (attr_count_cur >= WV_MAX_ATTR_PER_TAG) { \
                a->truncated = 1; \
            } else { \
                wv_push_attr(a, attr_name, attr_name_len, \
                             attr_val, attr_val_len, attr_has_value); \
                attr_count_cur++; \
            } \
            if (attr_val_overflow) a->truncated = 1; \
        } \
        attr_name_len = 0; attr_val_len = 0; \
        attr_val_overflow = 0; attr_has_value = 0; \
    } while (0)

    /* finish the tag being scanned into a START/END token */
#define EMIT_TAG() do { \
        uint32_t noff = (tag_name_len > 0 && !tag_name_overflow) \
                            ? wv_pool_put(a, tag_name, tag_name_len) : 0; \
        wv_push_token(a, is_end ? WV_T_END : WV_T_START, \
                      noff, (uint32_t)(tag_name_overflow ? 0 : tag_name_len), \
                      0, 0, attr_base, attr_count_cur, self_closing, \
                      tok_line, tok_col); \
        if (tag_name_overflow) a->truncated = 1; \
        tag_name_len = 0; tag_name_overflow = 0; \
        is_end = 0; self_closing = 0; \
        attr_base = a->attr_count; attr_count_cur = 0; \
    } while (0)

    /* finish a comment/doctype/cdata run into its token */
#define EMIT_RAW(kind) do { \
        uint32_t off = wv_pool_put(a, comment, comment_len); \
        wv_push_token(a, (kind), 0, 0, off, (uint32_t)comment_len, \
                      0, 0, 0, tok_line, tok_col); \
        if (comment_overflow) a->truncated = 1; \
        comment_len = 0; comment_overflow = 0; \
    } while (0)

    while (pos < len) {
        unsigned char c = (unsigned char)html[pos];

        switch (state) {

        case ST_TEXT:
            if (c == '<') {
                FLUSH_TEXT();
                tok_line = line; tok_col = col;
                state = ST_TAG_OPEN;
                ADV();
            } else if (c == '&') {
                char ref[1]; size_t rl = 0; size_t save = pos;
                if (wv_try_char_ref(html, len, &pos, ref, 1, &rl)) {
                    if (text_len == 0) { tok_line = line; tok_col = col; }
                    TXT_PUSH(ref[0]);
                    COUNT_ADV(save, pos);       /* pos already advanced */
                } else {
                    if (text_len == 0) { tok_line = line; tok_col = col; }
                    TXT_PUSH('&');
                    ADV();
                }
            } else {
                if (text_len == 0) { tok_line = line; tok_col = col; }
                TXT_PUSH(c == 0 ? '?' : (char)c);
                ADV();
            }
            break;

        case ST_TAG_OPEN:
            if (c == '!') {
                state = ST_MARKUP_DECL;
                ADV();
            } else if (c == '/') {
                state = ST_END_TAG_OPEN;
                ADV();
            } else if (c == '?') {
                state = ST_BOGUS_COMMENT;
                comment_len = 0; comment_overflow = 0;
                ADV();
            } else if (wv_is_alpha(c)) {
                tag_name_len = 0; tag_name_overflow = 0;
                is_end = 0; self_closing = 0;
                attr_base = a->attr_count; attr_count_cur = 0;
                state = ST_TAG_NAME;
                TAGN_PUSH(wv_to_lower(c));
                ADV();
            } else if (c == '<') {
                /* WHATWG: parse error, emit '<' as text, reconsume. */
                FLUSH_TEXT();
                text[0] = '<'; text_len = 1; tok_line = line; tok_col = col;
                FLUSH_TEXT();
                state = ST_TEXT;
                ADV();
            } else if (c == '>') {
                /* WHATWG: parse error, switch to data. */
                state = ST_TEXT;
                ADV();
            } else {
                /* WHATWG: parse error, emit '<' as text, reconsume. */
                FLUSH_TEXT();
                text[0] = '<'; text_len = 1; tok_line = line; tok_col = col;
                FLUSH_TEXT();
                state = ST_TEXT;
                ADV();
            }
            break;

        case ST_END_TAG_OPEN:
            if (wv_is_alpha(c)) {
                tag_name_len = 0; tag_name_overflow = 0;
                is_end = 1; self_closing = 0;
                attr_base = a->attr_count; attr_count_cur = 0;
                state = ST_TAG_NAME;
                TAGN_PUSH(wv_to_lower(c));
                ADV();
            } else if (c == '>') {
                /* WHATWG: parse error, switch to data. */
                state = ST_TEXT;
                ADV();
            } else {
                /* WHATWG: emit "</" as text, reconsume in bogus comment;
                 * simplified: emit "</" and treat the byte as text. */
                FLUSH_TEXT();
                text[0] = '<'; text[1] = '/'; text_len = 2;
                tok_line = line; tok_col = col;
                FLUSH_TEXT();
                state = ST_TEXT;
                ADV();
            }
            break;

        case ST_TAG_NAME:
            if (wv_is_space(c)) {
                state = ST_BEFORE_ATTR_NAME;
                ADV();
            } else if (c == '/') {
                state = ST_SELF_CLOSING_START;
                ADV();
            } else if (c == '>') {
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else if (c == 0) {
                TAGN_PUSH('?');
                ADV();
            } else {
                TAGN_PUSH(wv_to_lower(c));
                ADV();
            }
            break;

        case ST_BEFORE_ATTR_NAME:
            if (wv_is_space(c)) {
                ADV();
            } else if (c == '/') {
                state = ST_SELF_CLOSING_START;
                ADV();
            } else if (c == '>') {
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else {
                attr_name_len = 0;
                ATTRN_PUSH(c == '=' ? '=' : wv_to_lower(c));
                state = ST_ATTR_NAME;
                ADV();
            }
            break;

        case ST_ATTR_NAME:
            if (wv_is_space(c)) {
                state = ST_AFTER_ATTR_NAME;
                ADV();
            } else if (c == '/') {
                state = ST_SELF_CLOSING_START;
                ADV();
            } else if (c == '=') {
                state = ST_BEFORE_ATTR_VALUE;
                ADV();
            } else if (c == '>') {
                FLUSH_ATTR();
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else if (c == 0) {
                ATTRN_PUSH('?');
                ADV();
            } else {
                ATTRN_PUSH(wv_to_lower(c));
                ADV();
            }
            break;

        case ST_AFTER_ATTR_NAME:
            if (wv_is_space(c)) {
                ADV();
            } else if (c == '/') {
                FLUSH_ATTR();               /* name-only attribute complete */
                state = ST_SELF_CLOSING_START;
                ADV();
            } else if (c == '=') {
                state = ST_BEFORE_ATTR_VALUE;
                ADV();
            } else if (c == '>') {
                FLUSH_ATTR();
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else {
                /* WHATWG: start a new attribute with this byte. */
                FLUSH_ATTR();
                ATTRN_PUSH(c == 0 ? '?' : wv_to_lower(c));
                state = ST_ATTR_NAME;
                ADV();
            }
            break;

        case ST_BEFORE_ATTR_VALUE:
            if (wv_is_space(c)) {
                ADV();
            } else if (c == '"') {
                attr_has_value = 1;
                attr_val_len = 0; attr_val_overflow = 0;
                state = ST_ATTR_VALUE_DQ;
                ADV();
            } else if (c == '\'') {
                attr_has_value = 1;
                attr_val_len = 0; attr_val_overflow = 0;
                state = ST_ATTR_VALUE_SQ;
                ADV();
            } else if (c == '>') {
                /* WHATWG: parse error; emit the tag, no value. */
                FLUSH_ATTR();
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else {
                attr_has_value = 1;
                attr_val_len = 0; attr_val_overflow = 0;
                state = ST_ATTR_VALUE_UNQ;
                /* reconsume: fall through handled by the switch on next
                 * iteration with the same byte; emulate by processing now */
                ATTRV_PUSH(c == 0 ? '?' : (char)c);
                ADV();
            }
            break;

        case ST_ATTR_VALUE_DQ:
            if (c == '"') {
                state = ST_AFTER_ATTR_VALUE;
                ADV();
            } else if (c == '&') {
                char ref[1]; size_t rl = 0; size_t save = pos;
                if (wv_try_char_ref(html, len, &pos, ref, 1, &rl)) {
                    ATTRV_PUSH(ref[0]);
                    COUNT_ADV(save, pos);       /* pos already advanced */
                } else {
                    ATTRV_PUSH('&');
                    ADV();
                }
            } else if (c == 0) {
                ATTRV_PUSH('?');
                ADV();
            } else {
                ATTRV_PUSH(c);
                ADV();
            }
            break;

        case ST_ATTR_VALUE_SQ:
            if (c == '\'') {
                state = ST_AFTER_ATTR_VALUE;
                ADV();
            } else if (c == '&') {
                char ref[1]; size_t rl = 0; size_t save = pos;
                if (wv_try_char_ref(html, len, &pos, ref, 1, &rl)) {
                    ATTRV_PUSH(ref[0]);
                    COUNT_ADV(save, pos);       /* pos already advanced */
                } else {
                    ATTRV_PUSH('&');
                    ADV();
                }
            } else if (c == 0) {
                ATTRV_PUSH('?');
                ADV();
            } else {
                ATTRV_PUSH(c);
                ADV();
            }
            break;

        case ST_ATTR_VALUE_UNQ:
            if (wv_is_space(c)) {
                state = ST_AFTER_ATTR_VALUE;
                ADV();
            } else if (c == '>') {
                FLUSH_ATTR();
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else if (c == '&') {
                char ref[1]; size_t rl = 0; size_t save = pos;
                if (wv_try_char_ref(html, len, &pos, ref, 1, &rl)) {
                    ATTRV_PUSH(ref[0]);
                    COUNT_ADV(save, pos);       /* pos already advanced */
                } else {
                    ATTRV_PUSH('&');
                    ADV();
                }
            } else if (c == 0) {
                ATTRV_PUSH('?');
                ADV();
            } else {
                ATTRV_PUSH(c);
                ADV();
            }
            break;

        case ST_AFTER_ATTR_VALUE:
            if (wv_is_space(c)) {
                FLUSH_ATTR();               /* attribute is complete */
                state = ST_BEFORE_ATTR_NAME;
                ADV();
            } else if (c == '/') {
                FLUSH_ATTR();               /* attribute is complete */
                state = ST_SELF_CLOSING_START;
                ADV();
            } else if (c == '>') {
                FLUSH_ATTR();
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else {
                /* WHATWG: parse error, reconsume in before-attr-name. */
                FLUSH_ATTR();
                state = ST_BEFORE_ATTR_NAME;
            }
            break;

        case ST_SELF_CLOSING_START:
            if (c == '>') {
                self_closing = 1;
                FLUSH_ATTR();               /* anything still pending */
                EMIT_TAG();
                state = ST_TEXT;
                ADV();
            } else {
                /* WHATWG: parse error, reconsume in before-attr-name. */
                FLUSH_ATTR();
                state = ST_BEFORE_ATTR_NAME;
            }
            break;

        case ST_MARKUP_DECL:
            if (c == '-' && pos + 1 < len && html[pos + 1] == '-') {
                state = ST_COMMENT;
                comment_len = 0; comment_overflow = 0;
                ADV_N(2);
            } else if (wv_eq_ci(html, pos, len, "doctype")) {
                state = ST_DOCTYPE;
                comment_len = 0; comment_overflow = 0;
                ADV_N(7);
            } else if (c == '[' && pos + 6 < len &&
                       html[pos + 1] == 'C' && html[pos + 2] == 'D' &&
                       html[pos + 3] == 'A' && html[pos + 4] == 'T' &&
                       html[pos + 5] == 'A' && html[pos + 6] == '[') {
                state = ST_CDATA;
                comment_len = 0; comment_overflow = 0;
                ADV_N(7);
            } else {
                state = ST_BOGUS_COMMENT;
                comment_len = 0; comment_overflow = 0;
            }
            break;

        case ST_COMMENT:
            if (c == '-' && pos + 2 < len && html[pos + 1] == '-' &&
                html[pos + 2] == '>') {
                EMIT_RAW(WV_T_COMMENT);
                state = ST_TEXT;
                ADV_N(3);
            } else if (c == 0) {
                CMNT_PUSH('?');
                ADV();
            } else {
                CMNT_PUSH(c);
                ADV();
            }
            break;

        case ST_BOGUS_COMMENT:
            if (c == '>') {
                EMIT_RAW(WV_T_COMMENT);
                state = ST_TEXT;
                ADV();
            } else if (c == 0) {
                CMNT_PUSH('?');
                ADV();
            } else {
                CMNT_PUSH(c);
                ADV();
            }
            break;

        case ST_DOCTYPE:
            if (c == '>') {
                EMIT_RAW(WV_T_DOCTYPE);
                state = ST_TEXT;
                ADV();
            } else if (c == 0) {
                CMNT_PUSH('?');
                ADV();
            } else {
                CMNT_PUSH(c);
                ADV();
            }
            break;

        case ST_CDATA:
            if (c == ']' && pos + 2 < len && html[pos + 1] == ']' &&
                html[pos + 2] == '>') {
                EMIT_RAW(WV_T_CDATA);
                state = ST_TEXT;
                ADV_N(3);
            } else if (c == 0) {
                CMNT_PUSH('?');
                ADV();
            } else {
                CMNT_PUSH(c);
                ADV();
            }
            break;
        }
    }

    /* ---- EOF: flush whatever state we are in, then emit WV_T_EOF ---- */
    switch (state) {
    case ST_TEXT:
        FLUSH_TEXT();
        break;
    case ST_TAG_OPEN:
        /* WHATWG: emit '<' as text. */
        FLUSH_TEXT();
        text[0] = '<'; text_len = 1; tok_line = line; tok_col = col;
        FLUSH_TEXT();
        break;
    case ST_END_TAG_OPEN:
        /* WHATWG: emit "</" as text. */
        FLUSH_TEXT();
        text[0] = '<'; text[1] = '/'; text_len = 2;
        tok_line = line; tok_col = col;
        FLUSH_TEXT();
        break;
    case ST_TAG_NAME:
    case ST_BEFORE_ATTR_NAME:
    case ST_ATTR_NAME:
    case ST_AFTER_ATTR_NAME:
    case ST_BEFORE_ATTR_VALUE:
    case ST_ATTR_VALUE_DQ:
    case ST_ATTR_VALUE_SQ:
    case ST_ATTR_VALUE_UNQ:
    case ST_AFTER_ATTR_VALUE:
    case ST_SELF_CLOSING_START:
        FLUSH_ATTR();
        EMIT_TAG();
        break;
    case ST_COMMENT:
    case ST_BOGUS_COMMENT:
        EMIT_RAW(WV_T_COMMENT);
        break;
    case ST_DOCTYPE:
        EMIT_RAW(WV_T_DOCTYPE);
        break;
    case ST_CDATA:
        EMIT_RAW(WV_T_CDATA);
        break;
    default:
        break;
    }

    wv_push_token(a, WV_T_EOF, 0, 0, 0, 0, 0, 0, 0, line, col);
    return (int)a->tok_count;
}
