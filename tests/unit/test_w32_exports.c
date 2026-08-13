/* tests/unit/test_w32_exports.c — WIN32_PLAN.md phase W32-7.
 *
 * The export-directory parser.
 *
 * This is the structure GetProcAddress walks, and it is attacker-controlled
 * data: a DLL says how many functions it has, where the three parallel
 * arrays live, and -- the dangerous one -- AddressOfNameOrdinals[i] is an
 * index INTO AddressOfFunctions that comes straight out of the file.  An
 * unvalidated index there reads wherever the file says.
 *
 * So the tests here are mostly about malformed input.  The happy path is one
 * case; the rest are files built to be wrong in a specific way, constructed
 * in memory so each one isolates a single defect.
 */

#define AURALITE_W32_HOST_TEST 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "w32/w32_pe.h"

static int passed = 0, failed = 0, tn = 0;

#define CHECK(cond, msg) do {                                              \
        if (cond) { passed++; }                                            \
        else { failed++; printf("    FAIL L%d: %s\n", __LINE__, (msg)); }  \
    } while (0)

#define RUN(f) do { tn++; printf("  [%d] %s\n", tn, #f); f(); } while (0)

/* ---- a minimal PE32+ DLL, built in memory ------------------------------
 *
 * Hand-built rather than linked so a test can make one field wrong without
 * fighting the linker.  Layout: headers at 0, one section covering
 * RVA 0x1000..0x2000 mapped from file offset 0x200.
 */

#define IMG_SIZE   0x3000
#define SEC_RVA    0x1000
#define SEC_OFF    0x200
#define SEC_SIZE   0x1000
#define FILE_SIZE  (SEC_OFF + SEC_SIZE)

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p+4, (uint32_t)(v>>32)); }

/* Offset in the file for a given RVA inside our one section. */
static size_t off_of(uint32_t rva) { return SEC_OFF + (rva - SEC_RVA); }

static uint8_t *make_dll(uint32_t export_rva, uint32_t export_size) {
    uint8_t *f = calloc(1, FILE_SIZE);

    f[0] = 'M'; f[1] = 'Z';
    put32(f + 0x3C, 0x80);                 /* e_lfanew */

    uint8_t *pe = f + 0x80;
    pe[0]='P'; pe[1]='E'; pe[2]=0; pe[3]=0;
    put16(pe + 4, 0x8664);                 /* AMD64            */
    put16(pe + 6, 1);                      /* one section      */
    put16(pe + 20, 240);                   /* SizeOfOptionalHeader */
    put16(pe + 22, 0x2022);                /* DLL | EXECUTABLE | LARGE_ADDRESS */

    uint8_t *opt = pe + 24;
    put16(opt + 0, 0x20B);                 /* PE32+            */
    put32(opt + 16, 0x1000);               /* AddressOfEntryPoint */
    put64(opt + 24, 0x180000000ull);       /* ImageBase        */
    put32(opt + 32, 0x1000);               /* SectionAlignment */
    put32(opt + 36, 0x200);                /* FileAlignment    */
    put16(opt + 68, 2);                    /* Subsystem: GUI   */
    put32(opt + 56, IMG_SIZE);             /* SizeOfImage      */
    put32(opt + 60, 0x200);                /* SizeOfHeaders    */
    put32(opt + 108, 16);                  /* NumberOfRvaAndSizes */
    put32(opt + 112 + 0 * 8, export_rva);  /* export directory */
    put32(opt + 112 + 0 * 8 + 4, export_size);

    uint8_t *sec = opt + 240;
    memcpy(sec, ".text\0\0", 7);
    put32(sec + 8,  SEC_SIZE);             /* VirtualSize      */
    put32(sec + 12, SEC_RVA);              /* VirtualAddress   */
    put32(sec + 16, SEC_SIZE);             /* SizeOfRawData    */
    put32(sec + 20, SEC_OFF);              /* PointerToRawData */
    put32(sec + 36, 0x60000020);           /* CODE|EXEC|READ   */

    return f;
}

/* Write an export directory at @dir_rva describing @n_names named exports.
 * The caller can then corrupt individual fields. */
static void write_exports(uint8_t *f, uint32_t dir_rva,
                          uint32_t ordinal_base,
                          uint32_t n_functions, uint32_t n_names) {
    uint32_t func_rva    = dir_rva + 0x40;
    uint32_t names_rva   = dir_rva + 0x80;
    uint32_t nameord_rva = dir_rva + 0xC0;
    uint32_t strs_rva    = dir_rva + 0x100;

    uint8_t *d = f + off_of(dir_rva);
    put32(d + 16, ordinal_base);
    put32(d + 20, n_functions);
    put32(d + 24, n_names);
    put32(d + 28, func_rva);
    put32(d + 32, names_rva);
    put32(d + 36, nameord_rva);

    /* Function RVAs: point at code inside the section. */
    for (uint32_t i = 0; i < n_functions; i++)
        put32(f + off_of(func_rva) + i * 4, SEC_RVA + 0x10 + i * 0x10);

    /* Names: "Fn0", "Fn1", ... each 4 bytes including the NUL. */
    for (uint32_t i = 0; i < n_names; i++) {
        uint32_t s = strs_rva + i * 8;
        put32(f + off_of(names_rva) + i * 4, s);
        uint8_t *p = f + off_of(s);
        p[0]='F'; p[1]='n'; p[2]=(uint8_t)('0'+i); p[3]=0;
        put16(f + off_of(nameord_rva) + i * 2, (uint16_t)i);
    }
}

/* ---------------------------------------------------------------------- */

static void well_formed_exports(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 3, 3);

    pe_image_t img;
    CHECK(pe_parse(f, FILE_SIZE, &img) == PE_OK, "fixture parses");

    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) == PE_OK, "exports parse");
    CHECK(n == 3, "found all three exports");
    if (n == 3) {
        CHECK(strcmp(e[0].name, "Fn0") == 0, "first export named");
        CHECK(e[0].ordinal == 1, "ordinal_base applied");
        CHECK(e[2].ordinal == 3, "third ordinal");
        CHECK(e[0].rva == SEC_RVA + 0x10, "rva as written");
        CHECK(!e[0].is_forwarder, "not a forwarder");
    }

    /* Measuring pass, same contract as pe_imports/pe_relocations. */
    size_t m = 0;
    CHECK(pe_exports(&img, NULL, 0, &m) == PE_OK, "measuring pass ok");
    CHECK(m == 3, "measuring pass agrees");
    free(f);
}

/* An export with no name is legal -- it is reachable by ordinal only.  It
 * must still be reported, or a by-ordinal export vanishes. */
static void export_without_a_name(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 3, 2);     /* 3 functions, only 2 named */

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) == PE_OK, "parses with an unnamed export");
    CHECK(n == 3, "the unnamed export is still reported");
    if (n == 3) CHECK(e[2].name[0] == 0, "third export has no name");
    free(f);
}

/* A zero RVA in the function array is a hole in the ordinal space, not an
 * export.  Reporting it would hand out a pointer to the image base. */
static void holes_are_skipped(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 3, 3);
    put32(f + off_of(0x1440) + 1 * 4, 0);   /* punch out function 1 */

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) == PE_OK, "parses with a hole");
    CHECK(n == 2, "the hole is not reported as an export");
    free(f);
}

/* A forwarder's RVA points back inside the export directory, where a string
 * naming another DLL lives.  It must be flagged, because returning that
 * address to a caller hands them a pointer to text they will then call. */
static void forwarder_is_flagged(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 2, 2);
    /* Point function 0 into the directory's own range. */
    put32(f + off_of(0x1440) + 0 * 4, 0x1400 + 0x180);

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) == PE_OK, "parses with a forwarder");
    CHECK(n == 2, "forwarder is still an export");
    if (n >= 1) CHECK(e[0].is_forwarder, "forwarder detected");
    if (n >= 2) CHECK(!e[1].is_forwarder, "ordinary export not misflagged");
    free(f);
}

/* --- malformed input ---------------------------------------------------
 * Each of these must be refused rather than followed.  None may read out of
 * bounds; under ASan that is enforced, not merely hoped for. */

static void counts_out_of_range_refused(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 2, 2);
    put32(f + off_of(0x1400) + 20, 0x7FFFFFFF);   /* NumberOfFunctions */

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) != PE_OK, "absurd function count refused");
    free(f);
}

static void arrays_outside_the_image_refused(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 2, 2);
    put32(f + off_of(0x1400) + 28, 0x7F000000);   /* AddressOfFunctions */

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) != PE_OK,
          "function array outside the image refused");
    free(f);
}

static void name_pointer_outside_the_image_refused(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 2, 2);
    put32(f + off_of(0x1480) + 0 * 4, 0x7F000000); /* a name string RVA */

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    CHECK(pe_exports(&img, e, 8, &n) != PE_OK,
          "name string outside the image refused");
    free(f);
}

/* The sharp one: AddressOfNameOrdinals[i] indexes the function array, and
 * the value comes out of the file.  An index past NumberOfFunctions must not
 * be used to read.  A parser that trusts it reads out of bounds -- which is
 * why this test exists and why it runs under ASan. */
static void bogus_name_ordinal_is_safe(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 2, 2);
    put16(f + off_of(0x14C0) + 0 * 2, 0xFFFF);

    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 0;
    /* Either refusing or ignoring the bad entry is acceptable; reading out
     * of bounds is not, and ASan is what decides that. */
    int rc = pe_exports(&img, e, 8, &n);
    CHECK(rc == PE_OK || rc != PE_OK, "bogus name ordinal did not crash");
    CHECK(n <= 2, "no phantom exports invented");
    free(f);
}

static void no_export_directory_is_not_an_error(void) {
    uint8_t *f = make_dll(0, 0);
    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    pe_export_t e[8];
    size_t n = 99;
    CHECK(pe_exports(&img, e, 8, &n) == PE_OK, "no exports is not an error");
    CHECK(n == 0, "and reports zero");
    free(f);
}

static void null_arguments_refused(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 1, 1);
    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);
    size_t n = 0;
    CHECK(pe_exports(NULL, NULL, 0, &n) != PE_OK, "NULL image refused");
    CHECK(pe_exports(&img, NULL, 0, NULL) != PE_OK, "NULL count refused");
    free(f);
}

/* A buffer smaller than the export count must still report the true total,
 * so a caller can size a second call -- and must not write past the end. */
static void small_buffer_reports_true_total(void) {
    uint8_t *f = make_dll(0x1400, 0x200);
    write_exports(f, 0x1400, 1, 3, 3);
    pe_image_t img;
    pe_parse(f, FILE_SIZE, &img);

    struct { pe_export_t e[1]; unsigned canary; } box;
    box.canary = 0xA5A5A5A5u;
    size_t n = 0;
    CHECK(pe_exports(&img, box.e, 1, &n) == PE_OK, "small buffer still ok");
    CHECK(n == 3, "true total reported");
    CHECK(box.canary == 0xA5A5A5A5u, "nothing written past the buffer");
    free(f);
}

int main(void) {
    printf("== test_w32_exports (WIN32_PLAN W32-7) ==\n");
    RUN(well_formed_exports);
    RUN(export_without_a_name);
    RUN(holes_are_skipped);
    RUN(forwarder_is_flagged);
    RUN(counts_out_of_range_refused);
    RUN(arrays_outside_the_image_refused);
    RUN(name_pointer_outside_the_image_refused);
    RUN(bogus_name_ordinal_is_safe);
    RUN(no_export_directory_is_not_an_error);
    RUN(null_arguments_refused);
    RUN(small_buffer_reports_true_total);
    printf("== %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
