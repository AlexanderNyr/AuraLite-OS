/* w32/include/w32/w32_reloc.h — RESIDUE3_PLAN.md phase C8.
 *
 * SPDX-License-Identifier: Apache-2.0.  Written for AuraLite OS from the
 * PE/COFF specification (Microsoft, public): base relocations,
 * IMAGE_REL_BASED_DIR64 and the .reloc block grammar.
 *
 * Base relocation, shared by both loaders.
 *
 * Before C8 the same twenty lines lived twice: `userspace/apps/w32run/w32run.c`
 * (the main executable) and `w32/src/w32_module.c` (every DLL).  Both held a
 * `static pe_reloc_t relocs[16384]` and refused an image that needed more, and
 * both were subtly different about what they did with a bad entry.  This file
 * is the one implementation, and it stores nothing: the relocations are
 * applied as the directory is walked (see pe_relocations_each()).
 *
 * The cap is therefore gone rather than raised.  Audacity 3.7.5 (90932 base
 * relocations, a 236 KB .reloc section) was the measured case that made the
 * fixed buffer fatal — "w32run: too many relocations", before a single import
 * was resolved.  A raise to 131072 would have been the same bug with a bigger
 * number in it.
 *
 * What stays refused, and is *named* instead of half-applied:
 *   - a relocation type this personality does not implement (only DIR64 is;
 *     the refusal carries the type, so a HIGHLOW image says "3", not "bad");
 *   - a fixup whose 8 bytes fall outside the mapped span;
 *   - IMAGE_FILE_RELOCS_STRIPPED at a non-preferred base (an image that cannot
 *     be moved, refused rather than run at the wrong address);
 *   - a malformed relocation directory (the PE parser's own rules).
 *
 */

#ifndef AURALITE_W32_RELOC_H
#define AURALITE_W32_RELOC_H

#include <stddef.h>
#include <stdint.h>

#include "w32/w32_pe.h"

/* How many relocation types the report can count separately.  PE relocation
 * types are 4 bits, so 16 is the whole space; anything larger is counted in
 * `type_other`. */
#define W32_RELOC_TYPES 16

typedef struct {
    size_t   seen;                          /* entries the walk reached     */
    size_t   applied;                       /* DIR64 fixups written         */
    size_t   skipped;                       /* IMAGE_REL_ABSOLUTE padding   */
    size_t   by_type[W32_RELOC_TYPES];      /* applied per type             */
    size_t   type_other;                    /* applied with type >= 16      */
    uint64_t delta;                         /* actual base - preferred base */
    int      refused_type;                  /* -1 unless `why` is a type    */
    uint32_t refused_rva;                   /* the offending entry's RVA    */
    const char *why;                        /* NULL = relocated cleanly     */
} w32_reloc_report_t;

/* Apply every base relocation of `img` to the image mapped at `base`, which
 * spans `span` bytes.  Returns 0 on success, -1 on refusal with
 * report->why / refused_type / refused_rva filled in.
 *
 * `base` must already hold the headers and every section (the caller's mmap
 * and memcpy), and the relocation directory is read from the FILE image
 * (`img`), not from the mapping. */
int w32_relocate_image(const pe_image_t *img, uint8_t *base, size_t span,
                       w32_reloc_report_t *report);

/* The same walk, counting only: no mapping, no writes.  Used by the loader's
 * pre-flight when it wants the scale of an image before it commits to mapping
 * it (and by the gap ledger when it reports what an image would cost). */
int w32_recount_relocations(const pe_image_t *img, w32_reloc_report_t *report);

/* One line a human can paste into a receipt:
 *   "[w32] relocations: 90932 applied (DIR64), delta 0x7ff5e0b00000, no cap"
 * or the refusal with its type and RVA.  Returns the number of characters
 * written, like snprintf. */
int w32_reloc_describe(const w32_reloc_report_t *report, char *out, size_t n);

#endif /* AURALITE_W32_RELOC_H */
