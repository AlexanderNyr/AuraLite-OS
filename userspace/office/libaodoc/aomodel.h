/* userspace/office/libaodoc/aomodel.h — the document model (OFFICE_PLAN.md OF2).
 *
 * OF0 shipped a document HANDLE and a dirty-rect canvas; OF1 shipped text.  This
 * is the thing a word processor actually edits: paragraphs of styled runs, in
 * one flat structure with index links and no pointers, so a document can be
 * serialised, copied and validated by walking two arrays.
 *
 * The shape, and why:
 *
 *   text[0 .. text_len)          every paragraph's UTF-8, concatenated, in order
 *   paras[0 .. para_len)         slices of `text`, in order, contiguous, covering
 *                                all of it; a paragraph's text carries the '\n'
 *                                that ENDS it, except for the last one
 *   runs[0 .. run_len)           slices of `text` grouped by paragraph: a
 *                                paragraph's runs are ordered, contiguous, and
 *                                cover the paragraph exactly
 *
 * Two consequences worth stating, because they are the invariants the fuzz gate
 * re-checks after every operation:
 *
 *   - a POSITION is a byte offset into `text`, so a caret, a selection and a
 *     search hit are all one integer.  Cyrillic costs two bytes, which is why
 *     the model must never step a position by one and expect a character --
 *     ao_model_next_cp() is how you move.
 *   - the canonical form forbids two ADJACENT runs with identical attributes,
 *     and forbids empty runs inside a paragraph.  Without that, "make this bold"
 *     run twice produces a different (and unequal) document than once, and every
 *     save would churn.
 *
 * The model never allocates: the caller lends it three buffers (text, runs,
 * paragraphs) and every operation reports AO_E_FULL rather than corrupting or
 * aborting.  A document that does not fit is a message, not a crash -- the same
 * rule OF1 applied to fonts and OF0 to arenas.
 */

#ifndef AURALITE_OFFICE_AOMODEL_H
#define AURALITE_OFFICE_AOMODEL_H

#include <stddef.h>
#include <stdint.h>

#include "aodoc.h"   /* AO_STYLE_*, ao_utf8_* */

/* Caps.  They are the caller's buffers, so these are the DEFAULTS the apps
 * use, not limits of the code: 64 KiB of UTF-8 is about 30 000 Russian
 * characters (two bytes each), which is a long letter. */
#define AO_MODEL_TEXT_DEFAULT  (64u * 1024u)
#define AO_MODEL_RUNS_DEFAULT  4096u
#define AO_MODEL_PARAS_DEFAULT 2048u

/* Run attributes.  `style` is a BITMASK so "bold italic" is one run, not two;
 * underline lives in the same byte because there is no reason to split it. */
/* AO_STYLE_BOLD / _ITALIC / _UNDERLINE / _OFF live in aodoc.h. */

#define AO_FONT_SANS 0u
#define AO_FONT_MONO 1u

#define AO_ALIGN_LEFT    0u
#define AO_ALIGN_CENTER  1u
#define AO_ALIGN_RIGHT   2u
#define AO_ALIGN_JUSTIFY 3u

#define AO_SIZE_DEFAULT 11u

typedef struct {
    uint32_t off;        /* byte offset into the model's text pool */
    uint32_t len;        /* bytes; 0 only for an empty unit (see below) */
    uint8_t  style;      /* AO_STYLE_* bits */
    uint8_t  size;       /* px em, 6..64 */
    uint8_t  font;       /* AO_FONT_* */
    uint8_t  _pad;
} ao_run_t;

/* Paragraph styles (OF3).  The style is a paragraph property, not a run
 * property: "Heading 1" is a fact about the block, direct formatting (the
 * run bits) layers over it.  AO_PARA_PAGEBREAK is a flag bit in the same
 * byte: this paragraph starts a new page. */
#define AO_PARA_NORMAL   0u
#define AO_PARA_H1       1u
#define AO_PARA_H2       2u
#define AO_PARA_H3       3u
#define AO_PARA_QUOTE    4u
#define AO_PARA_BULLET   5u
#define AO_PARA_NUMBER   6u
#define AO_PARA_TABLE    7u   /* text is one table ROW, cells split by '\t' */
#define AO_PARA_IMAGE    8u   /* text is the image path, nothing else */
#define AO_PARA_KIND_MASK 0x0Fu
#define AO_PARA_PAGEBREAK 0x80u

typedef struct {
    uint32_t off, len;             /* slice of the text pool */
    uint32_t first_run, run_count; /* slice of the run array */
    uint8_t  align;                /* AO_ALIGN_* */
    uint8_t  style;                /* AO_PARA_* kind | AO_PARA_PAGEBREAK */
    int16_t  indent;               /* px, left indent */
    int16_t  space_before;         /* px, ish: OF3 turns these into layout */
    int16_t  space_after;
} ao_para_t;

/* Status codes: 0 is success, everything else is a receipt. */
#define AO_OK          0
#define AO_E_ARG      -1   /* NULL model, buffer or nonsensical index */
#define AO_E_FULL     -2   /* the caller's buffer is too small */
#define AO_E_RANGE    -3   /* position outside the document */
#define AO_E_FORMAT   -4   /* a serialised document that does not parse */
#define AO_E_UTF8     -5   /* text that is not valid UTF-8 */

typedef struct {
    char      *text;
    uint32_t   text_len, text_cap;
    ao_run_t  *runs;
    uint32_t   run_len, run_cap;
    ao_para_t *paras;
    uint32_t   para_len, para_cap;
    uint32_t   rev;              /* bumped by every mutation */
} ao_model_t;

/* Bind the model to caller-owned memory and reset it to one empty paragraph
 * with one empty run (a document is never "no paragraphs": a caret has to have
 * somewhere to be). */
int  ao_model_init(ao_model_t *m, char *text, size_t text_cap,
                   ao_run_t *runs, size_t run_cap,
                   ao_para_t *paras, size_t para_cap);
void ao_model_clear(ao_model_t *m);

/* ------------------------------------------------------------- invariants */

/* Check every structural rule.  On failure returns 0 and, when `why` is not
 * NULL, stores a static string naming the first broken rule.  The fuzz gate
 * calls this after every operation. */
int  ao_model_valid(const ao_model_t *m, const char **why);

/* ------------------------------------------------------------- inspection */

uint32_t ao_model_text_len(const ao_model_t *m);
uint32_t ao_model_para_count(const ao_model_t *m);
uint32_t ao_model_run_count(const ao_model_t *m);

/* Paragraph index containing byte `pos` (a position ON a paragraph boundary
 * belongs to the paragraph that STARTS there). */
uint32_t ao_model_para_of(const ao_model_t *m, uint32_t pos);
uint32_t ao_model_para_start(const ao_model_t *m, uint32_t pi);
/* Index of the run containing `pos` within its paragraph. */
uint32_t ao_model_run_of(const ao_model_t *m, uint32_t pos);

/* Attributes in force at `pos` -- what "type here" inherits. */
uint8_t  ao_model_style_at(const ao_model_t *m, uint32_t pos);
uint8_t  ao_model_size_at(const ao_model_t *m, uint32_t pos);
uint8_t  ao_model_font_at(const ao_model_t *m, uint32_t pos);

/* Character stepping.  next_cp() returns the position after the codepoint that
 * starts at `pos` (or `pos` at the end); prev_cp() the position of the
 * codepoint that ends at `pos`.  Both refuse to split a codepoint, so a caret
 * never lands in the middle of a Cyrillic letter. */
uint32_t ao_model_next_cp(const ao_model_t *m, uint32_t pos);
uint32_t ao_model_prev_cp(const ao_model_t *m, uint32_t pos);

/* ---------------------------------------------------------------- editing */

/* Insert UTF-8 at `pos`.  Newlines split paragraphs; the new text takes the
 * attributes in force at `pos`.  `bytes` must be valid UTF-8 (AO_E_UTF8
 * otherwise: the model is not a byte bag). */
int ao_model_insert(ao_model_t *m, uint32_t pos, const char *bytes, uint32_t n);

/* Delete n bytes starting at `pos`.  Deleting a newline joins the paragraphs;
 * deleting the last character of a paragraph that is not empty leaves an empty
 * run, not an empty paragraph.  `n` is clamped to the end of the document. */
int ao_model_delete(ao_model_t *m, uint32_t pos, uint32_t n);

/* Set or clear style bits over [pos, pos+n).  Boundaries are split so the
 * result is canonical (adjacent equal runs are merged). */
int ao_model_set_style(ao_model_t *m, uint32_t pos, uint32_t n, uint8_t bits, int on);

/* Size and font over [pos, pos+n): the same split-and-apply as the style bits,
 * kept separate because they are values, not flags.  OF3's layout reads them. */
int ao_model_set_size_font(ao_model_t *m, uint32_t pos, uint32_t n,
                           uint8_t size_px, uint8_t font);

/* Paragraph alignment for the paragraph containing `pos`. */
int ao_model_set_align(ao_model_t *m, uint32_t pos, uint8_t align);

/* Paragraph style (AO_PARA_* kind, PAGEBREAK flag preserved unless the
 * caller includes it) and left indent, for the paragraph containing `pos`. */
int     ao_model_set_para_style(ao_model_t *m, uint32_t pos, uint8_t style);
uint8_t ao_model_para_style_at(const ao_model_t *m, uint32_t pos);
int     ao_model_set_indent(ao_model_t *m, uint32_t pos, int16_t indent_px);

/* ------------------------------------------------------------------ text */

/* Copy [pos, pos+n) into out (NUL-terminated).  Returns the byte count copied
 * or a negative AO_E_*.  `cap` includes the terminator. */
int ao_model_copy_text(const ao_model_t *m, uint32_t pos, uint32_t n,
                       char *out, uint32_t cap);

/* Find `needle` from `from`.  Returns the position or -1 on no match.  ASCII
 * case folding only, and the caller is told so: Unicode folding needs tables
 * the OF1 fonts do not have yet. */
int64_t ao_model_find(const ao_model_t *m, uint32_t from, const char *needle,
                      int fold_ascii);

/* ------------------------------------------------- OF2 closure: navigation */

/* Word navigation.  A word is a maximal run of word codepoints (letters and
 * digits of any script, '_'); whitespace and punctuation are their own class
 * and are skipped as a group.  word_left returns the start of the word (or
 * class run) ending at or before `pos`; word_right returns the end of the
 * word (or class run) starting at or after `pos`.  Both step by codepoint,
 * so a Cyrillic word is one word, never a row of half-letters. */
uint32_t ao_model_word_left(const ao_model_t *m, uint32_t pos);
uint32_t ao_model_word_right(const ao_model_t *m, uint32_t pos);

/* Number of non-overlapping occurrences of `needle` (same folding rule as
 * ao_model_find).  0 for an empty needle. */
uint32_t ao_model_count(const ao_model_t *m, const char *needle, int fold_ascii);

/* Replace every non-overlapping occurrence of `needle` with `with`, scanning
 * left to right and continuing AFTER each replacement (so a replacement that
 * itself contains the needle terminates).  Returns the number replaced, or a
 * negative AO_E_*; on AO_E_FULL the replacements already made stay made and
 * the caller is told by the negative return.  An empty needle is AO_E_ARG,
 * never an infinite loop. */
int ao_model_replace_all(ao_model_t *m, const char *needle, const char *with);

/* --------------------------------------------------------- serialisation */

/* A deterministic, little-endian binary form: the same document always
 * serialises to the same bytes, which is what makes save->load->save
 * byte-identical and what the undo stack stores. */
size_t ao_model_serialized_size(const ao_model_t *m);
int    ao_model_serialize(const ao_model_t *m, void *out, size_t cap, size_t *len_out);
int    ao_model_deserialize(ao_model_t *m, const void *in, size_t len);

#endif /* AURALITE_OFFICE_AOMODEL_H */
