/* w32_pe.h — PE32+ (COFF) image parsing for the w32 personality.
 *
 * WIN32_PLAN.md phase W32-2.
 *
 * This is a *parser*, not a loader: it reads a byte buffer that is entirely
 * under an attacker's control and produces validated, bounds-checked views of
 * the structures a loader will later need.  It never allocates, never maps
 * anything and never dereferences a file offset it has not first proven lies
 * inside the buffer.  Phase W32-3 builds the kernel loader on top of it, and
 * keeping the parsing separable is what lets that happen with the risky part
 * already tested (WIN32_PLAN.md D2).
 *
 * All structure layouts here are from the published PE/COFF specification.
 * They are facts about a file format, written from the spec, not copied from
 * any SDK header (WIN32_PLAN.md section 1.3).
 *
 * Endianness: PE is little-endian.  Fields are read byte-by-byte through the
 * rd16/rd32/rd64 helpers rather than by casting a struct over the buffer, so
 * the parser is correct regardless of host endianness and cannot fault on an
 * unaligned access.
 */

#ifndef AURALITE_W32_PE_H
#define AURALITE_W32_PE_H

#include <stddef.h>
#include <stdint.h>

/* ---- Signatures and constants (PE/COFF specification) ------------------ */

#define PE_DOS_MAGIC            0x5A4Du       /* "MZ" */
#define PE_NT_SIGNATURE         0x00004550u   /* "PE\0\0" */
#define PE_OPT_MAGIC_PE32       0x010Bu
#define PE_OPT_MAGIC_PE32PLUS   0x020Bu

#define PE_MACHINE_AMD64        0x8664u
#define PE_MACHINE_I386         0x014Cu

/* IMAGE_FILE_HEADER.Characteristics */
#define PE_FILE_EXECUTABLE_IMAGE  0x0002u
#define PE_FILE_LARGE_ADDR_AWARE  0x0020u
#define PE_FILE_DLL               0x2000u

/* Subsystems.  The EFI ones matter to us in the negative: AuraLite's own
 * BOOTX64.EFI is a PE, and it must never be launchable as a process. */
#define PE_SUBSYSTEM_NATIVE            1u
#define PE_SUBSYSTEM_WINDOWS_GUI       2u
#define PE_SUBSYSTEM_WINDOWS_CUI       3u
#define PE_SUBSYSTEM_EFI_APPLICATION  10u
#define PE_SUBSYSTEM_EFI_BOOT_DRIVER  11u
#define PE_SUBSYSTEM_EFI_RUNTIME_DRV  12u
#define PE_SUBSYSTEM_EFI_ROM          13u

/* IMAGE_SECTION_HEADER.Characteristics */
#define PE_SCN_CNT_CODE            0x00000020u
#define PE_SCN_CNT_INITIALIZED     0x00000040u
#define PE_SCN_CNT_UNINITIALIZED   0x00000080u
#define PE_SCN_MEM_DISCARDABLE     0x02000000u
#define PE_SCN_MEM_EXECUTE         0x20000000u
#define PE_SCN_MEM_READ            0x40000000u
#define PE_SCN_MEM_WRITE           0x80000000u

/* Data directory indices. */
#define PE_DIR_EXPORT      0
#define PE_DIR_IMPORT      1
#define PE_DIR_RESOURCE    2
#define PE_DIR_BASERELOC   5
#define PE_DIR_TLS         9
#define PE_DIR_IAT        12
#define PE_DIR_DELAY_IMPORT 13
#define PE_NUM_DIRECTORIES 16

/* Base relocation types we care about for x86-64. */
#define PE_REL_ABSOLUTE    0
#define PE_REL_DIR64      10

/* ---- Return codes ------------------------------------------------------ */

#define PE_OK                  0
#define PE_ERR_ARG           (-1)
#define PE_ERR_TRUNCATED     (-2)   /* a field/table runs past end of buffer */
#define PE_ERR_NOT_PE        (-3)   /* missing MZ or PE\0\0 */
#define PE_ERR_NOT_PE32PLUS  (-4)   /* PE32 (32-bit) or unknown optional magic */
#define PE_ERR_MACHINE       (-5)   /* not AMD64 */
#define PE_ERR_MALFORMED     (-6)   /* self-inconsistent: overlapping/absurd */
#define PE_ERR_UNSUPPORTED   (-7)   /* well-formed but out of scope */

/* ---- Parsed views ------------------------------------------------------ */

typedef struct {
    char     name[9];          /* NUL-terminated; PE names are 8 bytes, no NUL
                                * required in the file, so we add one. */
    uint32_t virtual_size;
    uint32_t virtual_address;  /* RVA */
    uint32_t raw_size;
    uint32_t raw_offset;       /* file offset */
    uint32_t characteristics;
} pe_section_t;

typedef struct {
    uint32_t rva;
    uint32_t size;
} pe_dir_t;

typedef struct {
    const uint8_t *data;       /* borrowed; the caller owns the buffer */
    size_t         size;

    uint32_t  pe_offset;       /* e_lfanew */
    uint16_t  machine;
    uint16_t  section_count;
    uint16_t  characteristics;
    uint16_t  opt_magic;
    uint16_t  subsystem;
    uint16_t  dll_characteristics;

    uint64_t  image_base;
    uint32_t  size_of_image;
    uint32_t  size_of_headers;
    uint32_t  entry_point_rva;
    uint32_t  section_alignment;
    uint32_t  file_alignment;

    uint32_t  num_directories;
    pe_dir_t  dir[PE_NUM_DIRECTORIES];

    uint32_t  section_table_offset;
} pe_image_t;

/* One resolved import: which DLL, and which symbol from it. */
typedef struct {
    char     dll[64];          /* truncated with NUL if longer */
    char     name[128];        /* empty when imported by ordinal */
    uint16_t ordinal;          /* valid when by_ordinal */
    int      by_ordinal;
    uint32_t iat_rva;          /* where the resolved address must be written */
} pe_import_t;

/* One base relocation to apply. */
typedef struct {
    uint32_t rva;              /* location to fix up */
    uint16_t type;             /* PE_REL_* */
} pe_reloc_t;

/* ---- API --------------------------------------------------------------- */

/* Parse headers and validate the section table.  Does not walk imports or
 * relocations; those are separate so a caller pays only for what it needs. */
int pe_parse(const uint8_t *data, size_t size, pe_image_t *out);

/* Read section `i` (0-based).  Bounds-checked against section_count and the
 * buffer. */
int pe_get_section(const pe_image_t *img, uint16_t i, pe_section_t *out);

/* Translate an RVA to a file offset, verifying the whole [rva, rva+len) range
 * lies inside one section's raw data.  This is the single most security-
 * relevant function here: every table walk goes through it. */
int pe_rva_to_offset(const pe_image_t *img, uint32_t rva, uint32_t len,
                     uint32_t *offset_out);

/* Walk the import directory.  Writes up to `max` entries and always reports
 * the total it found in *count, so a caller can size a buffer.  Returns PE_OK
 * even when there is no import directory (count = 0). */
int pe_imports(const pe_image_t *img, pe_import_t *out, size_t max,
               size_t *count);

/* Walk the base relocation directory; same contract as pe_imports(). */
int pe_relocations(const pe_image_t *img, pe_reloc_t *out, size_t max,
                   size_t *count);

/* Is this image loadable as a w32 process?  Separated from pe_parse() so the
 * kernel can apply policy without re-parsing, and so the EFI refusal has one
 * home.  Returns PE_OK or a PE_ERR_* explaining the refusal. */
int pe_check_loadable(const pe_image_t *img);

/* Human-readable name for a return code, for diagnostics and tests. */
const char *pe_strerror(int err);

#endif /* AURALITE_W32_PE_H */
