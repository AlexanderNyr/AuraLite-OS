/* userspace/office/libaodoc/aodoc.h — the document core the suite builds on.
 *
 * OFFICE_PLAN.md OF0 asks for three things from this library before any
 * document code exists:
 *
 *   - a document handle with a PER-DOCUMENT ARENA, so a document is freed by
 *     forgetting one pointer (no malloc graph to unwind, no leak per undo step);
 *   - a DIRTY-RECT CANVAS that batches ag_window_invalidate_rect() calls and
 *     presents once per frame (D5: repaint the caret, not the window);
 *   - code small enough to be tested on the host, which is why the geometry
 *     here is pure: no syscalls, no libc allocation, no globals.
 *
 * The split that makes both testable: aodoc.c never talks to the GUI.  A
 * caller draws, calls ao_canvas_mark() for each region it touched, and then
 * ao_canvas_publish() (which lives in the application, because "publish"
 * means ag_window_invalidate_rect + ag_render_now and those are syscalls).
 * ao_canvas_plan() — the part with the rules — is pure and host-tested by
 * tests/unit/test_aodoc_canvas.c.
 */

#ifndef AURALITE_OFFICE_AODOC_H
#define AURALITE_OFFICE_AODOC_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ arena */

/* A bump allocator over caller-owned memory.  fast, forgetful, and exactly
 * right for a document's lifetimes: everything allocated for a document dies
 * with the document (or with an explicit reset, which is how a document is
 * re-laid-out after an edit that invalidates the layout). */
typedef struct {
    uint8_t *base;      /* caller-owned bytes */
    size_t   size;
    size_t   used;
    size_t   high_water; /* largest `used` ever seen: the real cost */
    uint32_t resets;
} ao_arena_t;

/* Alignment every allocation is rounded up to (8 bytes: enough for
 * uint64_t/double members, which is all the document model uses). */
/* Character style bits.  Defined ONCE, here, because both the text layer
 * (aoglyph.h) and the document model (aomodel.h) mean the same thing by them
 * and a style read out of a document is handed straight to the renderer. */
#define AO_STYLE_NONE      0x00u
#define AO_STYLE_BOLD      0x01u
#define AO_STYLE_ITALIC    0x02u
#define AO_STYLE_UNDERLINE 0x04u
#define AO_STYLE_OFF       0x00u

#define AO_ARENA_ALIGN 8u

void  ao_arena_init(ao_arena_t *a, void *mem, size_t bytes);
void  ao_arena_reset(ao_arena_t *a);
/* Returns NULL when the arena cannot satisfy the request — never aborts, so a
 * document that is too big is a receipt, not a crash. */
void *ao_arena_alloc(ao_arena_t *a, size_t bytes);
void *ao_arena_calloc(ao_arena_t *a, size_t count, size_t bytes);
size_t ao_arena_used(const ao_arena_t *a);
size_t ao_arena_available(const ao_arena_t *a);

/* --------------------------------------------------------------- document */

/* The minimum a document has to be in OF0: identified, owned, versioned.
 * Runs, paragraphs and layout arrive in OF2/OF3 on top of this handle. */
typedef struct {
    ao_arena_t *arena;
    char        title[64];
    uint32_t    rev;      /* bumped by ao_doc_touch(); the dirty check */
    uint32_t    edits;    /* counters the budget window reports */
} ao_doc_t;

ao_doc_t *ao_doc_new(ao_arena_t *arena, const char *title);
void      ao_doc_touch(ao_doc_t *doc);

/* ----------------------------------------------------------------- canvas */

/* At most this many rects are tracked between presents.  Eight is enough for
 * the shapes the suite produces per frame (caret, old caret, the line it left,
 * the line it joined, a scroll band); beyond it the canvas saturates to "the
 * whole content box", which is always correct and only ever costs a slower
 * frame. */
#define AO_CANVAS_MAX_RECTS 8

typedef struct {
    int32_t  x, y;
    uint32_t w, h;
} ao_rect_t;

typedef struct {
    int32_t  box_w, box_h;               /* the content box being painted */
    ao_rect_t r[AO_CANVAS_MAX_RECTS];
    int      n;
    int      saturated;                  /* "repaint everything" */
    uint32_t marks;                      /* ao_canvas_mark() calls, for tests */
} ao_canvas_t;

void ao_canvas_init(ao_canvas_t *c, int32_t box_w, int32_t box_h);
void ao_canvas_clear(ao_canvas_t *c);
/* Record that (x,y,w,h) changed.  Clipped to the content box; a rect that
 * clips to nothing records nothing. */
void ao_canvas_mark(ao_canvas_t *c, int32_t x, int32_t y, uint32_t w, uint32_t h);
/* Record the whole content box. */
void ao_canvas_mark_all(ao_canvas_t *c);

/* The plan for this frame: merged, clipped rects written to `out` (at most
 * `max`), returning how many.  Overlapping or touching rects are merged into
 * their bounding box until the set is stable, so a caret move inside a line
 * yields ONE rect, not two.  When the set cannot be merged below `max`, the
 * function writes the content box as a single rect and returns 1 — correctness
 * first, cost second.  A saturated canvas also yields exactly one full-box
 * rect.  Returns 0 when nothing changed (no present needed). */
int ao_canvas_plan(const ao_canvas_t *c, ao_rect_t *out, int max);

/* True when `a` and `b` overlap or touch (share an edge or a corner). */
int ao_rect_touches(const ao_rect_t *a, const ao_rect_t *b);

/* ------------------------------------------------------------------ UTF-8 */

/* Text is stored and edited as UTF-8 and nowhere else.  The reason is the one
 * that decides the whole font question: a Russian document written in CP1251
 * is a document that cannot be pasted into an OOXML file (OF8) or searched
 * codepoint-by-codepoint (OF2) without a conversion step that loses data.
 *
 * The decoders follow the Unicode conformance rules and REJECT what the spec
 * says to reject — a decoder that silently accepts an overlong form is how
 * "the same text" becomes two byte strings that compare unequal:
 *
 *   - overlong encodings        (C0 80 for NUL, E0 80 80 for a 3-byte zero)
 *   - UTF-16 surrogate halves   (ED A0 80 .. ED BF BF), not scalar values
 *   - anything above U+10FFFF    (F4 90 80 80)
 *   - truncated tails            (a lead byte with too few continuations)
 *   - stray continuation bytes   (80..BF where a lead byte belongs)
 *
 * A caller that hits an error is expected to advance ONE byte and try again:
 * that is how a damaged document still renders everything after the damage,
 * which is the "refusal, not corruption" rule applied to text.
 */
#define AO_UTF8_OK            0
#define AO_UTF8_E_TRUNCATED  -1   /* lead byte, not enough continuation bytes */
#define AO_UTF8_E_OVERLONG   -2   /* shortest form is shorter */
#define AO_UTF8_E_SURROGATE  -3   /* U+D800..U+DFFF is not a scalar value */
#define AO_UTF8_E_RANGE      -4   /* above U+10FFFF */
#define AO_UTF8_E_LEAD       -5   /* stray continuation or invalid lead byte */

/* Decode one codepoint from `s` (at most `len` bytes).  On success returns the
 * number of bytes consumed (1..4) and stores the codepoint in *cp.  On failure
 * returns one of AO_UTF8_E_* and leaves *cp at U+FFFD, the replacement
 * character, so a caller that ignores the return value still draws something
 * honest rather than a stale codepoint. */
int      ao_utf8_decode(const char *s, size_t len, uint32_t *cp);
/* Encode `cp`; writes 1..4 bytes into out (which must hold 4) and returns the
 * count, or 0 when `cp` is not a scalar value (surrogate, > U+10FFFF). */
int      ao_utf8_encode(uint32_t cp, char out[4]);
/* 1 when the whole of s[0..len) is valid UTF-8, 0 otherwise. */
int      ao_utf8_valid(const char *s, size_t len);
/* Codepoints in s[0..len); an invalid byte counts as one (a replacement). */
size_t   ao_utf8_count(const char *s, size_t len);
/* The start of the codepoint before byte offset `i` (for Backspace).  Returns
 * 0 at the start of the string and never walks past a lead byte, so deleting
 * backwards ALWAYS removes exactly one codepoint — a byte-wise Backspace over
 * UTF-8 is how "П" turns into two replacement characters. */
size_t   ao_utf8_prev(const char *s, size_t i);

#endif /* AURALITE_OFFICE_AODOC_H */
