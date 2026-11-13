/* userspace/office/libaodoc/aodeck.c — the Slide deck (OFFICE_PLAN.md OF6).
 *
 * The deck is slides of objects.  Z-order is the array.  Undo is a ring of
 * serialised decks.  Parse refuses everything it does not understand and
 * leaves the document empty.  Same rules as the rest of office; see aodeck.h.
 */

#include "aodeck.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static int parse_impl(ao_deck_t *d, const char *buf, size_t n, int keep_undo);

/* ------------------------------------------------------------------ init */

static void obj_bind(ao_deck_t *d, ao_deck_obj_t *o) {
    (void)d;
    memset(o, 0, sizeof(*o));
    o->kind = AO_OBJ_NONE;
    ao_model_init(&o->md, o->md_text, sizeof o->md_text,
                  o->md_runs, AO_DECK_RUNS,
                  o->md_paras, AO_DECK_PARAS);
    o->t_valign = 0;
    o->t_size = 18;
    o->t_bold = 0;
    o->t_font = AO_FONT_SANS;
    o->t_color = 0x00101010;
    o->stroke_w = 2;
}

static void master_defaults(ao_deck_t *d) {
    d->master_bg = 0x00FFFFFF;
    d->master_color = 0x00101010;
    d->master_size = 18;
    d->master_bold = 0;
    d->master_page_numbers = 1;
    d->guides_on = 1;
}

static void obj_rebind(ao_deck_obj_t *o) {
    /* After any by-value copy of an object, md's three buffer pointers are
     * the one thing that did NOT travel: they still aim at the old slot.
     * The buffers themselves are embedded, so repointing is the whole fix. */
    o->md.text = o->md_text;
    o->md.runs = o->md_runs;
    o->md.paras = o->md_paras;
}

static void rebind_all(ao_deck_t *d) {
    for (uint32_t s = 0; s < d->slide_len; s++)
        for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
            obj_rebind(&d->slides[s].objs[i]);
}

static void doc_reset(ao_deck_t *d) {
    memset(d->slides, 0, sizeof d->slides);
    for (uint32_t s = 0; s < AO_DECK_MAX_SLIDES; s++)
        for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
            obj_bind(d, &d->slides[s].objs[i]);
    d->slide_len = 1;
    master_defaults(d);
}

void ao_deck_init(ao_deck_t *deck) {
    if (!deck) return;
    memset(deck, 0, sizeof(*deck));
    for (uint32_t s = 0; s < AO_DECK_MAX_SLIDES; s++)
        for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
            obj_bind(deck, &deck->slides[s].objs[i]);
    master_defaults(deck);
    deck->slide_len = 1;
}

/* ---------------------------------------------------------- slide ops */

int ao_deck_slide_add(ao_deck_t *deck, uint32_t after, uint32_t *idx_out) {
    if (!deck) return AO_DECK_E_ARG;
    if (deck->slide_len >= AO_DECK_MAX_SLIDES) return AO_DECK_E_FULL;
    if (after >= deck->slide_len) return AO_DECK_E_RANGE;
    uint32_t at = after + 1;
    memmove(&deck->slides[at + 1], &deck->slides[at],
            (deck->slide_len - at) * sizeof(deck->slides[0]));
    memset(&deck->slides[at], 0, sizeof(deck->slides[0]));
    for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
        obj_bind(deck, &deck->slides[at].objs[i]);
    deck->slide_len++;
    rebind_all(deck);
    if (idx_out) *idx_out = at;
    return AO_DECK_OK;
}

int ao_deck_slide_remove(ao_deck_t *deck, uint32_t idx) {
    if (!deck) return AO_DECK_E_ARG;
    if (idx >= deck->slide_len) return AO_DECK_E_RANGE;
    if (deck->slide_len <= 1) return AO_DECK_E_RANGE;   /* never zero slides */
    memmove(&deck->slides[idx], &deck->slides[idx + 1],
            (deck->slide_len - idx - 1) * sizeof(deck->slides[0]));
    memset(&deck->slides[deck->slide_len - 1], 0, sizeof(deck->slides[0]));
    for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
        obj_bind(deck, &deck->slides[deck->slide_len - 1].objs[i]);
    deck->slide_len--;
    rebind_all(deck);
    return AO_DECK_OK;
}

int ao_deck_slide_dup(ao_deck_t *deck, uint32_t idx, uint32_t *idx_out) {
    if (!deck) return AO_DECK_E_ARG;
    if (idx >= deck->slide_len) return AO_DECK_E_RANGE;
    if (deck->slide_len >= AO_DECK_MAX_SLIDES) return AO_DECK_E_FULL;
    /* A byte copy carries everything: the text model's buffers are embedded
     * in the object, so only md's three pointers go stale. */
    uint32_t at = idx + 1;
    memmove(&deck->slides[at + 1], &deck->slides[at],
            (deck->slide_len - at) * sizeof(deck->slides[0]));
    memcpy(&deck->slides[at], &deck->slides[idx], sizeof(deck->slides[0]));
    deck->slide_len++;
    rebind_all(deck);   /* AFTER slide_len grows: the copy is inside the walk */
    if (idx_out) *idx_out = at;
    return AO_DECK_OK;
}

int ao_deck_slide_move(ao_deck_t *deck, uint32_t from, uint32_t to) {
    if (!deck) return AO_DECK_E_ARG;
    if (from >= deck->slide_len || to >= deck->slide_len) return AO_DECK_E_RANGE;
    if (from == to) return AO_DECK_OK;
    /* No slide-sized temporary on the stack: ao_deck_slide_t is ~28 KB. */
    static ao_deck_slide_t tmp;
    memcpy(&tmp, &deck->slides[from], sizeof tmp);
    if (from < to) {
        memmove(&deck->slides[from], &deck->slides[from + 1],
                (to - from) * sizeof(deck->slides[0]));
    } else {
        memmove(&deck->slides[to + 1], &deck->slides[to],
                (from - to) * sizeof(deck->slides[0]));
    }
    memcpy(&deck->slides[to], &tmp, sizeof tmp);
    rebind_all(deck);
    return AO_DECK_OK;
}

/* ----------------------------------------------------------- obj ops */

static void obj_defaults(ao_deck_t *d, ao_deck_obj_t *o, uint32_t kind) {
    o->kind = kind;
    switch (kind) {
    case AO_OBJ_TEXT:
        o->x = 80;  o->y = 120; o->w = 800; o->h = 120;
        o->t_color = d->master_color;
        o->t_size = d->master_size;
        o->t_bold = d->master_bold;
        o->t_valign = 0;
        break;
    case AO_OBJ_RECT:
        o->x = 120; o->y = 200; o->w = 240; o->h = 140;
        o->fill = 0x002F60C0; o->fill_on = 1;
        o->stroke = 0x00000000; o->stroke_on = 0; o->stroke_w = 2;
        break;
    case AO_OBJ_ROUNDED:
        o->x = 120; o->y = 200; o->w = 240; o->h = 140;
        o->fill = 0x002CA05B; o->fill_on = 1;
        o->stroke = 0x00000000; o->stroke_on = 0; o->stroke_w = 2;
        break;
    case AO_OBJ_ELLIPSE:
        o->x = 340; o->y = 150; o->w = 280; o->h = 240;
        o->fill = 0x00B2543C; o->fill_on = 1;
        o->stroke = 0x00000000; o->stroke_on = 0; o->stroke_w = 2;
        break;
    case AO_OBJ_LINE:
        o->x = 120; o->y = 400; o->w = 840; o->h = 400;  /* endpoints */
        o->stroke = 0x00000000; o->stroke_on = 1; o->stroke_w = 2;
        o->fill_on = 0;
        break;
    case AO_OBJ_POLYLINE:
        o->x = 120; o->y = 180; o->w = 480; o->h = 260;
        o->stroke = 0x00101010; o->stroke_on = 1; o->stroke_w = 2;
        o->fill_on = 0; o->npts = 0;
        break;
    case AO_OBJ_IMAGE:
        o->x = 280; o->y = 140; o->w = 400; o->h = 260;
        o->fill_on = 0; o->stroke_on = 0; o->stroke_w = 2;
        o->img_path[0] = '\0';
        break;
    default:
        o->kind = AO_OBJ_NONE;
        break;
    }
}

int ao_deck_obj_add(ao_deck_t *deck, uint32_t slide, uint32_t kind,
                    uint32_t *idx_out) {
    if (!deck) return AO_DECK_E_ARG;
    if (slide >= deck->slide_len) return AO_DECK_E_RANGE;
    if (kind < AO_OBJ_FIRST || kind > AO_OBJ_LAST) return AO_DECK_E_ARG;
    ao_deck_slide_t *sd = &deck->slides[slide];
    if (sd->obj_len >= AO_DECK_MAX_OBJECTS) return AO_DECK_E_FULL;
    uint32_t at = sd->obj_len;
    obj_bind(deck, &sd->objs[at]);
    obj_defaults(deck, &sd->objs[at], kind);
    sd->obj_len = at + 1;
    if (idx_out) *idx_out = at;
    return AO_DECK_OK;
}

int ao_deck_obj_remove(ao_deck_t *deck, uint32_t slide, uint32_t idx) {
    if (!deck) return AO_DECK_E_ARG;
    if (slide >= deck->slide_len) return AO_DECK_E_RANGE;
    ao_deck_slide_t *sd = &deck->slides[slide];
    if (idx >= sd->obj_len) return AO_DECK_E_RANGE;
    memmove(&sd->objs[idx], &sd->objs[idx + 1],
            (sd->obj_len - idx - 1) * sizeof(sd->objs[0]));
    sd->obj_len--;
    obj_bind(deck, &sd->objs[sd->obj_len]);
    rebind_all(deck);
    return AO_DECK_OK;
}

int ao_deck_obj_move(ao_deck_t *deck, uint32_t slide, uint32_t idx, int delta) {
    if (!deck) return AO_DECK_E_ARG;
    if (slide >= deck->slide_len) return AO_DECK_E_RANGE;
    ao_deck_slide_t *sd = &deck->slides[slide];
    if (idx >= sd->obj_len) return AO_DECK_E_RANGE;
    int32_t to = (int32_t)idx + (delta > 0 ? 1 : -1);
    if (to < 0 || (uint32_t)to >= sd->obj_len) return AO_DECK_E_RANGE;
    /* Swap the two slots wholesale (the objects are POD) and repoint md. */
    ao_deck_obj_t a, b;
    memcpy(&a, &sd->objs[idx], sizeof a);
    memcpy(&b, &sd->objs[(uint32_t)to], sizeof b);
    memcpy(&sd->objs[(uint32_t)to], &a, sizeof a);
    memcpy(&sd->objs[idx], &b, sizeof b);
    rebind_all(deck);
    return AO_DECK_OK;
}

int ao_deck_obj_to_front(ao_deck_t *deck, uint32_t slide, uint32_t idx) {
    if (!deck) return AO_DECK_E_ARG;
    if (slide >= deck->slide_len) return AO_DECK_E_RANGE;
    ao_deck_slide_t *sd = &deck->slides[slide];
    if (idx >= sd->obj_len) return AO_DECK_E_RANGE;
    while (idx + 1 < sd->obj_len) {
        if (ao_deck_obj_move(deck, slide, idx, +1) != AO_DECK_OK) break;
        idx++;
    }
    return AO_DECK_OK;
}

int ao_deck_obj_to_back(ao_deck_t *deck, uint32_t slide, uint32_t idx) {
    if (!deck) return AO_DECK_E_ARG;
    if (slide >= deck->slide_len) return AO_DECK_E_RANGE;
    ao_deck_slide_t *sd = &deck->slides[slide];
    if (idx >= sd->obj_len) return AO_DECK_E_RANGE;
    while (idx > 0) {
        if (ao_deck_obj_move(deck, slide, idx, -1) != AO_DECK_OK) break;
        idx--;
    }
    return AO_DECK_OK;
}

static int32_t obj_hit_rect(const ao_deck_obj_t *o, int32_t x, int32_t y) {
    switch (o->kind) {
    case AO_OBJ_LINE: {
        /* distance from point to segment (x0,y0)-(w,h) */
        int32_t x0 = o->x, y0 = o->y, x1 = o->w, y1 = o->h;
        int32_t dx = x1 - x0, dy = y1 - y0;
        int32_t len2 = dx * dx + dy * dy;
        int64_t t = len2 ? ((int64_t)(x - x0) * dx + (int64_t)(y - y0) * dy) * 1000 / len2 : 0;
        if (t < 0) t = 0;
        if (t > 1000) t = 1000;
        int32_t px = x0 + (int32_t)((dx * t) / 1000);
        int32_t py = y0 + (int32_t)((dy * t) / 1000);
        int32_t ddx = x - px, ddy = y - py;
        return (ddx * ddx + ddy * ddy <= 64) ? 1 : 0;   /* 8px slop */
    }
    case AO_OBJ_POLYLINE: {
        if (o->npts < 2) return 0;
        for (uint8_t k = 0; k + 1 < o->npts; k++) {
            ao_deck_obj_t seg = *o;
            seg.x = o->pts_x[k]; seg.y = o->pts_y[k];
            seg.w = o->pts_x[k + 1]; seg.h = o->pts_y[k + 1];
            if (obj_hit_rect(&seg, x, y)) return 1;
        }
        return 0;
    }
    default:
        return (x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h)
                   ? 1 : 0;
    }
}

int32_t ao_deck_obj_at(const ao_deck_t *deck, uint32_t slide,
                       int32_t x, int32_t y) {
    if (!deck) return -1;
    if (slide >= deck->slide_len) return -1;
    const ao_deck_slide_t *sd = &deck->slides[slide];
    for (uint32_t i = sd->obj_len; i-- > 0;) {
        if (obj_hit_rect(&sd->objs[i], x, y)) return (int32_t)i;
    }
    return -1;
}

/* ---------------------------------------------------------------- undo */

static void push_snap(char snaps[][AO_DECK_UNDO_BYTES], uint32_t lens[],
                      uint32_t *pos, uint32_t *len, const char *buf,
                      uint32_t n) {
    uint32_t i = *pos;
    memcpy(snaps[i], buf, n);
    lens[i] = n;
    *pos = (i + 1u) % AO_DECK_UNDO_DEPTH;
    if (*len < AO_DECK_UNDO_DEPTH) (*len)++;
}

static void push_redo(ao_deck_t *d, const char *buf, uint32_t n) {
    if (d->redo_len < AO_DECK_UNDO_DEPTH) {
        memcpy(d->redo_snaps[d->redo_len], buf, n);
        d->redo_snap_len[d->redo_len] = n;
        d->redo_len++;
        return;
    }
    /* full: drop the oldest redo entry */
    memmove(&d->redo_snaps[0], &d->redo_snaps[1],
            (AO_DECK_UNDO_DEPTH - 1u) * AO_DECK_UNDO_BYTES);
    memmove(&d->redo_snap_len[0], &d->redo_snap_len[1],
            (AO_DECK_UNDO_DEPTH - 1u) * sizeof(d->redo_snap_len[0]));
    memcpy(d->redo_snaps[AO_DECK_UNDO_DEPTH - 1u], buf, n);
    d->redo_snap_len[AO_DECK_UNDO_DEPTH - 1u] = n;
}

void ao_deck_undo_push(ao_deck_t *d) {
    if (!d) return;
    char buf[AO_DECK_UNDO_BYTES];
    size_t n = 0;
    if (ao_deck_serialize(d, buf, sizeof buf, &n) != AO_DECK_OK)
        return;                       /* too big for a slot: undo stops growing */
    push_snap(d->undo_snaps, d->undo_snap_len, &d->undo_pos, &d->undo_len,
              buf, (uint32_t)n);
    d->undo_total++;
    d->redo_len = 0;                  /* a new edit forks the timeline */
}

int ao_deck_undo(ao_deck_t *d) {
    if (!d || d->undo_len == 0) return 0;
    d->undo_pos = (d->undo_pos + AO_DECK_UNDO_DEPTH - 1u) % AO_DECK_UNDO_DEPTH;
    uint32_t idx = d->undo_pos;
    char cur[AO_DECK_UNDO_BYTES];
    size_t cur_n = 0;
    if (ao_deck_serialize(d, cur, sizeof cur, &cur_n) == AO_DECK_OK)
        push_redo(d, cur, (uint32_t)cur_n);
    if (parse_impl(d, d->undo_snaps[idx], d->undo_snap_len[idx],
                   /*keep_undo=*/1) != AO_DECK_OK) {
        d->undo_len--;
        return 0;
    }
    d->undo_len--;
    return 1;
}

int ao_deck_redo(ao_deck_t *d) {
    if (!d || d->redo_len == 0) return 0;
    uint32_t idx = (uint32_t)d->redo_len - 1u;
    char cur[AO_DECK_UNDO_BYTES];
    size_t cur_n = 0;
    if (ao_deck_serialize(d, cur, sizeof cur, &cur_n) == AO_DECK_OK)
        push_snap(d->undo_snaps, d->undo_snap_len, &d->undo_pos, &d->undo_len,
                  cur, (uint32_t)cur_n);
    if (parse_impl(d, d->redo_snaps[idx], d->redo_snap_len[idx],
                   /*keep_undo=*/1) != AO_DECK_OK)
        return 0;
    d->redo_len--;
    return 1;
}

/* ---------------------------------------------------------------- snap */

int32_t ao_snap_axis(int32_t v, const int32_t *guides, uint32_t n, int32_t tol,
                     int32_t *snapped, uint32_t *guide_idx) {
    if (snapped) *snapped = 0;
    if (guide_idx) *guide_idx = 0;
    if (!guides || n == 0 || tol < 0) return v;
    int32_t best = v;
    uint32_t best_i = n, best_d = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        int64_t dd = (int64_t)v - guides[i];
        if (dd < 0) dd = -dd;
        uint32_t d = (uint32_t)dd;
        if (d > (uint32_t)tol) continue;
        if (d < best_d) { best_d = d; best = guides[i]; best_i = i; }
    }
    if (best_i == n) return v;
    if (snapped) *snapped = 1;
    if (guide_idx) *guide_idx = best_i;
    return best;
}

uint32_t ao_snap_box(int32_t x, int32_t y, int32_t w, int32_t h,
                     const int32_t *vguides, uint32_t nv,
                     const int32_t *hguides, uint32_t nh, int32_t tol,
                     int32_t *out_x, int32_t *out_y) {
    uint32_t flags = 0;
    int32_t outx = x, outy = y;
    int32_t xc[3] = { x, x + w / 2, x + w };
    int32_t yc[3] = { y, y + h / 2, y + h };

    int32_t best_off_x = 0; uint32_t best_i_x = nv, best_d_x = 0xFFFFFFFFu;
    for (uint32_t c = 0; c < 3; c++) {
        int32_t snapped = 0; uint32_t gi = 0;
        int32_t sv = ao_snap_axis(xc[c], vguides, nv, tol, &snapped, &gi);
        if (!snapped) continue;
        int32_t off = sv - xc[c];
        uint32_t dd = off < 0 ? (uint32_t)-off : (uint32_t)off;
        if (dd < best_d_x) { best_d_x = dd; best_i_x = gi; best_off_x = off; }
    }
    int32_t best_off_y = 0; uint32_t best_i_y = nh, best_d_y = 0xFFFFFFFFu;
    for (uint32_t c = 0; c < 3; c++) {
        int32_t snapped = 0; uint32_t gi = 0;
        int32_t sv = ao_snap_axis(yc[c], hguides, nh, tol, &snapped, &gi);
        if (!snapped) continue;
        int32_t off = sv - yc[c];
        uint32_t dd = off < 0 ? (uint32_t)-off : (uint32_t)off;
        if (dd < best_d_y) { best_d_y = dd; best_i_y = gi; best_off_y = off; }
    }
    if (best_i_x != nv) { flags |= AO_SNAP_V_GUIDE; outx += best_off_x; }
    if (best_i_y != nh) { flags |= AO_SNAP_H_GUIDE; outy += best_off_y; }
    if (out_x) *out_x = outx;
    if (out_y) *out_y = outy;
    return flags;
}

/* ------------------------------------------------------- serialisation */

typedef struct { char *out; size_t cap, n; int full; } wr_t;

static void wr_raw(wr_t *w, const char *s, size_t len) {
    if (w->full) return;
    if (w->n + len >= w->cap) { w->full = 1; return; }
    memcpy(w->out + w->n, s, len);
    w->n += len;
}

static void wr_str(wr_t *w, const char *s) { wr_raw(w, s, strlen(s)); }

static void wr_f(wr_t *w, const char *fmt, ...) {
    char tmp[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) wr_raw(w, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* Escape &, <, > (text) and &, <, >, " (attributes). */
static void wr_esc_text(wr_t *w, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
        case '&':  wr_str(w, "&amp;");  break;
        case '<':  wr_str(w, "&lt;");   break;
        case '>':  wr_str(w, "&gt;");   break;
        default:   wr_raw(w, s + i, 1); break;
        }
    }
}

static void wr_esc_attr(wr_t *w, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&':  wr_str(w, "&amp;");  break;
        case '<':  wr_str(w, "&lt;");   break;
        case '>':  wr_str(w, "&gt;");   break;
        case '"':  wr_str(w, "&quot;"); break;
        default:   wr_raw(w, s, 1);     break;
        }
    }
}

static void wr_obj_common(wr_t *w, const ao_deck_obj_t *o) {
    wr_f(w, " x=\"%d\" y=\"%d\" w=\"%d\" h=\"%d\"", o->x, o->y, o->w, o->h);
}

static void wr_fill_stroke(wr_t *w, const ao_deck_obj_t *o) {
    wr_f(w, " fill=\"#%06X\" stroke=\"#%06X\" sw=\"%u\" fill-on=\"%u\" stroke-on=\"%u\"",
         o->fill & 0xFFFFFFu, o->stroke & 0xFFFFFFu,
         (unsigned)o->stroke_w, (unsigned)o->fill_on, (unsigned)o->stroke_on);
}

const char *ao_deck_kind_name(uint32_t kind) {
    switch (kind) {
    case AO_OBJ_NONE:     return "none";
    case AO_OBJ_TEXT:     return "text";
    case AO_OBJ_RECT:     return "rect";
    case AO_OBJ_ROUNDED:  return "rounded";
    case AO_OBJ_ELLIPSE:  return "ellipse";
    case AO_OBJ_LINE:     return "line";
    case AO_OBJ_POLYLINE: return "polyline";
    case AO_OBJ_IMAGE:    return "image";
    default:              return "none";
    }
}

uint32_t ao_deck_kind_from_name(const char *s) {
    if (!s) return AO_OBJ_NONE;
    for (uint32_t k = AO_OBJ_FIRST; k <= AO_OBJ_LAST; k++)
        if (!strcmp(s, ao_deck_kind_name(k))) return k;
    return AO_OBJ_NONE;
}

int ao_deck_serialize(const ao_deck_t *deck, char *out, size_t cap,
                      size_t *out_len) {
    if (!deck || !out || cap < 2) return AO_DECK_E_ARG;
    wr_t w = { out, cap, 0, 0 };

    wr_str(&w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    wr_str(&w, "<aodeck version=\"1\">\n");
    wr_f(&w, "  <master bg=\"#%06X\" color=\"#%06X\" size=\"%u\" bold=\"%u\" page-numbers=\"%u\" guides=\"%u\"/>\n",
         deck->master_bg & 0xFFFFFFu, deck->master_color & 0xFFFFFFu,
         (unsigned)deck->master_size, (unsigned)deck->master_bold,
         (unsigned)deck->master_page_numbers, (unsigned)deck->guides_on);

    for (uint32_t si = 0; si < deck->slide_len; si++) {
        const ao_deck_slide_t *sd = &deck->slides[si];
        wr_str(&w, "  <slide>\n");
        if (sd->notes[0]) {
            wr_str(&w, "    <notes>");
            wr_esc_text(&w, sd->notes, strlen(sd->notes));
            wr_str(&w, "</notes>\n");
        }
        for (uint32_t i = 0; i < sd->obj_len; i++) {
            const ao_deck_obj_t *o = &sd->objs[i];
            if (o->kind == AO_OBJ_NONE) continue;
            wr_f(&w, "    <obj type=\"%s\"", ao_deck_kind_name(o->kind));
            wr_obj_common(&w, o);
            switch (o->kind) {
            case AO_OBJ_TEXT:
                wr_f(&w, " size=\"%u\" bold=\"%u\" valign=\"%u\" color=\"#%06X\" font=\"%u\"",
                     (unsigned)o->t_size, (unsigned)o->t_bold,
                     (unsigned)o->t_valign, o->t_color & 0xFFFFFFu,
                     (unsigned)o->t_font);
                wr_str(&w, ">\n");
                {
                    /* one <p> per paragraph: style, align, indent, spacing */
                    uint32_t pc = ao_model_para_count(&o->md);
                    for (uint32_t pi = 0; pi < pc; pi++) {
                        uint32_t ps = ao_model_para_start(&o->md, pi);
                        uint32_t pe = (pi + 1 < pc) ? ao_model_para_start(&o->md, pi + 1)
                                                    : o->md.text_len;
                        /* the model's paragraph carries its trailing '\n' */
                        uint32_t plen = pe - ps;
                        if (plen && o->md.text[ps + plen - 1] == '\n') plen--;
                        const ao_para_t *pa = &o->md.paras[pi];
                        wr_f(&w, "      <p style=\"%u\" align=\"%u\" ind=\"%d\">",
                             (unsigned)(pa->style & AO_PARA_KIND_MASK),
                             (unsigned)pa->align, (int)pa->indent);
                        wr_esc_text(&w, o->md.text + ps, plen);
                        wr_str(&w, "</p>\n");
                    }
                }
                wr_str(&w, "    </obj>\n");
                break;
            case AO_OBJ_RECT:
            case AO_OBJ_ROUNDED:
            case AO_OBJ_ELLIPSE:
            case AO_OBJ_LINE:
                wr_fill_stroke(&w, o);
                wr_str(&w, "/>\n");
                break;
            case AO_OBJ_POLYLINE:
                wr_f(&w, " stroke=\"#%06X\" sw=\"%u\" stroke-on=\"%u\" pts=\"",
                     o->stroke & 0xFFFFFFu, (unsigned)o->stroke_w,
                     (unsigned)o->stroke_on);
                for (uint8_t k = 0; k < o->npts; k++) {
                    wr_f(&w, "%s%d,%d", k ? ";" : "", (int)o->pts_x[k], (int)o->pts_y[k]);
                }
                wr_str(&w, "\"/>\n");
                break;
            case AO_OBJ_IMAGE:
                wr_str(&w, " path=\"");
                wr_esc_attr(&w, o->img_path);
                wr_str(&w, "\"/>\n");
                break;
            default:
                break;
            }
        }
        wr_str(&w, "  </slide>\n");
    }
    wr_str(&w, "</aodeck>\n");
    if (w.full) return AO_DECK_E_FULL;
    out[w.n] = '\0';
    if (out_len) *out_len = w.n;
    return AO_DECK_OK;
}

/* -------------------------------------------------------------- parse */

typedef struct { const char *p, *end; } cur_t;

static void skipws(cur_t *c) {
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int starts_with(const cur_t *c, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)(c->end - c->p) >= n && memcmp(c->p, lit, n) == 0;
}

static int eat(cur_t *c, const char *lit) {
    skipws(c);
    if (!starts_with(c, lit)) return -1;
    c->p += strlen(lit);
    return 0;
}

#define KV_MAX 14
#define KV_VAL_MAX (AO_DECK_PATH_MAX + 32)
typedef struct { char name[24]; char value[KV_VAL_MAX]; } kv_t;

static int kv_find(const kv_t *kv, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(kv[i].name, name)) return (int)i;
    return -1;
}

static int xesc_into(char *dst, size_t cap, const char *src, size_t len) {
    /* decode &amp; &lt; &gt; &quot; entities; everything else raw */
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (src[i] == '&') {
            if (!memcmp(src + i, "&amp;", 5))  { if (o < cap-1) dst[o++] = '&';  i += 5; continue; }
            if (!memcmp(src + i, "&lt;", 4))   { if (o < cap-1) dst[o++] = '<';  i += 4; continue; }
            if (!memcmp(src + i, "&gt;", 4))   { if (o < cap-1) dst[o++] = '>';  i += 4; continue; }
            if (!memcmp(src + i, "&quot;", 6)) { if (o < cap-1) dst[o++] = '"';  i += 6; continue; }
            return -1;
        }
        if (src[i] == '<') return -1;   /* raw markup inside a text run */
        if (o < cap - 1) dst[o++] = src[i];
        i++;
    }
    dst[o] = '\0';
    return (int)o;
}

static int parse_attrs(cur_t *c, kv_t *kv, uint32_t maxk, uint32_t *nk,
                       int *self_closing) {
    uint32_t n = 0;
    *self_closing = 0;
    for (;;) {
        skipws(c);
        if (starts_with(c, "/>")) { c->p += 2; *self_closing = 1; *nk = n; return 0; }
        if (*c->p == '>')        { c->p += 1; *self_closing = 0; *nk = n; return 0; }
        if (n >= maxk) return -1;
        /* name */
        uint32_t nl = 0;
        while (c->p < c->end &&
               ((*c->p >= 'a' && *c->p <= 'z') || *c->p == '-' ||
                (*c->p >= '0' && *c->p <= '9'))) {
            if (nl >= sizeof kv[n].name - 1) return -1;
            kv[n].name[nl++] = *c->p++;
        }
        if (nl == 0) return -1;
        kv[n].name[nl] = '\0';
        if (eat(c, "=\"") != 0) return -1;
        /* value */
        const char *vstart = c->p;
        while (c->p < c->end && *c->p != '"') c->p++;
        if (c->p >= c->end) return -1;
        size_t vlen = (size_t)(c->p - vstart);
        c->p++;                          /* closing quote */
        if (kv_find(kv, n, kv[n].name) >= 0) return -1;   /* duplicate attr */
        int dl = xesc_into(kv[n].value, KV_VAL_MAX, vstart, vlen);
        if (dl < 0) return -1;
        n++;
    }
}

static int req_i32(const kv_t *kv, uint32_t n, const char *name, int32_t *out) {
    int i = kv_find(kv, n, name);
    if (i < 0) return -1;
    const char *s = kv[i].value;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!*s) return -1;
    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 0x7FFFFFFF) return -1;
    }
    if (neg) v = -v;
    *out = (int32_t)v;
    return 0;
}

static int req_u32(const kv_t *kv, uint32_t n, const char *name,
                   uint32_t *out, uint32_t max) {
    int64_t v64 = 0;
    int i = kv_find(kv, n, name);
    if (i < 0) return -1;
    const char *s = kv[i].value;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v64 = v64 * 10 + (*s - '0');
        if (v64 > max) return -1;
    }
    *out = (uint32_t)v64;
    return 0;
}

static int req_u8(const kv_t *kv, uint32_t n, const char *name,
                  uint8_t *out, uint32_t max) {
    uint32_t v;
    if (req_u32(kv, n, name, &v, max) != 0) return -1;
    *out = (uint8_t)v;
    return 0;
}

static int req_color(const kv_t *kv, uint32_t n, const char *name,
                     uint32_t *out) {
    int i = kv_find(kv, n, name);
    if (i < 0) return -1;
    const char *s = kv[i].value;
    if (s[0] != '#') return -1;
    s++;
    if (strlen(s) != 6) return -1;
    uint32_t v = 0;
    for (int k = 0; k < 6; k++) {
        char ch = s[k];
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
        else return -1;
        v = v * 16 + d;
    }
    *out = v;   /* 0x00RRGGBB */
    return 0;
}

/* Read escaped text up to the terminator element. */
static int read_until(cur_t *c, const char *term, char *out, size_t cap) {
    const char *start = c->p;
    const char *end = c->end;
    size_t term_len = strlen(term);
    for (const char *q = start; q + term_len <= end; q++) {
        if (memcmp(q, term, term_len) == 0) {
            size_t len = (size_t)(q - start);
            int dl = xesc_into(out, cap, start, len);
            if (dl < 0) return -1;
            c->p = q + term_len;
            return 0;
        }
    }
    return -1;
}

static int parse_impl(ao_deck_t *d, const char *buf, size_t n, int keep_undo) {
    if (keep_undo) {
        /* replace the document, keep the undo rings */
        memset(d->slides, 0, sizeof d->slides);
        for (uint32_t s = 0; s < AO_DECK_MAX_SLIDES; s++)
            for (uint32_t i = 0; i < AO_DECK_MAX_OBJECTS; i++)
                obj_bind(d, &d->slides[s].objs[i]);
        d->slide_len = 1;
        master_defaults(d);
    } else {
        ao_deck_init(d);
    }
    if (!buf || n == 0) goto fmt;

    cur_t c = { buf, buf + n };
    cur_t *cp = &c;

    /* optional <?xml ... ?> declaration */
    skipws(cp);
    if (starts_with(cp, "<?xml")) {
        const char *q = NULL;
        for (const char *t = cp->p; t + 1 < cp->end; t++)
            if (t[0] == '?' && t[1] == '>') { q = t; break; }
        if (!q) goto fmt;
        cp->p = q + 2;
    }
    if (eat(cp, "<aodeck") != 0) goto fmt;
    {
        uint32_t ver = 0;
        kv_t kv[KV_MAX]; uint32_t nk = 0; int selfclose = 0;
        if (parse_attrs(cp, kv, KV_MAX, &nk, &selfclose) != 0) goto fmt;
        if (selfclose) goto fmt;
        if (nk != 1 || req_u32(kv, nk, "version", &ver, 1) != 0) goto fmt;
    }

    /* the document may contain at most one master and any number of slides */
    int seen_master = 0;
    for (;;) {
        skipws(cp);
        if (starts_with(cp, "</aodeck>")) { cp->p += strlen("</aodeck>"); break; }
        if (starts_with(cp, "<master")) {
            if (seen_master) goto fmt;
            seen_master = 1;
            cp->p += strlen("<master");
            kv_t kv[KV_MAX]; uint32_t nk = 0; int selfclose = 0;
            if (parse_attrs(cp, kv, KV_MAX, &nk, &selfclose) != 0) goto fmt;
            if (!selfclose) goto fmt;
            if (nk != 6) goto fmt;
            if (req_color(kv, nk, "bg", &d->master_bg) != 0) goto fmt;
            if (req_color(kv, nk, "color", &d->master_color) != 0) goto fmt;
            if (req_u8(kv, nk, "size", &d->master_size, 64) != 0) goto fmt;
            if (req_u8(kv, nk, "bold", &d->master_bold, 1) != 0) goto fmt;
            if (req_u8(kv, nk, "page-numbers", &d->master_page_numbers, 1) != 0) goto fmt;
            if (req_u8(kv, nk, "guides", &d->guides_on, 1) != 0) goto fmt;
            continue;
        }
        if (starts_with(cp, "<slide>")) {
            cp->p += strlen("<slide>");
            if (d->slide_len > 1 || d->slides[0].obj_len > 0 ||
                d->slides[0].notes[0]) {
                /* first <slide> fills slot 0; later ones add */
                uint32_t idx;
                if (ao_deck_slide_add(d, d->slide_len - 1, &idx) != AO_DECK_OK)
                    goto fmt;
            }
            ao_deck_slide_t *sd = &d->slides[d->slide_len - 1];
            for (;;) {
                skipws(cp);
                if (starts_with(cp, "<notes>")) {
                    cp->p += strlen("<notes>");
                    if (read_until(cp, "</notes>", sd->notes, AO_DECK_NOTES_MAX) != 0)
                        goto fmt;
                    continue;
                }
                if (starts_with(cp, "<obj")) {
                    cp->p += strlen("<obj");
                    if (sd->obj_len >= AO_DECK_MAX_OBJECTS) goto fmt;
                    kv_t kv[KV_MAX]; uint32_t nk = 0; int selfclose = 0;
                    if (parse_attrs(cp, kv, KV_MAX, &nk, &selfclose) != 0) goto fmt;
                    int ti = kv_find(kv, nk, "type");
                    if (ti < 0) goto fmt;
                    uint32_t kind = ao_deck_kind_from_name(kv[ti].value);
                    if (kind == AO_OBJ_NONE) goto fmt;
                    uint32_t oi = sd->obj_len;
                    ao_deck_obj_t *o = &sd->objs[oi];
                    obj_bind(d, o);
                    o->kind = kind;
                    /* common: x, y, w, h */
                    int32_t xv, yv, wv, hv;
                    if (req_i32(kv, nk, "x", &xv) != 0) goto fmt;
                    if (req_i32(kv, nk, "y", &yv) != 0) goto fmt;
                    if (req_i32(kv, nk, "w", &wv) != 0) goto fmt;
                    if (req_i32(kv, nk, "h", &hv) != 0) goto fmt;
                    o->x = xv; o->y = yv; o->w = wv; o->h = hv;
                    switch (kind) {
                    case AO_OBJ_TEXT: {
                        uint32_t pk = 0;         /* paragraphs written so far */
                        if (selfclose) goto fmt;   /* text objs carry content */
                        if (req_u8(kv, nk, "size", &o->t_size, 64) != 0) goto fmt;
                        if (req_u8(kv, nk, "bold", &o->t_bold, 1) != 0) goto fmt;
                        if (req_u8(kv, nk, "valign", &o->t_valign, 2) != 0) goto fmt;
                        if (req_color(kv, nk, "color", &o->t_color) != 0) goto fmt;
                        if (req_u8(kv, nk, "font", &o->t_font, 1) != 0) goto fmt;
                        if (nk != 10) goto fmt;    /* x y w h type size bold valign color font */
                        /* children: <p style align ind>text</p> ... */
                        for (;;) {
                            skipws(cp);
                            if (starts_with(cp, "</obj>")) {
                                cp->p += strlen("</obj>");
                                break;
                            }
                            if (!starts_with(cp, "<p")) goto fmt;
                            cp->p += 2;
                            kv_t pkv[KV_MAX]; uint32_t pnk = 0; int pself = 0;
                            if (parse_attrs(cp, pkv, KV_MAX, &pnk, &pself) != 0)
                                goto fmt;
                            if (pself) goto fmt;
                            if (pnk != 3) goto fmt;
                            uint32_t pstyle, palign;
                            int32_t pind;
                            if (req_u32(pkv, pnk, "style", &pstyle, 255) != 0) goto fmt;
                            if (req_u32(pkv, pnk, "align", &palign, 3) != 0) goto fmt;
                            if (req_i32(pkv, pnk, "ind", &pind) != 0) goto fmt;
                            if (pind < -4096 || pind > 4096) goto fmt;
                            char content[AO_DECK_TEXT_MAX];
                            if (read_until(cp, "</p>", content, sizeof content) != 0)
                                goto fmt;
                            uint32_t clen = (uint32_t)strlen(content);
                            uint32_t pos = ao_model_text_len(&o->md);
                            if (ao_model_insert(&o->md, pos, content, clen) != AO_OK)
                                goto fmt;
                            if (ao_model_insert(&o->md, pos + clen, "\n", 1) != AO_OK)
                                goto fmt;
                            /* style the paragraph that just finished: it is
                             * the pk-th one (the model always keeps a phantom
                             * empty paragraph after a trailing newline, so
                             * "pc - 1" would point at it, not at ours). */
                            uint32_t ps = ao_model_para_start(&o->md, pk);
                            (void)ao_model_set_para_style(&o->md, ps, (uint8_t)pstyle);
                            (void)ao_model_set_align(&o->md, ps, (uint8_t)palign);
                            (void)ao_model_set_indent(&o->md, ps, (int16_t)pind);
                            pk++;
                        }
                        /* strip the trailing newline of the last paragraph */
                        {
                            uint32_t tl = ao_model_text_len(&o->md);
                            if (tl > 0 && o->md.text[tl - 1] == '\n')
                                (void)ao_model_delete(&o->md, tl - 1, 1);
                        }
                        /* object-level typography over every run */
                        {
                            uint32_t tl = ao_model_text_len(&o->md);
                            if (tl) {
                                if (o->t_bold)
                                    (void)ao_model_set_style(&o->md, 0, tl, AO_STYLE_BOLD, 1);
                                (void)ao_model_set_size_font(&o->md, 0, tl, o->t_size, o->t_font);
                            }
                        }
                        break;
                    }
                    case AO_OBJ_RECT:
                    case AO_OBJ_ROUNDED:
                    case AO_OBJ_ELLIPSE:
                    case AO_OBJ_LINE: {
                        if (!selfclose) goto fmt;
                        uint32_t v;
                        if (req_color(kv, nk, "fill", &o->fill) != 0) goto fmt;
                        if (req_color(kv, nk, "stroke", &o->stroke) != 0) goto fmt;
                        if (req_u32(kv, nk, "sw", &v, 32) != 0) goto fmt;
                        o->stroke_w = (uint8_t)v;
                        if (req_u8(kv, nk, "fill-on", &o->fill_on, 1) != 0) goto fmt;
                        if (req_u8(kv, nk, "stroke-on", &o->stroke_on, 1) != 0) goto fmt;
                        if (nk != 10) goto fmt;    /* x y w h type fill stroke sw fill-on stroke-on */
                        if (kind == AO_OBJ_LINE) o->fill_on = 0;
                        break;
                    }
                    case AO_OBJ_POLYLINE: {
                        if (!selfclose) goto fmt;
                        uint32_t v;
                        if (req_color(kv, nk, "stroke", &o->stroke) != 0) goto fmt;
                        if (req_u32(kv, nk, "sw", &v, 32) != 0) goto fmt;
                        o->stroke_w = (uint8_t)v;
                        if (req_u8(kv, nk, "stroke-on", &o->stroke_on, 1) != 0) goto fmt;
                        int pi = kv_find(kv, nk, "pts");
                        if (pi < 0) goto fmt;
                        if (nk != 9) goto fmt;     /* x y w h type stroke sw stroke-on pts */
                        const char *s = kv[pi].value;
                        o->npts = 0;
                        while (*s) {
                            if (o->npts >= AO_DECK_MAX_PTS) goto fmt;
                            int32_t px, py;
                            if (sscanf(s, "%d,%d", &px, &py) != 2) goto fmt;
                            o->pts_x[o->npts] = (int16_t)px;
                            o->pts_y[o->npts] = (int16_t)py;
                            o->npts++;
                            const char *semi = strchr(s, ';');
                            if (!semi) break;
                            s = semi + 1;
                        }
                        break;
                    }
                    case AO_OBJ_IMAGE: {
                        if (!selfclose) goto fmt;
                        int pi = kv_find(kv, nk, "path");
                        if (pi < 0) goto fmt;
                        if (nk != 6) goto fmt;     /* x y w h type path */
                        snprintf(o->img_path, AO_DECK_PATH_MAX, "%s", kv[pi].value);
                        break;
                    }
                    default: goto fmt;
                    }
                    sd->obj_len = oi + 1;
                    continue;
                }
                if (starts_with(cp, "</slide>")) {
                    cp->p += strlen("</slide>");
                    break;
                }
                goto fmt;
            }
            continue;
        }
        goto fmt;
    }
    /* trailing garbage is a refusal */
    skipws(cp);
    if (cp->p != cp->end) goto fmt;
    return AO_DECK_OK;

fmt:
    if (keep_undo) doc_reset(d); else ao_deck_init(d);
    return AO_DECK_E_FMT;
}

int ao_deck_parse(ao_deck_t *deck, const char *buf, size_t n) {
    if (!deck) return AO_DECK_E_ARG;
    return parse_impl(deck, buf, n, /*keep_undo=*/0);
}

/* ------------------------------------------------------------- valid */

int ao_deck_valid(const ao_deck_t *deck, const char **why) {
    static const char *const w_null = "deck is NULL";
    static const char *const w_slides = "slide_len out of range";
    static const char *const w_objs = "obj_len out of range";
    static const char *const w_kind = "object kind out of range";
    static const char *const w_model = "text model is inconsistent";
    static const char *const w_size = "object has a negative size";
    static const char *const w_notes = "notes are not NUL-terminated in range";
    static const char *const w_master = "master field out of range";
    if (!deck) { if (why) *why = w_null; return AO_DECK_E_ARG; }
    if (deck->slide_len < 1 || deck->slide_len > AO_DECK_MAX_SLIDES) {
        if (why) *why = w_slides;
        return AO_DECK_E_FMT;
    }
    if (deck->master_size > 64 || deck->master_bold > 1 ||
        deck->master_page_numbers > 1 || deck->guides_on > 1) {
        if (why) *why = w_master;
        return AO_DECK_E_FMT;
    }
    for (uint32_t s = 0; s < deck->slide_len; s++) {
        const ao_deck_slide_t *sd = &deck->slides[s];
        if (sd->obj_len > AO_DECK_MAX_OBJECTS) {
            if (why) *why = w_objs;
            return AO_DECK_E_FMT;
        }
        if (memchr(sd->notes, '\0', AO_DECK_NOTES_MAX) == NULL) {
            if (why) *why = w_notes;
            return AO_DECK_E_FMT;
        }
        for (uint32_t i = 0; i < sd->obj_len; i++) {
            const ao_deck_obj_t *o = &sd->objs[i];
            if (o->kind < AO_OBJ_FIRST || o->kind > AO_OBJ_LAST) {
                if (why) *why = w_kind;
                return AO_DECK_E_FMT;
            }
            if (o->kind != AO_OBJ_LINE && o->kind != AO_OBJ_POLYLINE &&
                (o->w < 0 || o->h < 0)) {
                if (why) *why = w_size;
                return AO_DECK_E_FMT;
            }
            if (o->kind == AO_OBJ_TEXT) {
                const char *mwhy;
                if (!ao_model_valid(&o->md, &mwhy)) {
                    if (why) *why = w_model;
                    return AO_DECK_E_FMT;
                }
            }
            if (o->kind == AO_OBJ_POLYLINE && o->npts > AO_DECK_MAX_PTS) {
                if (why) *why = w_size;
                return AO_DECK_E_FMT;
            }
        }
    }
    if (why) *why = NULL;
    return AO_DECK_OK;
}
