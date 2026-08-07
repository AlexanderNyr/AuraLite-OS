/*
 * wv_html.h — HTML tokeniser for the AuraLite web view (WEBVIEW_PLAN W1).
 *
 * Design constraints from the plan (D3, §2):
 *   - The user stack is 64 KiB.  This tokeniser is a TABLE-DRIVEN STATE
 *     MACHINE with an explicit loop: no recursion anywhere, so a hostile
 *     document cannot overflow the stack.
 *   - Bounded memory by construction: the caller supplies fixed-size
 *     buffers (tokens, attributes, string pool).  When a limit is hit the
 *     scan continues (so input is always consumed to EOF — no hangs) but
 *     the data is dropped and `truncated` is set; the caller can diagnose
 *     or proceed with what it got.
 *   - Tolerance: every malformed input produces something usable.  All
 *     real HTML is malformed.
 *
 * The tokeniser is self-contained C11 with no libc dependencies, so the
 * same file compiles on the host (unit tests) and in AuraLite user space.
 *
 * Deliberate simplifications vs WHATWG HTML, documented so they are
 * decisions rather than accidents:
 *   - Character references are only the five named ones plus numeric
 *     (&#NN; / &#xNN;), and a reference must end in ';' to be consumed.
 *   - Names are normalised to lower case (as HTML requires); values are
 *     kept verbatim except for character references.
 *   - NUL bytes are replaced with U+FFFD, which the 8-bit font cannot
 *     represent, so the replacement is '?' (0x3F).
 *   - <!DOCTYPE ...> content and comment content are capped; CDATA is
 *     accepted anywhere (foreign-content rule relaxed).
 */

#ifndef AURALITE_WV_HTML_H
#define AURALITE_WV_HTML_H

#include <stddef.h>
#include <stdint.h>

/* ---- Limits (fixed-size buffers; exceeding one sets truncated=1) ---- */
#define WV_MAX_TOKENS       2048
#define WV_MAX_ATTRS        8192        /* total across all tokens */
#define WV_MAX_ATTR_PER_TAG 63
#define WV_MAX_NAME         127         /* tag / attribute name bytes */
#define WV_MAX_ATTR_VALUE   511         /* attribute value bytes */
#define WV_MAX_TEXT         2047        /* one text token's bytes */
#define WV_MAX_COMMENT      4095
#define WV_POOL_SIZE        65536       /* string storage for everything */

/* ---- Token kinds ---- */
typedef enum {
    WV_T_TEXT = 0,      /* character data run */
    WV_T_START,         /* <name attrs> or <name attrs/> */
    WV_T_END,           /* </name> */
    WV_T_COMMENT,       /* <!-- ... --> or bogus comment */
    WV_T_DOCTYPE,       /* <!DOCTYPE ...> (content kept, not parsed) */
    WV_T_CDATA,         /* <![CDATA[ ... ]]> */
    WV_T_EOF
} wv_tok_type;

typedef struct {
    wv_tok_type type;
    uint32_t name_off;      /* offset of tag name in the pool; 0 = none */
    uint32_t name_len;
    uint32_t text_off;      /* offset of text content in the pool; 0 = none */
    uint32_t text_len;
    uint32_t attr_base;     /* index into arena->attrs[] */
    uint32_t attr_count;
    uint32_t self_closing;  /* 1 when the tag ended with '/>' */
    uint32_t line, col;     /* 1-based position of the token start */
} wv_token_t;

typedef struct {
    uint32_t name_off;
    uint32_t name_len;
    uint32_t value_off;     /* 0 + value_len 0 = attribute without a value */
    uint32_t value_len;
} wv_attr_t;

/* The arena is the whole working state.  The caller provides the backing
 * arrays (heap, static, or — for tiny test arenas — stack). */
typedef struct {
    wv_token_t *toks;
    size_t      tok_cap;
    size_t      tok_count;
    wv_attr_t  *attrs;
    size_t      attr_cap;
    size_t      attr_count;
    char       *pool;
    size_t      pool_cap;
    size_t      pool_used;
    int         truncated;      /* 1 if any limit was hit during tokenising */
} wv_arena_t;

void wv_arena_init(wv_arena_t *a,
                   wv_token_t *toks, size_t tok_cap,
                   wv_attr_t  *attrs, size_t attr_cap,
                   char       *pool,  size_t pool_cap);

/* Tokenise html[0..len).  Returns the number of tokens (including the
 * trailing WV_T_EOF), or -1 when arena or html is NULL.  On success the
 * arena holds tok_count tokens; tokens are sorted in document order.
 * `truncated` is set when any internal limit was exceeded; the result is
 * still usable. */
int wv_html_tokenize(wv_arena_t *a, const char *html, size_t len);

/* Accessors: strings are NUL-terminated inside the pool. */
const char *wv_tok_str(const wv_arena_t *a, uint32_t off);
const wv_attr_t *wv_tok_attr(const wv_arena_t *a, const wv_token_t *t, size_t i);

#endif /* AURALITE_WV_HTML_H */
