/* userspace/office/libaodoc/aodox.h — the OOXML bridge: .docx (OFFICE_PLAN OF8).
 *
 * OF8 exists because a suite that cannot open a file the rest of the world
 * sent is a toy.  OF7 already paid for the two hard parts — a bounded ZIP
 * reader/writer and a strict pull XML parser — so this file is only the
 * TRANSLATION between OOXML's shape and our model's shape.  Nothing here
 * parses bytes: aozip.c does the container, aoxml.c does the markup.
 *
 * The subset, stated once so a caller knows what a save will cost:
 *
 *   import   w:p paragraphs, w:r runs (w:b / w:i / w:u / w:sz / w:rFonts),
 *            w:jc alignment, w:pStyle (Heading1-3, Quote, ListParagraph),
 *            w:numPr lists (bullet vs decimal), w:tbl tables, inline images
 *            resolved through word/_rels/document.xml.rels, w:sectPr page
 *            size and margins.
 *   export   the same subset, plus the package parts a validator expects:
 *            [Content_Types].xml, _rels/.rels, word/_rels/document.xml.rels,
 *            word/document.xml, word/styles.xml and one PNG part per image.
 *
 * Two rules decide every edge case:
 *
 *   1. REFUSAL, NOT CORRUPTION.  An unsupported construct is named in a
 *      receipt ("skipped <w:sdt>") and the import continues.  Nothing is
 *      dropped silently, and nothing is invented: a construct we do not
 *      understand is absent from the model, not approximated in it.
 *   2. DETERMINISM.  Fixed part order, fixed relationship ids, fixed
 *      timestamps (aozip pins them at 1980-01-01).  Saving an unchanged
 *      document twice produces the same bytes, which is what makes a save
 *      auditable with cmp(1).
 *
 * The library stays PURE for the same reason aodoc.c does: no syscalls, no
 * allocation, no globals.  Whatever touches the filesystem arrives as a
 * callback (ao_docx_media_fn), so every rule above is testable on the host
 * by tests/unit/test_ao_ooxml.c.
 *
 * Namespaces are matched on the LOCAL part of a qname, so a document written
 * with a default namespace ("<p>") and one written with Word's prefixes
 * ("<w:p>") import identically.  Namespace URIs are not validated: OOXML
 * reuses local names across namespaces, and the structure we walk (w:body /
 * w:p / w:r) is unambiguous without them.
 */

#ifndef AURALITE_OFFICE_AODOX_H
#define AURALITE_OFFICE_AODOX_H

#include <stddef.h>
#include <stdint.h>

#include "aomodel.h"
#include "aozip.h"

/* ---- caps.  Every one of them is a refusal with a receipt, never a crash. */

#define AO_DOCX_MAX_RELS        24u    /* relationship ids tracked per part  */
#define AO_DOCX_MAX_REL_ID      64u    /* bytes of one rId                   */
#define AO_DOCX_MAX_PATH        128u   /* bytes of one part/media path       */
#define AO_DOCX_MAX_TEXT_BYTES  4096u  /* one decoded w:t, or one table cell */
#define AO_DOCX_MAX_ROW_BYTES   4096u  /* one assembled table row            */
#define AO_DOCX_MAX_MEDIA       8u     /* images in one document             */
#define AO_DOCX_MAX_NOTES       16u    /* distinct "skipped <x>" receipts    */

/* Relationship table filled by the importer when the caller supplies one.
 * The caller owns the storage (~5 KiB), so it can live wherever it fits:
 * a static in the app, a static in a test. */
typedef struct {
    char     id[AO_DOCX_MAX_RELS][AO_DOCX_MAX_REL_ID];
    char     target[AO_DOCX_MAX_RELS][AO_DOCX_MAX_PATH];
    uint32_t count;
    uint32_t overflow;   /* relationships seen past the table: a receipt */
} ao_docx_rels_t;

/* Section properties in TWIPS (1440 per inch).  `seen` is 0 when the source
 * document had no w:sectPr at all, in which case the values are the A4
 * defaults below and a caller must not claim it read the page setup. */
#define AO_DOCX_A4_W   11906u
#define AO_DOCX_A4_H   16838u
#define AO_DOCX_MARGIN  1134u

typedef struct {
    uint32_t page_w, page_h;
    uint32_t margin_l, margin_r, margin_t, margin_b;
    uint32_t seen;
} ao_docx_sect_t;

/* A receipt.  Text is NOT NUL-terminated and is at most AO_DOCX_MAX_NOTE_BYTES
 * bytes; a nonzero return from the callback aborts the operation. */
#define AO_DOCX_MAX_NOTE_BYTES 96u
typedef int (*ao_docx_note_fn)(void *context, const char *text, size_t length);

/* Read one media file for export.  `out` == NULL asks only for the length;
 * otherwise fill out[0..*len_out) and return AO_OK.  The caller supplies it
 * because "open a file" is a syscall and this library makes none. */
typedef int (*ao_docx_media_fn)(void *context, const char *path,
                                uint8_t *out, size_t cap, size_t *len_out);

/* ----------------------------------------------------------------- export */

/* Upper bound on the archive ao_docx_export() can emit for `m`: XML can grow
 * a byte to six bytes when escaped, and every run and paragraph carries fixed
 * markup on top.  Media is NOT included — add the size of the largest image
 * yourself.  A caller sizes its sink with this and never guesses. */
size_t ao_docx_export_bound(const ao_model_t *m);

/* Write a .docx package to a sink.  Images are skipped with a receipt when no
 * media callback and buffer are supplied; the document text is never affected. */
int ao_docx_export(const ao_model_t *m, ao_zip_write_fn write, void *context,
                   size_t *archive_size);

int ao_docx_export_ex(const ao_model_t *m, ao_zip_write_fn write, void *context,
                      size_t *archive_size,
                      const ao_docx_sect_t *sect,
                      ao_docx_media_fn media, void *media_context,
                      uint8_t *media_buf, size_t media_cap,
                      ao_docx_note_fn note, void *note_context,
                      uint32_t *skipped_out);

/* ----------------------------------------------------------------- import */

/* Import a .docx into `m`, REPLACING its contents.  On any refusal the model
 * is left EMPTY — never half-loaded — and the return says which rule failed.
 *
 *   scratch/scratch_cap   holds word/document.xml (and the rels part while it
 *                         is parsed); must be at least that part's size and at
 *                         most AO_XML_MAX_INPUT_BYTES.
 *   rels                  optional; filled with the document's rId -> media
 *                         map so a caller can resolve image paths afterwards.
 *   sect_out              optional; page size and margins when w:sectPr exists.
 *   note                  optional; receives one "skipped <name>" per distinct
 *                         unsupported construct.
 *   skipped_out           optional; total count of skipped constructs.
 *
 * A .docx whose word/document.xml is missing is AO_E_FORMAT, not AO_E_ARG:
 * it is a malformed package, and the distinction is the caller's error
 * message.  A plain (non-ZIP) XML document is likewise AO_E_FORMAT — this is
 * the OOXML importer; the native one is ao_io_import(). */
int ao_docx_import(ao_model_t *m, const void *bytes, size_t length,
                   char *scratch, size_t scratch_cap,
                   ao_docx_rels_t *rels,
                   ao_docx_sect_t *sect_out,
                   ao_docx_note_fn note, void *note_context,
                   uint32_t *skipped_out);

/* The part an import reads, exposed so a test and a diagnostic can name it. */
const char *ao_docx_document_part(void);
const char *ao_docx_rels_part(void);

#endif /* AURALITE_OFFICE_AODOX_H */
