/* userspace/office/libaodoc/aodeck.h — the Slide deck (OFFICE_PLAN.md OF6).
 *
 * A deck is slides; a slide is an ordered list of objects; an object is a
 * text box (a Writer document laid out by the OF3 engine, the second
 * consumer that proves libaodoc is a library), a shape (rect, rounded rect,
 * ellipse, line, polyline), or an image.  Objects are painted in array order,
 * so z-order IS the array; the z-order operations just permute it.
 *
 * The same rules the rest of office holds to:
 *
 *   - the model never allocates.  A slide's objects and a text box's runs and
 *     paragraphs live in fixed buffers inside the deck; a document that does
 *     not fit is a message, not a crash.
 *   - parse refuses, and the deck is left empty.  A half-loaded deck would be
 *     worse than a message.
 *   - undo is snapshots.  Serialising the deck is already written and
 *     round-tested, so an undo entry IS a serialised deck; undo across a
 *     slide reorder is then exactly as correct as undo across a keystroke.
 *
 * The logical canvas is 960x540 points; the apps scale it (the editor at
 * roughly 0.75, the show to the framebuffer).
 */

#ifndef AURALITE_OFFICE_AODECK_H
#define AURALITE_OFFICE_AODECK_H

#include <stddef.h>
#include <stdint.h>

#include "aomodel.h"
#include "aodoc.h"

/* The logical slide, in points. */
#define AO_SLIDE_W 960
#define AO_SLIDE_H 540

/* Caps.  A slideshow is not a novel: 12 slides, 24 objects each. */
#define AO_DECK_MAX_SLIDES 12u
#define AO_DECK_MAX_OBJECTS 24u
#define AO_DECK_MAX_PTS 16u
#define AO_DECK_NOTES_MAX 512u
#define AO_DECK_TEXT_MAX 384u
#define AO_DECK_RUNS 12u
#define AO_DECK_PARAS 8u
#define AO_DECK_PATH_MAX 256u

/* Undo: a ring of serialised decks.  Entries larger than the slot are
 * refused (the caller keeps working, undo just stops growing). */
#define AO_DECK_UNDO_DEPTH 8u
#define AO_DECK_UNDO_BYTES 8192u

/* Object kinds. */
#define AO_OBJ_NONE     0u
#define AO_OBJ_TEXT     1u
#define AO_OBJ_RECT     2u
#define AO_OBJ_ROUNDED  3u
#define AO_OBJ_ELLIPSE  4u
#define AO_OBJ_LINE     5u
#define AO_OBJ_POLYLINE 6u
#define AO_OBJ_IMAGE    7u
#define AO_OBJ_FIRST    AO_OBJ_TEXT
#define AO_OBJ_LAST     AO_OBJ_IMAGE

/* Status codes: 0 success, everything else is a receipt. */
#define AO_DECK_OK       0
#define AO_DECK_E_ARG   -1
#define AO_DECK_E_FULL  -2
#define AO_DECK_E_RANGE -3
#define AO_DECK_E_FMT   -4

/* Snapping output bits for ao_snap_box(). */
#define AO_SNAP_V_GUIDE 0x1u   /* x was snapped to a vertical guide */
#define AO_SNAP_H_GUIDE 0x2u   /* y was snapped to a horizontal guide */

typedef struct {
    uint32_t kind;
    int32_t  x, y, w, h;      /* int16 would not hold off-slide drags */

    /* text objects: the Writer's model, embedded, with its own buffers */
    ao_model_t md;
    char       md_text[AO_DECK_TEXT_MAX];
    ao_run_t   md_runs[AO_DECK_RUNS];
    ao_para_t  md_paras[AO_DECK_PARAS];
    uint8_t    t_valign;       /* 0 top, 1 middle, 2 bottom */
    uint8_t    t_size;         /* pt */
    uint8_t    t_bold;         /* 1/0 */
    uint8_t    t_font;         /* AO_FONT_* */
    uint32_t   t_color;        /* 0x00RRGGBB */

    /* shapes and lines */
    uint32_t   fill;
    uint32_t   stroke;
    uint8_t    stroke_w;
    uint8_t    fill_on;
    uint8_t    stroke_on;

    /* polyline */
    uint8_t    npts;
    int16_t    pts_x[AO_DECK_MAX_PTS];
    int16_t    pts_y[AO_DECK_MAX_PTS];

    /* image */
    char       img_path[AO_DECK_PATH_MAX];
} ao_deck_obj_t;

typedef struct {
    ao_deck_obj_t objs[AO_DECK_MAX_OBJECTS];
    uint32_t      obj_len;
    char          notes[AO_DECK_NOTES_MAX];
} ao_deck_slide_t;

typedef struct {
    ao_deck_slide_t slides[AO_DECK_MAX_SLIDES];
    uint32_t        slide_len;

    /* the master slide: background, default text style, page number */
    uint32_t        master_bg;
    uint32_t        master_color;
    uint8_t         master_size;
    uint8_t         master_bold;
    uint8_t         master_page_numbers;
    uint8_t         guides_on;      /* view state; serialised with the master */

    /* snapshot undo: serialised decks, ring-ordered */
    uint32_t        undo_pos;       /* next slot to write */
    uint32_t        undo_len;       /* entries used */
    uint32_t        undo_total;     /* entries pushed ever (for receipts) */
    uint8_t         redo_len;
    char            undo_snaps[AO_DECK_UNDO_DEPTH][AO_DECK_UNDO_BYTES];
    uint32_t        undo_snap_len[AO_DECK_UNDO_DEPTH];
    char            redo_snaps[AO_DECK_UNDO_DEPTH][AO_DECK_UNDO_BYTES];
    uint32_t        redo_snap_len[AO_DECK_UNDO_DEPTH];
} ao_deck_t;

/* Parse the default title face for the demo decks: H1 at the master size. */
#define AO_DECK_TITLE_PARA AO_PARA_H1

void ao_deck_init(ao_deck_t *deck);

/* Structural ops.  All bounds-checked; failure leaves the deck unchanged. */
int ao_deck_slide_add(ao_deck_t *deck, uint32_t after, uint32_t *idx_out);
int ao_deck_slide_remove(ao_deck_t *deck, uint32_t idx);
int ao_deck_slide_dup(ao_deck_t *deck, uint32_t idx, uint32_t *idx_out);
int ao_deck_slide_move(ao_deck_t *deck, uint32_t from, uint32_t to);
int ao_deck_obj_add(ao_deck_t *deck, uint32_t slide, uint32_t kind,
                    uint32_t *idx_out);
int ao_deck_obj_remove(ao_deck_t *deck, uint32_t slide, uint32_t idx);
/* delta +1 raises toward the front (end of the array), -1 lowers. */
int ao_deck_obj_move(ao_deck_t *deck, uint32_t slide, uint32_t idx, int delta);
int ao_deck_obj_to_front(ao_deck_t *deck, uint32_t slide, uint32_t idx);
int ao_deck_obj_to_back(ao_deck_t *deck, uint32_t slide, uint32_t idx);
/* Topmost object under (x, y), or -1. */
int32_t ao_deck_obj_at(const ao_deck_t *deck, uint32_t slide,
                       int32_t x, int32_t y);

/* Undo.  ao_deck_undo_push() is called BEFORE a mutation; the push is the
 * serialised deck as it stands at the time of the call. */
void ao_deck_undo_push(ao_deck_t *deck);
int  ao_deck_undo(ao_deck_t *deck);
int  ao_deck_redo(ao_deck_t *deck);

/* Snapping arithmetic.  ao_snap_axis snaps one coordinate to the nearest
 * guide within tol; returns the (possibly snapped) value and, when snapped,
 * sets *snapped=1 and *guide_idx to the winning guide's index.
 * ao_snap_box considers a box's left/centre/right (x) and top/middle/bottom
 * (y) anchors against the two guide sets and reports AO_SNAP_V_GUIDE /
 * AO_SNAP_H_GUIDE in *flags.  Ties go to the earlier guide. */
int32_t ao_snap_axis(int32_t v, const int32_t *guides, uint32_t n, int32_t tol,
                     int32_t *snapped, uint32_t *guide_idx);
uint32_t ao_snap_box(int32_t x, int32_t y, int32_t w, int32_t h,
                     const int32_t *vguides, uint32_t nv,
                     const int32_t *hguides, uint32_t nh, int32_t tol,
                     int32_t *out_x, int32_t *out_y);

/* Serialisation.  Serialize writes into out[0..cap); on success *out_len is
 * the length (NUL not included).  Parse refuses on any inconsistency and
 * leaves the deck empty. */
int ao_deck_serialize(const ao_deck_t *deck, char *out, size_t cap,
                      size_t *out_len);
int ao_deck_parse(ao_deck_t *deck, const char *buf, size_t n);

/* Invariants, for the gate: returns AO_DECK_OK and leaves *why = NULL when
 * the deck is consistent; otherwise AO_DECK_E_FMT and *why = a static
 * string. */
int ao_deck_valid(const ao_deck_t *deck, const char **why);

/* The kind names, for receipts and the XML. */
const char *ao_deck_kind_name(uint32_t kind);
uint32_t    ao_deck_kind_from_name(const char *s);

#endif
