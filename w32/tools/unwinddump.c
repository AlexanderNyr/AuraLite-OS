/* unwinddump.c — dump Win64 unwind info through OUR parser, W32A-4.
 *
 * A host tool: it maps a PE32+ image (headers + sections at their VAs),
 * registers it with the w32 SEH image table, and walks .pdata the way the
 * fault dispatcher does — w32_seh_lookup per function, w32_seh_validate_chain
 * per UNWIND_INFO.  Its output is deliberately flat and stable, so the
 * equivalence gate (tests/unit/test_w32_a4equiv.py) can byte-compare it
 * against `llvm-readobj --unwind`.
 *
 * What this proves: our parser finds the same functions, reads the same
 * UNWIND_INFO headers/codes/handlers, and refuses nothing readobj accepts.
 * What it does NOT prove (covered elsewhere): seh_apply_codes' arithmetic
 * (the QEMU fixtures execute real unwinds) and scope-table semantics
 * (tests/unit/test_w32_a4.c, which readobj cannot see at all).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "w32/w32_pe.h"
#include "w32/w32_seh.h"

/* Data-directory slot of the exception table (no named constant exists). */
#define PE_DIR_EXCEPTION 3u

int main(int argc, char **argv) {
    FILE *f;
    long sz;
    uint8_t *file = NULL;
    pe_image_t img;
    uint8_t *image = NULL;
    uint32_t pdata_rva, pdata_size;
    size_t nfuncs, i;

    if (argc != 2) {
        fprintf(stderr, "usage: unwinddump <file.exe>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fprintf(stderr, "unwinddump: empty file\n");
        fclose(f);
        return 2;
    }
    file = malloc((size_t)sz);
    if (!file || fread(file, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "unwinddump: read error\n");
        fclose(f);
        free(file);
        return 2;
    }
    fclose(f);

    if (pe_parse(file, (size_t)sz, &img) != 0) {
        fprintf(stderr, "unwinddump: not a PE image\n");
        free(file);
        return 2;
    }

    /* Map it: headers + every section at its VA, the way the loader would
     * (relocations excluded — unwind RVAs need no fixing). */
    image = calloc(1, img.size_of_image ? img.size_of_image : 1);
    if (!image) {
        fprintf(stderr, "unwinddump: out of memory\n");
        free(file);
        return 2;
    }
    {
        size_t hdrs = img.size_of_headers;
        if (hdrs > (size_t)sz)
            hdrs = (size_t)sz;
        if (hdrs > img.size_of_image)
            hdrs = img.size_of_image;
        memcpy(image, file, hdrs);
        for (uint16_t s = 0; s < img.section_count; s++) {
            pe_section_t sec;
            if (pe_get_section(&img, s, &sec) != 0)
                continue;
            if (sec.virtual_address >= img.size_of_image)
                continue;
            {
                size_t room = img.size_of_image - sec.virtual_address;
                size_t copy = sec.raw_size;
                if (copy > room)
                    copy = room;
                if (sec.raw_offset + copy > (size_t)sz)
                    copy = (size_t)sz > sec.raw_offset
                               ? (size_t)sz - sec.raw_offset
                               : 0;
                if (copy > 0)
                    memcpy(image + sec.virtual_address,
                           file + sec.raw_offset, copy);
            }
        }
    }
    free(file);

    w32_seh_test_add_image((uint64_t)(uintptr_t)image, img.size_of_image);

    if (img.num_directories <= PE_DIR_EXCEPTION) {
        printf("pdata_count 0\n");
        return 0;
    }
    pdata_rva = img.dir[PE_DIR_EXCEPTION].rva;
    pdata_size = img.dir[PE_DIR_EXCEPTION].size;
    if (pdata_size == 0 || pdata_size % 12 != 0 ||
        (size_t)pdata_rva + pdata_size > img.size_of_image) {
        printf("pdata_count 0\n");
        return 0;
    }
    nfuncs = pdata_size / 12;
    printf("pdata_count %zu\n", nfuncs);

    for (i = 0; i < nfuncs; i++) {
        const w32_runtime_function_t *e =
            (const w32_runtime_function_t *)(image + pdata_rva + i * 12);
        const w32_runtime_function_t *found;
        const w32_unwind_info_t *ui;
        const uint8_t *codes;
        uint32_t begin = e->begin_address;
        uint32_t end = e->end_address;
        uint32_t xdata = e->unwind_data;
        unsigned ver, flags, count, freg, fpoff;
        size_t k;

        /* The dispatcher's own lookup must find this entry (mid-range, so
         * a degenerate empty range cannot hide behind its own start). */
        if (begin < end) {
            uint32_t probe = begin + (end - begin) / 2;
            found = w32_seh_lookup(image + pdata_rva, pdata_size, probe);
            if (!found || found->begin_address != begin ||
                found->unwind_data != xdata) {
                fprintf(stderr, "unwinddump: LOOKUP-FAIL func %zu\n", i);
                return 1;
            }
        }

        /* The dispatcher's own validator must accept this chain. */
        if (w32_seh_validate_chain(image, img.size_of_image, xdata) != 0) {
            fprintf(stderr, "unwinddump: CHAIN-REFUSED func %zu\n", i);
            return 1;
        }

        ui = (const w32_unwind_info_t *)(image + xdata);
        ver = ui->ver_flags & 0x07u;
        flags = (ui->ver_flags >> 3) & 0x1Fu;
        count = ui->count_of_codes;
        freg = ui->frame_reg_off & 0x0Fu;
        fpoff = (ui->frame_reg_off >> 4) & 0x0Fu;
        codes = (const uint8_t *)(ui + 1);

        printf("func %08x %08x xdata=%08x ver=%u flags=%02x prolog=%u "
               "freg=%u fpoff=%u codes=",
               begin, end, xdata, ver, flags, ui->size_of_prolog, freg,
               fpoff);
        for (k = 0; k < count * 2u; k++)
            printf("%02x", codes[k]);
        if (count == 0)
            printf("-");

        /* Handler RVA / chained triple, when the flags promise them. */
        {
            size_t slots = count + (count & 1u);   /* pad to even */
            const uint8_t *tail = codes + slots * 2u;
            if (flags & 0x01u) {
                uint32_t h;
                memcpy(&h, tail, 4);
                printf(" handler=%08x", h);
            } else {
                printf(" handler=-");
            }
            if (flags & 0x04u) {
                uint32_t c0, c1, c2;
                memcpy(&c0, tail, 4);
                memcpy(&c1, tail + 4, 4);
                memcpy(&c2, tail + 8, 4);
                printf(" chained=%08x,%08x,%08x", c0, c1, c2);
            }
        }
        printf("\n");
    }
    return 0;
}
