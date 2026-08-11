/*
 * test_w32_pe.c — host unit tests for WIN32_PLAN.md phase W32-2.
 *
 * The gate from the plan, in order:
 *   - malformed inputs refused without crash or over-read: e_lfanew past EOF,
 *     absurd section count, SizeOfRawData beyond the file, bogus relocation
 *     block size, import descriptor with a name RVA outside any section;
 *   - a fuzz corpus of truncated/bit-flipped PEs: no crash, no hang.
 *
 * The parser's real fixture is the project's own build/boot/BOOTX64.EFI, but a
 * unit test must not depend on a build artefact existing, so this file also
 * synthesises PE images in memory.  When the EFI binary *is* present, the
 * last test parses it and checks the fields llvm-readobj reports, plus the
 * property that matters most: a firmware image must be refused as a process.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_pe.h"
#include "../../w32/src/w32_pe.c"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%lld want %lld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- a minimal, well-formed PE32+ builder -------------------------------- */
/*
 * Layout produced (offsets):
 *   0x000 DOS header, e_lfanew = 0x80
 *   0x080 "PE\0\0" + COFF header (20) + optional header (240) + sections
 *   0x400 .text raw data
 *
 * Keeping the builder here — rather than checking in a binary blob — means
 * every hostile case below is one field edit away from the good image, which
 * is what makes the negative tests readable.
 */
#define IMG_SIZE   0x800u
#define PE_OFF     0x80u
#define OPT_OFF    (PE_OFF + 24u)
#define OPT_SIZE   240u
#define SECT_OFF   (OPT_OFF + OPT_SIZE)
#define TEXT_RAW   0x400u

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

static uint8_t *make_pe(void) {
    uint8_t *b = calloc(1, IMG_SIZE);

    w16(b, 0, PE_DOS_MAGIC);
    w32_(b, 0x3C, PE_OFF);

    w32_(b, PE_OFF, PE_NT_SIGNATURE);
    w16(b, PE_OFF + 4,  PE_MACHINE_AMD64);     /* Machine */
    w16(b, PE_OFF + 6,  1);                    /* NumberOfSections */
    w16(b, PE_OFF + 20, OPT_SIZE);             /* SizeOfOptionalHeader */
    w16(b, PE_OFF + 22, PE_FILE_EXECUTABLE_IMAGE | PE_FILE_LARGE_ADDR_AWARE);

    w16(b,  OPT_OFF + 0,   PE_OPT_MAGIC_PE32PLUS);
    w32_(b, OPT_OFF + 16,  0x1000);            /* AddressOfEntryPoint */
    w64_(b, OPT_OFF + 24,  0x140000000ull);    /* ImageBase */
    w32_(b, OPT_OFF + 32,  0x1000);            /* SectionAlignment */
    w32_(b, OPT_OFF + 36,  0x200);             /* FileAlignment */
    w32_(b, OPT_OFF + 56,  0x2000);            /* SizeOfImage */
    w32_(b, OPT_OFF + 60,  0x400);             /* SizeOfHeaders */
    w16(b,  OPT_OFF + 68,  PE_SUBSYSTEM_WINDOWS_CUI);
    w32_(b, OPT_OFF + 108, 16);                /* NumberOfRvaAndSizes */

    memcpy(b + SECT_OFF, ".text\0\0\0", 8);
    w32_(b, SECT_OFF + 8,  0x1000);            /* VirtualSize */
    w32_(b, SECT_OFF + 12, 0x1000);            /* VirtualAddress */
    w32_(b, SECT_OFF + 16, 0x200);             /* SizeOfRawData */
    w32_(b, SECT_OFF + 20, TEXT_RAW);          /* PointerToRawData */
    w32_(b, SECT_OFF + 36, PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ);
    return b;
}

/* ---- well-formed --------------------------------------------------------- */

static void test_parse_minimal(void) {
    uint8_t *b = make_pe();
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(img.machine, PE_MACHINE_AMD64);
    CHECK_EQ(img.section_count, 1);
    CHECK_EQ(img.opt_magic, PE_OPT_MAGIC_PE32PLUS);
    CHECK_EQ(img.image_base, 0x140000000ull);
    CHECK_EQ(img.entry_point_rva, 0x1000);
    CHECK_EQ(img.subsystem, PE_SUBSYSTEM_WINDOWS_CUI);
    CHECK_EQ(pe_check_loadable(&img), PE_OK);

    pe_section_t s;
    CHECK_EQ(pe_get_section(&img, 0, &s), PE_OK);
    CHECK(strcmp(s.name, ".text") == 0);
    CHECK_EQ(s.virtual_address, 0x1000);
    /* Out-of-range section index is an argument error, not a read. */
    CHECK_EQ(pe_get_section(&img, 1, &s), PE_ERR_ARG);
    free(b);
}

static void test_rva_translation(void) {
    uint8_t *b = make_pe();
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);

    uint32_t off;
    CHECK_EQ(pe_rva_to_offset(&img, 0x1000, 1, &off), PE_OK);
    CHECK_EQ(off, TEXT_RAW);
    CHECK_EQ(pe_rva_to_offset(&img, 0x1010, 4, &off), PE_OK);
    CHECK_EQ(off, TEXT_RAW + 0x10);

    /* Straddling the end of the raw data must be refused, not clamped. */
    CHECK_EQ(pe_rva_to_offset(&img, 0x11FF, 8, &off), PE_ERR_TRUNCATED);
    /* An RVA in no section at all. */
    CHECK_EQ(pe_rva_to_offset(&img, 0x9000, 1, &off), PE_ERR_MALFORMED);
    /* Length that would wrap the 32-bit RVA space. */
    CHECK_EQ(pe_rva_to_offset(&img, 0xFFFFFFF0u, 0x20, &off), PE_ERR_MALFORMED);
    free(b);
}

/* ---- malformed: each is the good image with one field broken -------------- */

static void test_reject_not_pe(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w16(b, 0, 0x4243);                      /* not "MZ" */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_NOT_PE);
    free(b);

    b = make_pe();
    w32_(b, PE_OFF, 0xDEADBEEF);            /* not "PE\0\0" */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_NOT_PE);
    free(b);

    /* Buffer far too small to hold even a DOS header. */
    uint8_t tiny[8] = { 0x4D, 0x5A };
    CHECK_EQ(pe_parse(tiny, sizeof tiny, &img), PE_ERR_TRUNCATED);
    CHECK_EQ(pe_parse(NULL, 100, &img), PE_ERR_ARG);
}

static void test_reject_bad_e_lfanew(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w32_(b, 0x3C, 0xFFFFFFF0u);             /* past EOF */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_TRUNCATED);
    free(b);

    b = make_pe();
    w32_(b, 0x3C, IMG_SIZE - 4);            /* headers would run off the end */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_TRUNCATED);
    free(b);

    b = make_pe();
    w32_(b, 0x3C, 0x10);                    /* overlaps the DOS header */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);
}

static void test_reject_machine_and_magic(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w16(b, PE_OFF + 4, PE_MACHINE_I386);    /* 32-bit: out of scope */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MACHINE);
    free(b);

    b = make_pe();
    w16(b, OPT_OFF, PE_OPT_MAGIC_PE32);     /* PE32, not PE32+ */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_NOT_PE32PLUS);
    free(b);
}

static void test_reject_absurd_section_count(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w16(b, PE_OFF + 6, 0xFFFF);             /* the plan's named case */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);

    b = make_pe();
    w16(b, PE_OFF + 6, 0);                  /* zero sections */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);
}

static void test_reject_raw_data_beyond_file(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w32_(b, SECT_OFF + 16, 0x10000);        /* SizeOfRawData past EOF */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_TRUNCATED);
    free(b);

    b = make_pe();
    w32_(b, SECT_OFF + 20, 0xFFFFFF00u);    /* PointerToRawData past EOF */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_TRUNCATED);
    free(b);
}

static void test_reject_bad_alignment(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w32_(b, OPT_OFF + 32, 0x1234);          /* not a power of two */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);

    b = make_pe();
    w32_(b, OPT_OFF + 32, 0x100);           /* section < file alignment */
    w32_(b, OPT_OFF + 36, 0x200);
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);

    b = make_pe();
    w32_(b, OPT_OFF + 36, 0);               /* zero alignment */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);
}

static void test_reject_too_many_directories(void) {
    pe_image_t img;
    uint8_t *b = make_pe();
    w32_(b, OPT_OFF + 108, 0x10000);        /* NumberOfRvaAndSizes absurd */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_ERR_MALFORMED);
    free(b);
}

/* ---- W^X and subsystem policy -------------------------------------------- */

static void test_reject_write_execute_section(void) {
    uint8_t *b = make_pe();
    w32_(b, SECT_OFF + 36,
         PE_SCN_MEM_EXECUTE | PE_SCN_MEM_WRITE | PE_SCN_MEM_READ);
    pe_image_t img;
    /* Parsing succeeds — the image is structurally fine — but policy refuses
     * it, mirroring how kernel/proc/elf.c derives W^X from p_flags. */
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(pe_check_loadable(&img), PE_ERR_UNSUPPORTED);
    free(b);
}

static void test_reject_efi_subsystem(void) {
    /* The property that keeps AuraLite's own BOOTX64.EFI from being run as a
     * user process. */
    static const uint16_t efi[] = {
        PE_SUBSYSTEM_EFI_APPLICATION, PE_SUBSYSTEM_EFI_BOOT_DRIVER,
        PE_SUBSYSTEM_EFI_RUNTIME_DRV, PE_SUBSYSTEM_EFI_ROM,
        PE_SUBSYSTEM_NATIVE
    };
    for (size_t i = 0; i < sizeof efi / sizeof efi[0]; i++) {
        uint8_t *b = make_pe();
        w16(b, OPT_OFF + 68, efi[i]);
        pe_image_t img;
        CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
        CHECK_EQ(pe_check_loadable(&img), PE_ERR_UNSUPPORTED);
        free(b);
    }
}

static void test_reject_no_entry_point(void) {
    uint8_t *b = make_pe();
    w32_(b, OPT_OFF + 16, 0);
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(pe_check_loadable(&img), PE_ERR_UNSUPPORTED);
    free(b);
}

/* ---- relocations --------------------------------------------------------- */

/* Add a .reloc section carrying one block, so the walker has real input. */
static uint8_t *make_pe_with_relocs(uint32_t block_size_override) {
    uint8_t *b = make_pe();

    /* Grow to two sections; the second is .reloc at RVA 0x2000, raw 0x600. */
    w16(b, PE_OFF + 6, 2);
    uint32_t s2 = SECT_OFF + 40;
    memcpy(b + s2, ".reloc\0\0", 8);
    w32_(b, s2 + 8,  0x200);
    w32_(b, s2 + 12, 0x2000);
    w32_(b, s2 + 16, 0x200);
    w32_(b, s2 + 20, 0x600);
    w32_(b, s2 + 36, PE_SCN_CNT_INITIALIZED | PE_SCN_MEM_READ);

    /* Directory entry 5 -> the .reloc RVA. */
    w32_(b, OPT_OFF + 112 + PE_DIR_BASERELOC * 8,     0x2000);
    w32_(b, OPT_OFF + 112 + PE_DIR_BASERELOC * 8 + 4, 12);

    /* One block: page 0x1000, size 12 => two 16-bit entries. */
    w32_(b, 0x600, 0x1000);
    w32_(b, 0x604, block_size_override ? block_size_override : 12);
    w16(b, 0x608, (uint16_t)((PE_REL_DIR64 << 12) | 0x018));
    w16(b, 0x60A, (uint16_t)((PE_REL_ABSOLUTE << 12) | 0));   /* padding */
    return b;
}

static void test_relocations_walk(void) {
    uint8_t *b = make_pe_with_relocs(0);
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);

    pe_reloc_t r[8]; size_t n = 0;
    CHECK_EQ(pe_relocations(&img, r, 8, &n), PE_OK);
    /* The ABSOLUTE entry is padding and must not be reported. */
    CHECK_EQ(n, 1);
    CHECK_EQ(r[0].type, PE_REL_DIR64);
    CHECK_EQ(r[0].rva, 0x1018);
    free(b);
}

static void test_reject_bad_reloc_block(void) {
    pe_image_t img;
    pe_reloc_t r[8]; size_t n;

    /* Block smaller than its own 8-byte header — the plan's named case. */
    uint8_t *b = make_pe_with_relocs(4);
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(pe_relocations(&img, r, 8, &n), PE_ERR_MALFORMED);
    free(b);

    /* Block claiming more than the directory contains. */
    b = make_pe_with_relocs(0x10000);
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(pe_relocations(&img, r, 8, &n), PE_ERR_MALFORMED);
    free(b);

    /* Odd block size: the entry array cannot be whole. */
    b = make_pe_with_relocs(11);
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    CHECK_EQ(pe_relocations(&img, r, 8, &n), PE_ERR_MALFORMED);
    free(b);
}

static void test_no_relocs_is_ok(void) {
    uint8_t *b = make_pe();
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    pe_reloc_t r[4]; size_t n = 99;
    /* A stripped image is legal and must report zero, not an error. */
    CHECK_EQ(pe_relocations(&img, r, 4, &n), PE_OK);
    CHECK_EQ(n, 0);
    free(b);
}

/* ---- imports ------------------------------------------------------------- */

static void test_reject_import_name_outside_sections(void) {
    uint8_t *b = make_pe();
    /* Point the import directory at an RVA that lies in no section — the
     * plan's named case. */
    w32_(b, OPT_OFF + 112 + PE_DIR_IMPORT * 8,     0x9000);
    w32_(b, OPT_OFF + 112 + PE_DIR_IMPORT * 8 + 4, 20);
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    pe_import_t imp[4]; size_t n;
    CHECK_EQ(pe_imports(&img, imp, 4, &n), PE_ERR_MALFORMED);
    free(b);
}

static void test_no_imports_is_ok(void) {
    uint8_t *b = make_pe();
    pe_image_t img;
    CHECK_EQ(pe_parse(b, IMG_SIZE, &img), PE_OK);
    pe_import_t imp[4]; size_t n = 99;
    CHECK_EQ(pe_imports(&img, imp, 4, &n), PE_OK);
    CHECK_EQ(n, 0);
    free(b);
}

/* ---- fuzz: truncation and bit flips must never crash or hang -------------- */

static void test_fuzz_truncation(void) {
    uint8_t *good = make_pe_with_relocs(0);
    /* Every prefix of a valid image. */
    for (size_t len = 0; len <= IMG_SIZE; len += 7) {
        pe_image_t img;
        int rc = pe_parse(good, len, &img);
        if (rc == PE_OK) {
            pe_import_t imp[8]; size_t ni;
            pe_reloc_t  rel[8]; size_t nr;
            (void)pe_imports(&img, imp, 8, &ni);
            (void)pe_relocations(&img, rel, 8, &nr);
            (void)pe_check_loadable(&img);
        }
        CHECK(rc <= 0);            /* never a positive/undefined return */
    }
    free(good);
}

static void test_fuzz_bitflips(void) {
    /* Deterministic sweep: flip one bit in the header region, parse, and walk
     * whatever the parser accepted.  ASan/UBSan turn any over-read into a
     * failure; without them this still catches hangs and wild returns. */
    for (uint32_t byte = 0; byte < SECT_OFF + 80; byte += 3) {
        for (int bit = 0; bit < 8; bit += 3) {
            uint8_t *b = make_pe_with_relocs(0);
            b[byte] ^= (uint8_t)(1u << bit);
            pe_image_t img;
            int rc = pe_parse(b, IMG_SIZE, &img);
            if (rc == PE_OK) {
                pe_import_t imp[16]; size_t ni;
                pe_reloc_t  rel[16]; size_t nr;
                (void)pe_imports(&img, imp, 16, &ni);
                (void)pe_relocations(&img, rel, 16, &nr);
                (void)pe_check_loadable(&img);
            }
            CHECK(rc <= 0);
            free(b);
        }
    }
}

/* ---- the real fixture, when the build produced it ------------------------ */

static void test_real_bootx64_efi(void) {
    FILE *f = fopen("build/boot/BOOTX64.EFI", "rb");
    if (!f) {
        printf("  SKIP: build/boot/BOOTX64.EFI not built\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    CHECK_EQ(got, (size_t)sz);

    pe_image_t img;
    CHECK_EQ(pe_parse(buf, (size_t)sz, &img), PE_OK);

    /* Cross-checked against:
     *   llvm-readobj-19 --file-headers build/boot/BOOTX64.EFI
     * Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664), SectionCount: 3,
     * OptionalHeaderSize: 240, Subsystem: EFI application (10). */
    CHECK_EQ(img.machine, PE_MACHINE_AMD64);
    CHECK_EQ(img.section_count, 3);
    CHECK_EQ(img.opt_magic, PE_OPT_MAGIC_PE32PLUS);
    CHECK_EQ(img.subsystem, PE_SUBSYSTEM_EFI_APPLICATION);
    CHECK(img.entry_point_rva != 0);

    /* Every section's raw range must already have been validated. */
    for (uint16_t i = 0; i < img.section_count; i++) {
        pe_section_t s;
        CHECK_EQ(pe_get_section(&img, i, &s), PE_OK);
    }

    /* The point of the exercise: a firmware image parses fine and is still
     * refused as a process. */
    CHECK_EQ(pe_check_loadable(&img), PE_ERR_UNSUPPORTED);

    /* Walking its tables must not fault, whatever they contain. */
    pe_import_t imp[64]; size_t ni;
    pe_reloc_t  rel[256]; size_t nr;
    CHECK(pe_imports(&img, imp, 64, &ni) <= 0);
    CHECK(pe_relocations(&img, rel, 256, &nr) <= 0);

    free(buf);
}

int main(void) {
    printf("== w32 PE32+ parser (W32-2) ==\n");

    RUN(test_parse_minimal);
    RUN(test_rva_translation);

    RUN(test_reject_not_pe);
    RUN(test_reject_bad_e_lfanew);
    RUN(test_reject_machine_and_magic);
    RUN(test_reject_absurd_section_count);
    RUN(test_reject_raw_data_beyond_file);
    RUN(test_reject_bad_alignment);
    RUN(test_reject_too_many_directories);

    RUN(test_reject_write_execute_section);
    RUN(test_reject_efi_subsystem);
    RUN(test_reject_no_entry_point);

    RUN(test_relocations_walk);
    RUN(test_reject_bad_reloc_block);
    RUN(test_no_relocs_is_ok);

    RUN(test_reject_import_name_outside_sections);
    RUN(test_no_imports_is_ok);

    RUN(test_fuzz_truncation);
    RUN(test_fuzz_bitflips);

    RUN(test_real_bootx64_efi);

    printf("%s: %d/%d tests passed\n", failed ? "FAIL" : "PASS", passed, tn);
    return failed ? 1 : 0;
}
