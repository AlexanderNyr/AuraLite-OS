/* userspace/office/libaodoc/aoxlsx.h — the OOXML bridge: .xlsx (OFFICE_PLAN OF8).
 *
 * The second leg of OF8.  aodox.c established the shape and this file follows
 * it exactly: the container is aozip.c, the markup is aoxml.c, and this is
 * only the TRANSLATION between SpreadsheetML and ao_book_t.  Nothing here
 * parses bytes and nothing here touches a filesystem.
 *
 * The subset, stated once so a caller knows what a save will cost:
 *
 *   import   xl/workbook.xml sheet names, xl/worksheets/sheetN.xml cells —
 *            numbers, shared strings, inline strings, booleans, error values
 *            and formulas (<f>, imported as typed input so the engine owns
 *            the result), xl/sharedStrings.xml, and <cols> column widths.
 *   export   the same subset, plus the package parts a validator expects:
 *            [Content_Types].xml, _rels/.rels, xl/workbook.xml,
 *            xl/_rels/workbook.xml.rels, xl/styles.xml,
 *            xl/sharedStrings.xml and one worksheet part per sheet.
 *
 * The two rules from aodox.c decide every edge case here too:
 *
 *   1. REFUSAL, NOT CORRUPTION.  An unsupported construct is named in a
 *      receipt ("skipped <mergeCells>") and the import continues.  Nothing
 *      is dropped silently and nothing is invented.
 *   2. DETERMINISM.  Fixed part order, fixed relationship ids, fixed
 *      timestamps (aozip pins them at 1980-01-01).  Saving an unchanged
 *      workbook twice produces the same bytes.
 *
 * Two places where the model is narrower than the format, stated up front
 * because they are the interesting ones:
 *
 *   MERGED CELLS.  ao_book_t has no span — a cell is a value at a position,
 *      and inventing a merge is how a sheet silently changes shape.  So
 *      <mergeCells> is a receipt on import and is never emitted, exactly as
 *      w:gridSpan and w:vMerge are handled in aodox.c.
 *   NUMBER FORMATS.  ao_book_t stores what the user typed and the engine's
 *      value; it has nowhere to keep a display format.  A cell whose style
 *      resolves to a non-General numFmt therefore imports as its raw value
 *      with a receipt, rather than being quietly converted.  Export writes
 *      General throughout, which is what the model actually holds.
 *
 * Namespaces are matched on the LOCAL part of a qname, so a sheet written
 * with a default namespace and one written with the usual prefixes import
 * identically.
 *
 * ATOMICITY.  aodox.c can clear a document model, so its importer leaves the
 * model EMPTY on refusal.  A book has no such reset: cells live in the
 * caller's arena and only the caller can drop them.  ao_xlsx_import()
 * therefore commits as it parses, and a caller that needs "all or nothing"
 * resets the arena and re-creates the book when the return is not AO_OK:
 *
 *     if (ao_xlsx_import(book, ...) != AO_OK) {
 *         ao_arena_reset(arena);
 *         book = ao_book_new(arena);
 *     }
 */
#ifndef AURALITE_OFFICE_AOXLSX_H
#define AURALITE_OFFICE_AOXLSX_H

#include <stddef.h>
#include <stdint.h>

#include "aocalc.h"
#include "aozip.h"

/* ---- caps.  Every one of them is a refusal with a receipt, never a crash. */

#define AO_XLSX_MAX_SHEETS      16u    /* sheets imported or exported        */
#define AO_XLSX_MAX_SHARED      2048u  /* distinct strings interned          */
#define AO_XLSX_MAX_COLS        256u   /* column-width overrides per sheet   */
#define AO_XLSX_MAX_CELL_BYTES  1024u  /* one decoded value or formula       */
#define AO_XLSX_MAX_TEXT_BYTES  4096u  /* one decoded shared string          */
#define AO_XLSX_MAX_NOTES       16u    /* distinct "skipped <x>" receipts    */
#define AO_XLSX_MAX_PATH        160u   /* bytes of one part path             */
#define AO_XLSX_MAX_NAME        128u   /* bytes of one sheet name            */

/* A receipt.  Text is NOT NUL-terminated and is at most AO_XLSX_MAX_NOTE_BYTES
 * bytes; a nonzero return from the callback aborts the operation. */
#define AO_XLSX_MAX_NOTE_BYTES 96u
typedef int (*ao_xlsx_note_fn)(void *context, const char *text, size_t length);

/* Column widths, in hundredths of an Excel character (the unit SpreadsheetML
 * uses).  0 means "no override".  Converts to and from the grid's pixels at
 * AO_XLSX_PX_PER_CHAR — the grid's own default column is 72 px, which is
 * 9.60 characters, close enough to Excel's 8.43 to look right on screen.
 *
 * The caller owns the storage (~1 KiB), so it can live wherever it fits. */
#define AO_XLSX_PX_PER_CHAR_X100 750u   /* 7.50 px per character */

typedef struct {
    uint32_t w[AO_XLSX_MAX_COLS];
} ao_xlsx_cols_t;

/* The shared-string table.  Offsets index NUL-terminated strings packed into
 * the caller's string buffer; `overflow` counts strings seen past the table
 * (a receipt, and on export those cells fall back to inline strings so a
 * workbook is never refused for having too many distinct strings).
 *
 * The caller owns the storage (~8 KiB). */
typedef struct {
    uint32_t off[AO_XLSX_MAX_SHARED];
    uint32_t count;
    uint32_t overflow;
} ao_xlsx_strings_t;

/* ----------------------------------------------------------------- export */

/* Upper bound on the archive ao_xlsx_export() can emit for `b`: XML can grow
 * a byte to six bytes when escaped, and every cell carries fixed markup on
 * top.  A caller sizes its sink with this and never guesses. */
size_t ao_xlsx_export_bound(const ao_book_t *b);

/* Write a .xlsx package to a sink.  Strings are interned into `strbuf` when
 * one is supplied; without one they are written as inline strings, which is
 * larger but equally valid.  `cols` supplies column widths for the first
 * sheet (NULL means none). */
/* Export is not a const operation: a formula's cached value is written, and
 * reading it recalculates the book.  The archive bytes for an unchanged
 * workbook are still identical, because recalculation is a function of the
 * workbook and nothing else. */
int ao_xlsx_export(ao_book_t *b, ao_zip_write_fn write, void *context,
                   size_t *archive_size);

int ao_xlsx_export_ex(ao_book_t *b, const ao_xlsx_cols_t *cols,
                      ao_zip_write_fn write, void *context,
                      size_t *archive_size,
                      char *strbuf, size_t strbuf_cap,
                      ao_xlsx_strings_t *strings,
                      ao_xlsx_note_fn note, void *note_context,
                      uint32_t *skipped_out);

/* ----------------------------------------------------------------- import */

/* Import a .xlsx into `book`, which must be freshly created (see ATOMICITY
 * above).  Returns AO_OK, or the rule that failed.
 *
 *   scratch/scratch_cap   holds one worksheet part while it is parsed; must
 *                         be at least that part's size and at most
 *                         AO_XML_MAX_INPUT_BYTES.
 *   strbuf/strbuf_cap     holds xl/sharedStrings.xml unpacked into
 *                         NUL-terminated strings; may be NULL, in which case
 *                         a shared string cell imports as blank and is
 *                         counted in `skipped_out`.
 *   strings               the resulting index -> offset table.
 *   cols_out              optional; column widths from <cols>.
 *   note                  optional; one "skipped <name>" per distinct
 *                         unsupported construct.
 *   skipped_out           optional; total count of skipped constructs.
 *
 * A package with no xl/workbook.xml is AO_E_FORMAT, not AO_E_ARG: it is a
 * malformed package, and the distinction is the caller's error message.  A
 * plain (non-ZIP) workbook is likewise AO_E_FORMAT — this is the OOXML
 * importer; the native one is ao_book_load(). */
int ao_xlsx_import(ao_book_t *book, const void *bytes, size_t length,
                   char *scratch, size_t scratch_cap,
                   char *strbuf, size_t strbuf_cap,
                   ao_xlsx_strings_t *strings,
                   ao_xlsx_cols_t *cols_out,
                   ao_xlsx_note_fn note, void *note_context,
                   uint32_t *skipped_out);

/* The parts an import reads, exposed so a test and a diagnostic can name
 * them without hardcoding a string that would then be a second truth. */
const char *ao_xlsx_workbook_part(void);
const char *ao_xlsx_shared_part(void);

#endif /* AURALITE_OFFICE_AOXLSX_H */
