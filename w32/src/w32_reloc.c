/* w32/src/w32_reloc.c — RESIDUE3_PLAN.md phase C8.
 *
 * SPDX-License-Identifier: Apache-2.0.  Written for AuraLite OS from the
 * PE/COFF specification (Microsoft, public).
 *
 * The one base-relocation implementation both loaders call.  See
 * w32/include/w32/w32_reloc.h for why the fixed 16384-entry buffer is gone
 * rather than enlarged.
 *
 * Host-buildable: `AURALITE_W32_HOST_TEST` only changes what the surrounding
 * translation units pull in (the header already avoids the guest-only ones).
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "w32/w32_reloc.h"

#define IMAGE_FILE_RELOCS_STRIPPED 0x0001u

typedef struct {
    uint8_t            *base;
    size_t              span;
    uint64_t            delta;
    w32_reloc_report_t *rep;
    int                 stop;
} reloc_ctx_t;

static uint64_t rd64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int k = 7; k >= 0; k--) v = (v << 8) | p[k];
    return v;
}

static void wr64le(uint8_t *p, uint64_t v) {
    for (int k = 0; k < 8; k++) p[k] = (uint8_t)((v >> (k * 8)) & 0xFF);
}

static void refuse(w32_reloc_report_t *rep, reloc_ctx_t *c,
                   const char *why, int type, uint32_t rva) {
    rep->why          = why;
    rep->refused_type = type;
    rep->refused_rva  = rva;
    c->stop           = 1;
}

static int reloc_visit(void *vctx, uint32_t type, uint32_t rva) {
    reloc_ctx_t *c = vctx;
    w32_reloc_report_t *rep = c->rep;

    if (type == PE_REL_ABSOLUTE) {          /* padding: never reaches here  */
        rep->skipped++;
        return 0;
    }
    if (type != PE_REL_DIR64) {
        /* Naming the type is the whole point: a WOW64/HIGHLOW image (type 3)
         * and a HIGHADJ image (type 4) are different refusals that both used
         * to print "unsupported relocation type" with no number. */
        refuse(rep, c, "unsupported relocation type", (int)type, rva);
        return 1;
    }
    /* The fixup writes 8 bytes at rva: the whole quad must be inside the
     * mapping, checked on the RVA, not on the pointer (rva + 8 in size_t). */
    if ((uint64_t)rva + 8u > (uint64_t)c->span) {
        refuse(rep, c, "relocation outside image", (int)type, rva);
        return 1;
    }

    uint8_t *p = c->base + rva;
    wr64le(p, rd64le(p) + c->delta);
    rep->applied++;
    if (type < W32_RELOC_TYPES) rep->by_type[type]++;
    else                        rep->type_other++;
    return 0;
}

int w32_relocate_image(const pe_image_t *img, uint8_t *base, size_t span,
                       w32_reloc_report_t *report) {
    w32_reloc_report_t scratch;
    reloc_ctx_t c;
    size_t reached = 0;
    int rc;

    if (!img || !base || span == 0) return -1;
    if (!report) { memset(&scratch, 0, sizeof scratch); report = &scratch; }
    memset(report, 0, sizeof *report);
    report->refused_type = -1;

    report->delta = (uint64_t)(uintptr_t)base - img->image_base;
    if (report->delta != 0 && (img->characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
        report->why = "relocations stripped and not at its preferred base";
        return -1;
    }

    c.base = base; c.span = span; c.delta = report->delta;
    c.rep = report; c.stop = 0;

    rc = pe_relocations_each(img, reloc_visit, &c, &reached);
    report->seen = reached;
    if (rc != PE_OK) {
        report->why = "bad relocation table";
        return -1;
    }
    if (c.stop) return -1;
    return 0;
}

/* Counting walk: the same parser, the same refusal order, no writes. */
typedef struct {
    size_t   n;
    int      bad_type;
    int      refused_type;
    uint32_t refused_rva;
    int      outside;
    size_t   span;
} count_ctx_t;

static int count_visit(void *vctx, uint32_t type, uint32_t rva) {
    count_ctx_t *c = vctx;
    c->n++;
    if (type != PE_REL_DIR64) {
        c->bad_type = 1; c->refused_type = (int)type; c->refused_rva = rva;
        return 1;
    }
    if (c->span && (uint64_t)rva + 8u > (uint64_t)c->span) {
        c->outside = 1; c->refused_type = (int)type; c->refused_rva = rva;
        return 1;
    }
    return 0;
}

int w32_recount_relocations(const pe_image_t *img, w32_reloc_report_t *report) {
    w32_reloc_report_t scratch;
    count_ctx_t c;
    size_t reached = 0;
    int rc;

    if (!img) return -1;
    if (!report) { memset(&scratch, 0, sizeof scratch); report = &scratch; }
    memset(report, 0, sizeof *report);
    report->refused_type = -1;

    memset(&c, 0, sizeof c);
    c.refused_type = -1;
    rc = pe_relocations_each(img, count_visit, &c, &reached);
    report->seen = reached;
    if (rc != PE_OK) { report->why = "bad relocation table"; return -1; }
    if (c.bad_type) {
        report->why = "unsupported relocation type";
        report->refused_type = c.refused_type;
        report->refused_rva  = c.refused_rva;
        return -1;
    }
    if (c.outside) {
        report->why = "relocation outside image";
        report->refused_type = c.refused_type;
        report->refused_rva  = c.refused_rva;
        return -1;
    }
    return 0;
}

int w32_reloc_describe(const w32_reloc_report_t *report, char *out, size_t n) {
    if (!report || !out || n == 0) return 0;
    if (report->why) {
        if (report->refused_type >= 0)
            return snprintf(out, n,
                            "[w32] relocation refused: %s (type %d at RVA 0x%x)",
                            report->why, report->refused_type,
                            (unsigned)report->refused_rva);
        return snprintf(out, n, "[w32] relocation refused: %s", report->why);
    }
    return snprintf(out, n,
                    "[w32] relocations: %lu applied (DIR64), delta 0x%llx, "
                    "no fixed cap",
                    (unsigned long)report->applied,
                    (unsigned long long)report->delta);
}
