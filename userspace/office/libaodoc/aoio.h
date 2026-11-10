/* userspace/office/libaodoc/aoio.h — the native document format (OFFICE_PLAN OF2).
 *
 * `<aodoc version="1">` is XML, and it is XML on purpose:
 *
 *   - OF7/OF8 build the ZIP container and the OOXML writer on top of an XML
 *     writer, so the suite gets one escaper and one entity reader, tested once;
 *   - a document is then a text file a human can read, diff and repair;
 *   - and the ESCAPING is the only place where a document can silently change
 *     its meaning, so it is the only place that needs to be paranoid.
 *
 * Shape:
 *
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <aodoc version="1">
 *    <p align="left" indent="0">
 *     <r style="biu" size="11" font="sans">Привет, мир — 2026</r>
 *    </p>
 *   </aodoc>
 *
 * A paragraph's text is stored WITHOUT its trailing newline (the paragraph
 * element IS the newline), and an empty paragraph is an empty <r/>.  That keeps
 * "one paragraph per <p>" true in both directions, which is what makes
 * save->load->save byte-identical.
 *
 * Limits: this is a document format for OUR files, and it refuses rather than
 * guesses.  A file with a nesting depth over AO_IO_MAX_DEPTH, an attribute over
 * AO_IO_MAX_ATTR, an entity it does not know, a non-UTF-8 body, a version it was
 * not written by, or a size past the model's capacities is an ERROR CODE, never
 * a half-loaded document.
 */

#ifndef AURALITE_OFFICE_AOIO_H
#define AURALITE_OFFICE_AOIO_H

#include <stddef.h>
#include <stdint.h>

#include "aomodel.h"

#define AO_IO_MAX_DEPTH 16      /* element nesting we will walk */
#define AO_IO_MAX_ATTR  256     /* bytes of one attribute value */

/* An upper bound on the exported size: every byte of text can become the
 * six-byte "&quot;" (a 1:1 escape is at most 6x), plus per-run and per-paragraph
 * markup.  A caller sizes its buffer with this and never guesses. */
size_t ao_io_export_bound(const ao_model_t *m);

/* Write the document as XML.  Returns AO_OK, AO_E_FULL (buffer too small: call
 * ao_io_export_bound()), or AO_E_ARG. */
int ao_io_export(const ao_model_t *m, char *out, size_t cap, size_t *len_out);

/* Parse XML into the model, replacing its contents.  On failure the model is
 * left EMPTY (never half-loaded) and the return is AO_E_FORMAT / AO_E_FULL /
 * AO_E_UTF8.  A NULL-terminated `xml` is also accepted when `len` is 0. */
int ao_io_import(ao_model_t *m, const char *xml, size_t len);

#endif /* AURALITE_OFFICE_AOIO_H */
