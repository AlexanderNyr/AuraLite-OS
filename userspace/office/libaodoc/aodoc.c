/* userspace/office/libaodoc/aodoc.c — see aodoc.h for what this is for.
 *
 * No syscalls, no libc heap, no globals: every function takes the object it
 * works on.  That is what lets tests/unit/test_aodoc_canvas.c compile this
 * file against the host and grade the same code the applications run.
 */

#include "aodoc.h"

#include <string.h>

/* ------------------------------------------------------------------ arena */

static uintptr_t align8(size_t n) {
    return (uintptr_t)((n + (AO_ARENA_ALIGN - 1u)) & ~(size_t)(AO_ARENA_ALIGN - 1u));
}

void ao_arena_init(ao_arena_t *a, void *mem, size_t bytes) {
    if (!a) return;
    a->base = (uint8_t *)mem;
    a->size = mem ? bytes : 0;
    a->used = 0;
    a->high_water = 0;
    a->resets = 0;
}

void ao_arena_reset(ao_arena_t *a) {
    if (!a) return;
    a->used = 0;
    a->resets++;
}

void *ao_arena_alloc(ao_arena_t *a, size_t bytes) {
    if (!a || !a->base) return NULL;
    if (bytes == 0) return NULL;
    uintptr_t need = align8(bytes);
    if (a->used + need > a->size) return NULL;   /* refused, not fatal */
    void *p = a->base + a->used;
    a->used += (size_t)need;
    if (a->used > a->high_water) a->high_water = a->used;
    return p;
}

void *ao_arena_calloc(ao_arena_t *a, size_t count, size_t bytes) {
    if (count != 0 && bytes > (size_t)-1 / count) return NULL;   /* overflow */
    size_t total = count * bytes;
    void *p = ao_arena_alloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}

size_t ao_arena_used(const ao_arena_t *a) { return a ? a->used : 0; }
size_t ao_arena_available(const ao_arena_t *a) {
    if (!a || a->used > a->size) return 0;
    return a->size - a->used;
}

/* --------------------------------------------------------------- document */

ao_doc_t *ao_doc_new(ao_arena_t *arena, const char *title) {
    if (!arena) return NULL;
    ao_doc_t *doc = (ao_doc_t *)ao_arena_calloc(arena, 1, sizeof(*doc));
    if (!doc) return NULL;
    doc->arena = arena;
    doc->rev = 1;
    if (title) {
        size_t n = strlen(title);
        if (n >= sizeof(doc->title)) n = sizeof(doc->title) - 1;
        memcpy(doc->title, title, n);
        doc->title[n] = '\0';
    }
    return doc;
}

void ao_doc_touch(ao_doc_t *doc) {
    if (!doc) return;
    doc->rev++;
    doc->edits++;
}

/* ----------------------------------------------------------------- canvas */

void ao_canvas_init(ao_canvas_t *c, int32_t box_w, int32_t box_h) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->box_w = box_w > 0 ? box_w : 0;
    c->box_h = box_h > 0 ? box_h : 0;
}

void ao_canvas_clear(ao_canvas_t *c) {
    if (!c) return;
    c->n = 0;
    c->saturated = 0;
}

/* Clip [x,x+w) x [y,y+h) to the content box.  Returns 0 when nothing is left. */
static int clip_to_box(const ao_canvas_t *c, int32_t x, int32_t y,
                       int32_t w, int32_t h, ao_rect_t *out) {
    if (w <= 0 || h <= 0) return 0;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w;
    int64_t y1 = (int64_t)y + h;
    if (x1 > c->box_w) x1 = c->box_w;
    if (y1 > c->box_h) y1 = c->box_h;
    if (x1 <= x0 || y1 <= y0) return 0;
    out->x = x0;
    out->y = y0;
    out->w = (uint32_t)(x1 - x0);
    out->h = (uint32_t)(y1 - y0);
    return 1;
}

void ao_canvas_mark(ao_canvas_t *c, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!c) return;
    c->marks++;
    if (c->saturated) return;
    ao_rect_t r;
    if ((int32_t)w <= 0 || (int32_t)h <= 0) return;
    if (!clip_to_box(c, x, y, (int32_t)w, (int32_t)h, &r)) return;
    if (c->n < AO_CANVAS_MAX_RECTS) {
        c->r[c->n++] = r;
        return;
    }
    /* Out of slots: merge into the last one, which keeps the set bounded and
     * still covers every pixel that was marked. */
    ao_rect_t *last = &c->r[AO_CANVAS_MAX_RECTS - 1];
    int32_t lx0 = last->x < r.x ? last->x : r.x;
    int32_t ly0 = last->y < r.y ? last->y : r.y;
    int64_t lx1 = (int64_t)last->x + last->w;
    int64_t ly1 = (int64_t)last->y + last->h;
    if ((int64_t)r.x + r.w > lx1) lx1 = (int64_t)r.x + r.w;
    if ((int64_t)r.y + r.h > ly1) ly1 = (int64_t)r.y + r.h;
    last->x = lx0;
    last->y = ly0;
    last->w = (uint32_t)(lx1 - lx0);
    last->h = (uint32_t)(ly1 - ly0);
}

void ao_canvas_mark_all(ao_canvas_t *c) {
    if (!c) return;
    c->saturated = 1;
    c->n = 0;
}

int ao_rect_touches(const ao_rect_t *a, const ao_rect_t *b) {
    if (!a || !b) return 0;
    int64_t ax1 = (int64_t)a->x + a->w, ay1 = (int64_t)a->y + a->h;
    int64_t bx1 = (int64_t)b->x + b->w, by1 = (int64_t)b->y + b->h;
    /* Touching counts: adjacent rects share an edge that must be composited. */
    return !(ax1 < b->x || bx1 < a->x || ay1 < b->y || by1 < a->y);
}

static void union_into(ao_rect_t *dst, const ao_rect_t *src) {
    int32_t x0 = dst->x < src->x ? dst->x : src->x;
    int32_t y0 = dst->y < src->y ? dst->y : src->y;
    int64_t x1 = (int64_t)dst->x + dst->w;
    int64_t y1 = (int64_t)dst->y + dst->h;
    if ((int64_t)src->x + src->w > x1) x1 = (int64_t)src->x + src->w;
    if ((int64_t)src->y + src->h > y1) y1 = (int64_t)src->y + src->h;
    dst->x = x0;
    dst->y = y0;
    dst->w = (uint32_t)(x1 - x0);
    dst->h = (uint32_t)(y1 - y0);
}

int ao_canvas_plan(const ao_canvas_t *c, ao_rect_t *out, int max) {
    if (!c || !out || max <= 0) return 0;

    if (c->saturated) {
        ao_rect_t full = { 0, 0, (uint32_t)c->box_w, (uint32_t)c->box_h };
        if (full.w == 0 || full.h == 0) return 0;
        out[0] = full;
        return 1;
    }
    if (c->n <= 0) return 0;

    /* Work on a copy: the plan must not disturb the canvas being drawn. */
    ao_rect_t work[AO_CANVAS_MAX_RECTS];
    int n = c->n;
    for (int i = 0; i < n; i++) work[i] = c->r[i];

    /* Merge touching rects until stable.  O(n^2) per pass over at most 8
     * rects, and it converges in at most n-1 passes. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < n && !changed; i++) {
            for (int j = i + 1; j < n; j++) {
                if (!ao_rect_touches(&work[i], &work[j])) continue;
                union_into(&work[i], &work[j]);
                for (int k = j; k + 1 < n; k++) work[k] = work[k + 1];
                n--;
                changed = 1;
                break;
            }
        }
    }

    if (n > max) {
        /* Still too many: one union of everything.  Correct, and the caller
         * learns the cost from the ratio the budget window prints. */
        ao_rect_t all = work[0];
        for (int i = 1; i < n; i++) union_into(&all, &work[i]);
        out[0] = all;
        return 1;
    }
    for (int i = 0; i < n; i++) out[i] = work[i];
    return n;
}

/* ------------------------------------------------------------------ UTF-8 */

#define AO_FFFD 0xFFFDu

int ao_utf8_decode(const char *s, size_t len, uint32_t *cp) {
    if (cp) *cp = AO_FFFD;
    if (!s || len == 0) return AO_UTF8_E_TRUNCATED;

    const uint8_t *p = (const uint8_t *)s;
    uint8_t b0 = p[0];

    if (b0 < 0x80) {                       /* ASCII, including NUL */
        if (cp) *cp = b0;
        return 1;
    }
    if (b0 < 0xC0) return AO_UTF8_E_LEAD;  /* continuation byte in lead pos */

    /* How many bytes this lead promises, and the shortest codepoint it may
     * legitimately encode (the overlong test is a comparison, not a table). */
    size_t need;
    uint32_t min;
    uint32_t v;
    if (b0 < 0xE0)      { need = 2; min = 0x80;    v = b0 & 0x1Fu; }
    else if (b0 < 0xF0) { need = 3; min = 0x800;   v = b0 & 0x0Fu; }
    else if (b0 < 0xF5) { need = 4; min = 0x10000; v = b0 & 0x07u; }
    else                { return AO_UTF8_E_LEAD; }   /* F5..FF cannot lead:
                                                      * 5/6-byte forms are not
                                                      * UTF-8, and F5..F7 would
                                                      * only reach past U+10FFFF */

    if (len < need) return AO_UTF8_E_TRUNCATED;
    for (size_t i = 1; i < need; i++) {
        uint8_t b = p[i];
        if ((b & 0xC0u) != 0x80u) return AO_UTF8_E_TRUNCATED;
        v = (v << 6) | (b & 0x3Fu);
    }
    if (v < min) return AO_UTF8_E_OVERLONG;
    if (v >= 0xD800u && v <= 0xDFFFu) return AO_UTF8_E_SURROGATE;
    if (v > 0x10FFFFu) return AO_UTF8_E_RANGE;
    if (cp) *cp = v;
    return (int)need;
}

int ao_utf8_encode(uint32_t cp, char out[4]) {
    if (!out) return 0;
    if (cp > 0x10FFFFu) return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;   /* not a scalar value */
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

int ao_utf8_valid(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int n = ao_utf8_decode(s + i, len - i, &cp);
        if (n < 0) return 0;
        i += (size_t)n;
    }
    return 1;
}

size_t ao_utf8_count(const char *s, size_t len) {
    size_t i = 0, n = 0;
    while (i < len) {
        uint32_t cp;
        int k = ao_utf8_decode(s + i, len - i, &cp);
        i += (k > 0) ? (size_t)k : 1u;      /* resync one byte on damage */
        n++;
    }
    return n;
}

size_t ao_utf8_prev(const char *s, size_t i) {
    if (!s || i == 0) return 0;
    size_t j = i - 1;
    /* Walk back over at most three continuation bytes, and stop at a byte that
     * cannot be a continuation even if it is one (damaged text). */
    while (j > 0 && ((uint8_t)s[j] & 0xC0u) == 0x80u && (i - j) < 4) j--;
    return j;
}
