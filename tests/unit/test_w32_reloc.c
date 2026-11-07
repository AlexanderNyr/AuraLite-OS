/* tests/unit/test_w32_reloc.c — RESIDUE3_PLAN.md phase C8 host gate.
 *
 * Pins the loader's base-relocation path and the numbers that made it a
 * phase.  The measured case: Audacity 3.7.5 carries 90932 base relocations,
 * and both loaders (the exe one in userspace/apps/w32run, the DLL one in
 * w32/src/w32_module.c) held a `static pe_reloc_t relocs[16384]` copy, so the
 * image died at "too many relocations" before import resolution began.
 *
 * What this gate proves, and why it needs a host test:
 *   - the cap is GONE, not raised: a generated image with 90932 relocations
 *     (Audacity's measured count) and one with 200000 both relocate cleanly,
 *     and `pe_relocations_each` stores nothing, so the second number costs
 *     nothing but time;
 *   - the parse contract the old array form had -- "report the true total even
 *     when the buffer is smaller" -- still holds, because the gap ledger uses
 *     it to size an image before mapping it;
 *   - the refusals are still refusals, and they NAME what they refused: a
 *     HIGHLOW entry (type 3) used to print "unsupported relocation type" with
 *     no number, and a fixup past the end of the mapping was only caught
 *     because the caller happened to bounds-check first;
 *   - the fixups are actually applied (a known quad gets exactly `delta`
 *     added, once), which is the one property a count cannot show.
 *
 * The image builder is here, not in a checked-in blob, so every hostile case
 * is one field away from the good image.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_pe.h"
#include "w32/w32_reloc.h"
#include "w32/w32_module.h"   /* the slot/depth constants, not the loader */
#include "../../w32/src/w32_pe.c"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++;                     \
                    if (failed == b) passed++;                     \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) {                                  \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%lld want %lld\n",      \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the builder --------------------------------------------------------- */

#define PE_OFF     0x80u
#define OPT_OFF    (PE_OFF + 24u)
#define OPT_SIZE   240u
#define SECT_OFF   (OPT_OFF + OPT_SIZE)
#define HDR_SIZE   0x400u
#define TEXT_RAW   0x400u
#define TEXT_FILE  0x200u
#define RELOC_RAW  0x600u
#define TEXT_RVA   0x1000u
#define TEXT_SPAN  0x200000u          /* 2 MiB of virtual room for fixups */
#define RELOC_RVA  0x300000u
#define IMAGE_SPAN 0x400000u          /* SizeOfImage, and the mapped span */

#define ENT_PER_BLOCK 511u            /* 511 quads is one 4 KiB page */
#define BLOCK_BYTES(n) (8u + 2u * (n))

static void w16(uint8_t *b, uint32_t o, uint16_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8);
}
static void w32_(uint8_t *b, uint32_t o, uint32_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v>>8);
    b[o+2] = (uint8_t)(v>>16); b[o+3] = (uint8_t)(v>>24);
}
static void w64_(uint8_t *b, uint32_t o, uint64_t v) {
    w32_(b, o, (uint32_t)v); w32_(b, o+4, (uint32_t)(v>>32));
}

typedef struct {
    uint8_t *file;
    size_t   size;
    size_t   nrel;             /* relocations written (0 = none)          */
    uint32_t entry_type;       /* PE_REL_* used for every entry           */
} img_t;

static size_t reloc_bytes_for(size_t nrel) {
    size_t blocks = (nrel + ENT_PER_BLOCK - 1) / ENT_PER_BLOCK;
    if (blocks == 0) return 0;
    size_t last = nrel - (blocks - 1) * ENT_PER_BLOCK;
    return (blocks - 1) * BLOCK_BYTES(ENT_PER_BLOCK) + BLOCK_BYTES(last);
}

/* Build a two-section PE32+ (PE_) with `nrel` relocations of `type`.
 *
 * The fixups land at RVA 0x1000 + i*8, i.e. inside .text's 2 MiB virtual size
 * but far past its 0x200 raw bytes -- which is exactly the shape of a real
 * image (the .reloc directory names addresses in sections whose contents the
 * loader maps, not in the file it parses). */
static img_t build(size_t nrel, uint32_t type) {
    img_t m;
    size_t rb = reloc_bytes_for(nrel);
    memset(&m, 0, sizeof m);
    m.size = RELOC_RAW + rb + 0x200;
    m.file = calloc(1, m.size);
    m.nrel = nrel;
    m.entry_type = type;

    w16(m.file, 0, PE_DOS_MAGIC);
    w32_(m.file, 0x3C, PE_OFF);
    w32_(m.file, PE_OFF, PE_NT_SIGNATURE);
    w16(m.file, PE_OFF + 4,  PE_MACHINE_AMD64);
    w16(m.file, PE_OFF + 6,  2);                       /* sections         */
    w16(m.file, PE_OFF + 20, OPT_SIZE);
    w16(m.file, PE_OFF + 22, PE_FILE_EXECUTABLE_IMAGE | PE_FILE_LARGE_ADDR_AWARE);

    w16(m.file,  OPT_OFF + 0,   PE_OPT_MAGIC_PE32PLUS);
    w32_(m.file, OPT_OFF + 16,  TEXT_RVA);             /* entry point      */
    w64_(m.file, OPT_OFF + 24,  0x140000000ull);       /* ImageBase        */
    w32_(m.file, OPT_OFF + 32,  0x1000);               /* SectionAlignment */
    w32_(m.file, OPT_OFF + 36,  0x200);                /* FileAlignment    */
    w32_(m.file, OPT_OFF + 56,  IMAGE_SPAN);           /* SizeOfImage      */
    w32_(m.file, OPT_OFF + 60,  HDR_SIZE);             /* SizeOfHeaders    */
    w16(m.file,  OPT_OFF + 68,  PE_SUBSYSTEM_WINDOWS_CUI);
    w32_(m.file, OPT_OFF + 108, 16);                   /* NumRvaAndSizes   */

    /* .text */
    memcpy(m.file + SECT_OFF, ".text\0\0\0", 8);
    w32_(m.file, SECT_OFF + 8,  TEXT_SPAN);
    w32_(m.file, SECT_OFF + 12, TEXT_RVA);
    w32_(m.file, SECT_OFF + 16, TEXT_FILE);
    w32_(m.file, SECT_OFF + 20, TEXT_RAW);
    w32_(m.file, SECT_OFF + 36, PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE |
                                PE_SCN_MEM_READ | PE_SCN_MEM_WRITE);

    /* .reloc: directory entry 5 -> RELOC_RVA, size rb */
    memcpy(m.file + SECT_OFF + 40, ".reloc\0\0", 8);
    w32_(m.file, SECT_OFF + 40 + 8,  (uint32_t)rb);
    w32_(m.file, SECT_OFF + 40 + 12, RELOC_RVA);
    w32_(m.file, SECT_OFF + 40 + 16, (uint32_t)rb);
    w32_(m.file, SECT_OFF + 40 + 20, RELOC_RAW);
    w32_(m.file, SECT_OFF + 40 + 36, PE_SCN_CNT_INITIALIZED |
                                     PE_SCN_MEM_READ);
    w32_(m.file, OPT_OFF + 112 + 5 * 8, RELOC_RVA);
    w32_(m.file, OPT_OFF + 112 + 5 * 8 + 4, (uint32_t)rb);

    /* The blocks, filled the way a linker fills them: one 4 KiB page each. */
    size_t off = RELOC_RAW, i = 0;
    uint32_t page = 0;
    while (i < nrel) {
        size_t take = nrel - i;
        if (take > ENT_PER_BLOCK) take = ENT_PER_BLOCK;
        uint32_t page_rva = TEXT_RVA + page * 0x1000u;
        w32_(m.file, (uint32_t)off,     page_rva);
        w32_(m.file, (uint32_t)off + 4, (uint32_t)BLOCK_BYTES(take));
        for (size_t k = 0; k < take; k++) {
            /* Offsets step by 8 bytes so every fixup is a distinct quad and
             * none straddles the page end (0xFF8 is the last one that fits). */
            uint16_t voff = (uint16_t)(k * 8u);      /* distinct quads only */
            uint16_t e = (uint16_t)((type << 12) | (voff & 0x0FFFu));
            w16(m.file, (uint32_t)(off + 8 + k * 2), e);
        }
        off += BLOCK_BYTES(take);
        i += take;
        page++;
    }
    return m;
}

static void drop(img_t *m) { free(m->file); m->file = NULL; m->size = 0; }

/* A page-rounded mapping of the image, all zero: the loader's mmap. */
static uint8_t *map_of(const img_t *m) {
    (void)m;
    return calloc(1, IMAGE_SPAN);
}

/* ---- the cap is gone ----------------------------------------------------- */

static void test_audacity_scale_relocates(void) {
    /* The measured count from w32/app_ledger/audacity-3.7.5.gap. */
    img_t m = build(90932, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;

    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);

    /* The old contract still holds: the array form reports the TRUE total
     * even when the caller's buffer is tiny -- that is how a caller sizes
     * a buffer, and how the gap ledger recounts an image before mapping it. */
    {
        pe_reloc_t small[4];
        size_t total = 0;
        CHECK_EQ(pe_relocations(&img, small, 4, &total), PE_OK);
        CHECK_EQ(total, 90932);
    }

    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), 0);
    CHECK(rr.why == NULL);
    CHECK_EQ(rr.seen, 90932);
    CHECK_EQ(rr.applied, 90932);
    CHECK_EQ(rr.by_type[PE_REL_DIR64], 90932);
    CHECK(rr.delta != 0);

    /* Applied, not merely counted: the quad at the first fixup RVA moved by
     * exactly delta, and a byte past the last fixup did not move at all. */
    {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | base[TEXT_RVA + k];
        CHECK_EQ(v, rr.delta);
        uint64_t far = 0;
        for (int k = 7; k >= 0; k--) far = (far << 8) | base[TEXT_RVA + 0x100000 + k];
        CHECK_EQ(far, 0);
    }
    free(base); drop(&m);
}

static void test_no_cap_at_200k(void) {
    img_t m = build(200000, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;

    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), 0);
    CHECK_EQ(rr.applied, 200000);
    /* The line a receipt quotes: it says "no fixed cap" in so many words. */
    {
        char line[200];
        w32_reloc_describe(&rr, line, sizeof line);
        CHECK(strstr(line, "200000") != NULL);
        CHECK(strstr(line, "no fixed cap") != NULL);
    }
    free(base); drop(&m);
}

/* The stop test needs a real function pointer; declared at file scope above
 * the runner so the C89-ish guest compilers keep it simple. */
int g_stop_seen = 0;
static int stop_after_10(void *ctx, uint32_t type, uint32_t rva) {
    (void)ctx; (void)type; (void)rva;
    return ++g_stop_seen >= 10 ? 1 : 0;
}
static void test_each_stops_after_10(void) {
    img_t m = build(5000, PE_REL_DIR64);
    pe_image_t img;
    size_t n = 0;
    int ctx = 0;
    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(pe_relocations_each(&img, stop_after_10, &ctx, &n), PE_OK);
    CHECK_EQ(n, 10);
    CHECK_EQ(g_stop_seen, 10);
    drop(&m);
}

static void test_recount_writes_nothing(void) {
    img_t m = build(90932, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;
    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(w32_recount_relocations(&img, &rr), 0);
    CHECK_EQ(rr.seen, 90932);
    CHECK_EQ(rr.applied, 0);
    {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | base[TEXT_RVA + k];
        CHECK_EQ(v, 0);            /* counting is not relocating */
    }
    free(base); drop(&m);
}

/* ---- refusals stay refusals, and name what they refused ------------------ */

static void test_refuses_foreign_type_by_number(void) {
    img_t m = build(16, 3 /* IMAGE_REL_BASED_HIGHLOW */);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;
    char line[200];

    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), -1);
    CHECK(rr.why != NULL);
    CHECK(strstr(rr.why, "unsupported relocation type") != NULL);
    CHECK_EQ(rr.refused_type, 3);
    CHECK_EQ(rr.applied, 0);
    w32_reloc_describe(&rr, line, sizeof line);
    CHECK(strstr(line, "type 3") != NULL);

    /* The recount agrees, because it is the same parser and the same order. */
    CHECK_EQ(w32_recount_relocations(&img, &rr), -1);
    CHECK_EQ(rr.refused_type, 3);
    free(base); drop(&m);
}

static void test_refuses_fixup_past_span(void) {
    img_t m = build(64, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;

    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    /* Map only the headers: the first fixup at RVA 0x1000 is already out. */
    CHECK_EQ(w32_relocate_image(&img, base, 0x800, &rr), -1);
    CHECK(rr.why != NULL);
    CHECK(strstr(rr.why, "outside image") != NULL);
    CHECK_EQ(rr.refused_rva, TEXT_RVA);
    free(base); drop(&m);
}

static void test_stripped_needs_preferred_base(void) {
    img_t m = build(8, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;

    /* IMAGE_FILE_RELOCS_STRIPPED: Characteristics bit 0 of the COFF header.
     * Patched BEFORE the parse -- pe_parse copies the field into the image
     * struct, so editing the bytes afterwards changes nothing (the first
     * version of this test proved that the hard way). */
    w16(m.file, PE_OFF + 22, PE_FILE_EXECUTABLE_IMAGE |
                             PE_FILE_LARGE_ADDR_AWARE | 0x0001u);
    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);

    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), -1);
    CHECK(rr.why != NULL);
    CHECK(strstr(rr.why, "stripped") != NULL);

    /* Away from the preferred base it is a refusal; AT it, nothing needs
     * moving and the image is legal -- so the test moves the image's preferred
     * base to the mapping it actually has, rather than pretending 0x140000000
     * is mapped (which the first version did, and ASan said so). */
    w16(m.file, PE_OFF + 22, PE_FILE_EXECUTABLE_IMAGE |
                             PE_FILE_LARGE_ADDR_AWARE);
    w64_(m.file, OPT_OFF + 24, (uint64_t)(uintptr_t)base);
    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), 0);
    CHECK_EQ(rr.delta, 0);
    CHECK_EQ(rr.applied, 8);

    free(base); drop(&m);
}

static void test_no_relocations_is_legal(void) {
    img_t m = build(0, PE_REL_DIR64);
    uint8_t *base = map_of(&m);
    pe_image_t img;
    w32_reloc_report_t rr;

    CHECK_EQ(pe_parse(m.file, m.size, &img), PE_OK);
    CHECK_EQ(w32_relocate_image(&img, base, IMAGE_SPAN, &rr), 0);
    CHECK_EQ(rr.seen, 0);
    CHECK_EQ(rr.applied, 0);
    free(base); drop(&m);
}

/* ---- the module table's own scale (C8's second wall) -------------------- */

static void test_module_table_fits_the_bundle(void) {
    /* Audacity 3.7.5's bundle: 117 DLLs + 21 built-ins = 138 slots.  The table
     * was 64, which refuses the graph at the 44th load. */
    CHECK(W32_MODULE_MAX >= 138);
    CHECK(W32_MODULE_MAX == 256);
    /* The per-image hold list used to be W32_MODULE_MAX wide and silently
     * dropped the 33rd hold; Audacity.exe alone imports 34 modules. */
    CHECK(W32_MODULE_HOLDS_MAX >= 34);
    /* The depth ceiling used to be 8, which names a refusal on a graph that is
     * four levels deep (exe -> wxmsw -> lib-* -> vcruntime). */
    CHECK(W32_LOAD_DEPTH_MAX >= 16);
}

int main(void) {
    printf("=== AuraLite OS w32 base-relocation tests (RESIDUE3 C8) ===\n");
    RUN(test_audacity_scale_relocates);
    RUN(test_no_cap_at_200k);
    RUN(test_each_stops_after_10);
    RUN(test_recount_writes_nothing);
    RUN(test_refuses_foreign_type_by_number);
    RUN(test_refuses_fixup_past_span);
    RUN(test_stripped_needs_preferred_base);
    RUN(test_no_relocations_is_legal);
    RUN(test_module_table_fits_the_bundle);
    printf("\n%d/%d groups passed\n", passed, tn);
    return failed ? 1 : 0;
}
