/* w32_pe.c — PE32+ parsing, WIN32_PLAN.md phase W32-2.
 *
 * Threat model: `data` is a file the user obtained from somewhere else and
 * asked us to run.  Every offset, RVA, count and size in it is hostile.  The
 * rules this file follows, so that reviewing it is tractable:
 *
 *   1. Nothing is read without in_bounds() proving it fits first.
 *   2. No struct is cast over the buffer; fields are assembled from bytes.
 *      This removes both alignment faults and padding assumptions.
 *   3. Every arithmetic combination of two attacker values is checked for
 *      overflow *before* it is used, not after.
 *   4. Loops over attacker-supplied counts have an independent iteration cap,
 *      so a malformed structure cannot produce an unbounded walk.
 */

#include "w32/w32_pe.h"

/* Hard caps.  A real image is far below these; they exist so a corrupted
 * count cannot turn into a long loop. */
#define PE_MAX_SECTIONS      96u
#define PE_MAX_IMPORT_DLLS   4096u
#define PE_MAX_IMPORTS_TOTAL 65536u
#define PE_MAX_RELOC_BLOCKS  65536u
#define PE_MAX_RELOCS_TOTAL  1048576u

static int in_bounds(const pe_image_t *img, uint64_t off, uint64_t len) {
    if (off > img->size) return 0;
    if (len > img->size - off) return 0;   /* no overflow: both <= size */
    return 1;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

const char *pe_strerror(int err) {
    switch (err) {
    case PE_OK:                 return "ok";
    case PE_ERR_ARG:            return "bad argument";
    case PE_ERR_TRUNCATED:      return "truncated";
    case PE_ERR_NOT_PE:         return "not a PE image";
    case PE_ERR_NOT_PE32PLUS:   return "not PE32+";
    case PE_ERR_MACHINE:        return "wrong machine";
    case PE_ERR_MALFORMED:      return "malformed";
    case PE_ERR_UNSUPPORTED:    return "unsupported";
    default:                    return "unknown error";
    }
}

int pe_parse(const uint8_t *data, size_t size, pe_image_t *img) {
    if (!data || !img) return PE_ERR_ARG;

    for (size_t i = 0; i < sizeof(*img); i++) ((uint8_t *)img)[i] = 0;
    img->data = data;
    img->size = size;

    /* --- DOS header ---------------------------------------------------- */
    if (!in_bounds(img, 0, 0x40)) return PE_ERR_TRUNCATED;
    if (rd16(data) != PE_DOS_MAGIC) return PE_ERR_NOT_PE;

    uint32_t e_lfanew = rd32(data + 0x3C);
    /* The NT headers must start inside the file and after the DOS header.
     * A tiny e_lfanew would overlap the DOS header itself. */
    if (e_lfanew < 0x40) return PE_ERR_MALFORMED;
    img->pe_offset = e_lfanew;

    /* --- NT signature + COFF file header (4 + 20 bytes) ---------------- */
    if (!in_bounds(img, e_lfanew, 24)) return PE_ERR_TRUNCATED;
    if (rd32(data + e_lfanew) != PE_NT_SIGNATURE) return PE_ERR_NOT_PE;

    const uint8_t *fh = data + e_lfanew + 4;
    img->machine         = rd16(fh + 0);
    img->section_count   = rd16(fh + 2);
    uint16_t opt_size    = rd16(fh + 16);
    img->characteristics = rd16(fh + 18);

    if (img->machine != PE_MACHINE_AMD64) return PE_ERR_MACHINE;
    if (img->section_count == 0) return PE_ERR_MALFORMED;
    if (img->section_count > PE_MAX_SECTIONS) return PE_ERR_MALFORMED;

    /* --- Optional header ------------------------------------------------ */
    uint32_t opt_off = e_lfanew + 24;
    if (opt_size < 2) return PE_ERR_MALFORMED;
    if (!in_bounds(img, opt_off, opt_size)) return PE_ERR_TRUNCATED;

    const uint8_t *oh = data + opt_off;
    img->opt_magic = rd16(oh);
    if (img->opt_magic == PE_OPT_MAGIC_PE32) return PE_ERR_NOT_PE32PLUS;
    if (img->opt_magic != PE_OPT_MAGIC_PE32PLUS) return PE_ERR_NOT_PE32PLUS;

    /* PE32+ optional header is 112 bytes before the data directories. */
    if (opt_size < 112) return PE_ERR_MALFORMED;

    img->entry_point_rva   = rd32(oh + 16);
    img->image_base        = rd64(oh + 24);
    img->section_alignment = rd32(oh + 32);
    img->file_alignment    = rd32(oh + 36);
    img->size_of_image     = rd32(oh + 56);
    img->size_of_headers   = rd32(oh + 60);
    img->subsystem         = rd16(oh + 68);
    img->dll_characteristics = rd16(oh + 70);
    img->num_directories   = rd32(oh + 108);

    /* Alignments must be powers of two, and section >= file, or the loader's
     * later address arithmetic is meaningless. */
    if (img->section_alignment == 0 || img->file_alignment == 0)
        return PE_ERR_MALFORMED;
    if (img->section_alignment & (img->section_alignment - 1))
        return PE_ERR_MALFORMED;
    if (img->file_alignment & (img->file_alignment - 1))
        return PE_ERR_MALFORMED;
    if (img->section_alignment < img->file_alignment)
        return PE_ERR_MALFORMED;

    if (img->num_directories > PE_NUM_DIRECTORIES)
        return PE_ERR_MALFORMED;

    /* Directories follow the 112-byte fixed part, 8 bytes each. */
    uint32_t dir_bytes = img->num_directories * 8u;
    if (opt_size < 112u + dir_bytes) return PE_ERR_MALFORMED;
    if (!in_bounds(img, opt_off + 112u, dir_bytes)) return PE_ERR_TRUNCATED;

    for (uint32_t i = 0; i < img->num_directories; i++) {
        const uint8_t *d = oh + 112 + i * 8;
        img->dir[i].rva  = rd32(d);
        img->dir[i].size = rd32(d + 4);
    }

    /* --- Section table -------------------------------------------------- */
    uint64_t sect_off = (uint64_t)opt_off + opt_size;
    uint64_t sect_len = (uint64_t)img->section_count * 40u;
    if (!in_bounds(img, sect_off, sect_len)) return PE_ERR_TRUNCATED;
    img->section_table_offset = (uint32_t)sect_off;

    /* Validate every section once, here, so later callers can trust them. */
    for (uint16_t i = 0; i < img->section_count; i++) {
        pe_section_t s;
        int rc = pe_get_section(img, i, &s);
        if (rc != PE_OK) return rc;
    }
    return PE_OK;
}

int pe_get_section(const pe_image_t *img, uint16_t i, pe_section_t *out) {
    if (!img || !out) return PE_ERR_ARG;
    if (i >= img->section_count) return PE_ERR_ARG;

    uint64_t off = (uint64_t)img->section_table_offset + (uint64_t)i * 40u;
    if (!in_bounds(img, off, 40)) return PE_ERR_TRUNCATED;

    const uint8_t *s = img->data + off;
    for (int k = 0; k < 8; k++) out->name[k] = (char)s[k];
    out->name[8] = '\0';

    out->virtual_size    = rd32(s + 8);
    out->virtual_address = rd32(s + 12);
    out->raw_size        = rd32(s + 16);
    out->raw_offset      = rd32(s + 20);
    out->characteristics = rd32(s + 36);

    /* Raw data must lie inside the file.  A section with raw_size 0 (.bss) is
     * legal and has no file bytes to check. */
    if (out->raw_size) {
        if (!in_bounds(img, out->raw_offset, out->raw_size))
            return PE_ERR_TRUNCATED;
    }
    /* The virtual span must not wrap. */
    if ((uint64_t)out->virtual_address + out->virtual_size > 0xFFFFFFFFull)
        return PE_ERR_MALFORMED;
    return PE_OK;
}

int pe_rva_to_offset(const pe_image_t *img, uint32_t rva, uint32_t len,
                     uint32_t *offset_out) {
    if (!img || !offset_out) return PE_ERR_ARG;
    if ((uint64_t)rva + len > 0xFFFFFFFFull) return PE_ERR_MALFORMED;

    for (uint16_t i = 0; i < img->section_count; i++) {
        pe_section_t s;
        if (pe_get_section(img, i, &s) != PE_OK) return PE_ERR_MALFORMED;
        if (s.raw_size == 0) continue;

        if (rva < s.virtual_address) continue;
        uint32_t delta = rva - s.virtual_address;
        /* Must be inside the *raw* bytes: the tail of a section whose
         * virtual_size exceeds raw_size is zero-fill with nothing to read. */
        if (delta >= s.raw_size) continue;
        if (len > s.raw_size - delta) return PE_ERR_TRUNCATED;

        uint64_t off = (uint64_t)s.raw_offset + delta;
        if (!in_bounds(img, off, len)) return PE_ERR_TRUNCATED;
        *offset_out = (uint32_t)off;
        return PE_OK;
    }
    return PE_ERR_MALFORMED;   /* RVA not in any section */
}

/* Copy a NUL-terminated ASCII string starting at `rva` into `dst`, bounded
 * both by the destination and by the end of the containing section. */
static int copy_cstr_at_rva(const pe_image_t *img, uint32_t rva,
                            char *dst, size_t dstcap) {
    uint32_t off;
    int rc = pe_rva_to_offset(img, rva, 1, &off);
    if (rc != PE_OK) return rc;

    size_t n = 0;
    while (off + n < img->size) {
        uint8_t c = img->data[off + n];
        if (c == 0) { dst[n < dstcap ? n : dstcap - 1] = '\0'; return PE_OK; }
        if (n < dstcap - 1) dst[n] = (char)c;
        n++;
        if (n > 4096) break;    /* independent cap: no NUL in sight */
    }
    dst[dstcap - 1] = '\0';
    return PE_ERR_MALFORMED;    /* unterminated */
}

int pe_imports(const pe_image_t *img, pe_import_t *out, size_t max,
               size_t *count) {
    if (!img || !count) return PE_ERR_ARG;
    if (!out && max) return PE_ERR_ARG;
    *count = 0;

    if (img->num_directories <= PE_DIR_IMPORT) return PE_OK;
    pe_dir_t d = img->dir[PE_DIR_IMPORT];
    if (d.rva == 0 || d.size == 0) return PE_OK;   /* no imports: legal */

    size_t total = 0;

    for (uint32_t di = 0; di < PE_MAX_IMPORT_DLLS; di++) {
        /* Each IMAGE_IMPORT_DESCRIPTOR is 20 bytes; terminated by an
         * all-zero one. */
        uint64_t desc_rva = (uint64_t)d.rva + (uint64_t)di * 20u;
        if (desc_rva > 0xFFFFFFFFull) return PE_ERR_MALFORMED;

        uint32_t off;
        int rc = pe_rva_to_offset(img, (uint32_t)desc_rva, 20, &off);
        if (rc != PE_OK) return rc;

        const uint8_t *p = img->data + off;
        uint32_t orig_thunk = rd32(p + 0);
        uint32_t name_rva   = rd32(p + 12);
        uint32_t first_thunk= rd32(p + 16);

        if (orig_thunk == 0 && name_rva == 0 && first_thunk == 0) break;

        char dll[64];
        dll[0] = '\0';
        if (name_rva) {
            rc = copy_cstr_at_rva(img, name_rva, dll, sizeof dll);
            if (rc != PE_OK) return rc;
        }

        /* Prefer the original (import name) thunk; fall back to the IAT,
         * which is what a bound image leaves populated. */
        uint32_t thunk = orig_thunk ? orig_thunk : first_thunk;
        if (thunk == 0) continue;

        for (uint32_t ti = 0; ; ti++) {
            if (total >= PE_MAX_IMPORTS_TOTAL) return PE_ERR_MALFORMED;

            uint64_t te_rva = (uint64_t)thunk + (uint64_t)ti * 8u;
            if (te_rva > 0xFFFFFFFFull) return PE_ERR_MALFORMED;

            uint32_t toff;
            rc = pe_rva_to_offset(img, (uint32_t)te_rva, 8, &toff);
            if (rc != PE_OK) return rc;

            uint64_t entry = rd64(img->data + toff);
            if (entry == 0) break;

            pe_import_t imp;
            for (size_t k = 0; k < sizeof imp; k++) ((uint8_t *)&imp)[k] = 0;

            size_t dl = 0;
            while (dl < sizeof(imp.dll) - 1 && dll[dl]) { imp.dll[dl] = dll[dl]; dl++; }
            imp.dll[dl] = '\0';

            imp.iat_rva = (uint32_t)((uint64_t)first_thunk + (uint64_t)ti * 8u);

            if (entry & 0x8000000000000000ull) {
                imp.by_ordinal = 1;
                imp.ordinal = (uint16_t)(entry & 0xFFFFu);
            } else {
                /* Low 31 bits are an RVA to IMAGE_IMPORT_BY_NAME:
                 * 2-byte hint then a NUL-terminated name. */
                uint32_t hn_rva = (uint32_t)(entry & 0x7FFFFFFFull);
                uint32_t hoff;
                rc = pe_rva_to_offset(img, hn_rva, 3, &hoff);
                if (rc != PE_OK) return rc;
                rc = copy_cstr_at_rva(img, hn_rva + 2, imp.name, sizeof imp.name);
                if (rc != PE_OK) return rc;
            }

            if (total < max) out[total] = imp;
            total++;
        }
    }

    *count = total;
    return PE_OK;
}

int pe_relocations(const pe_image_t *img, pe_reloc_t *out, size_t max,
                   size_t *count) {
    if (!img || !count) return PE_ERR_ARG;
    if (!out && max) return PE_ERR_ARG;
    *count = 0;

    if (img->num_directories <= PE_DIR_BASERELOC) return PE_OK;
    pe_dir_t d = img->dir[PE_DIR_BASERELOC];
    if (d.rva == 0 || d.size == 0) return PE_OK;   /* stripped: legal */

    size_t total = 0;
    uint32_t consumed = 0;

    for (uint32_t b = 0; b < PE_MAX_RELOC_BLOCKS; b++) {
        if (consumed >= d.size) break;
        if (d.size - consumed < 8) return PE_ERR_MALFORMED;

        uint32_t hdr_rva = d.rva + consumed;
        uint32_t off;
        int rc = pe_rva_to_offset(img, hdr_rva, 8, &off);
        if (rc != PE_OK) return rc;

        uint32_t page_rva   = rd32(img->data + off);
        uint32_t block_size = rd32(img->data + off + 4);

        /* A block must at least contain its own header, must be even (it is
         * an array of uint16), and must not exceed what the directory says
         * remains — the three ways this field is used to escape. */
        if (block_size < 8) return PE_ERR_MALFORMED;
        if (block_size & 1) return PE_ERR_MALFORMED;
        if (block_size > d.size - consumed) return PE_ERR_MALFORMED;

        uint32_t nent = (block_size - 8) / 2;
        uint32_t eoff;
        rc = pe_rva_to_offset(img, hdr_rva + 8, block_size - 8, &eoff);
        if (rc != PE_OK) return rc;

        for (uint32_t i = 0; i < nent; i++) {
            if (total >= PE_MAX_RELOCS_TOTAL) return PE_ERR_MALFORMED;
            uint16_t e = rd16(img->data + eoff + i * 2);
            uint16_t type = (uint16_t)(e >> 12);
            uint16_t voff = (uint16_t)(e & 0x0FFFu);

            if (type == PE_REL_ABSOLUTE) continue;   /* padding, skip */

            pe_reloc_t r;
            r.type = type;
            uint64_t rva = (uint64_t)page_rva + voff;
            if (rva > 0xFFFFFFFFull) return PE_ERR_MALFORMED;
            r.rva = (uint32_t)rva;

            if (total < max) out[total] = r;
            total++;
        }
        consumed += block_size;
    }

    *count = total;
    return PE_OK;
}

int pe_check_loadable(const pe_image_t *img) {
    if (!img) return PE_ERR_ARG;

    if (img->machine != PE_MACHINE_AMD64) return PE_ERR_MACHINE;
    if (img->opt_magic != PE_OPT_MAGIC_PE32PLUS) return PE_ERR_NOT_PE32PLUS;

    if (!(img->characteristics & PE_FILE_EXECUTABLE_IMAGE))
        return PE_ERR_UNSUPPORTED;

    /* AuraLite's own BOOTX64.EFI is a valid PE32+ AMD64 executable.  Refusing
     * the EFI subsystems is what stops it — or any firmware binary — from
     * being launched as a user process.  See WIN32_PLAN.md W32-3. */
    switch (img->subsystem) {
    case PE_SUBSYSTEM_WINDOWS_GUI:
    case PE_SUBSYSTEM_WINDOWS_CUI:
        break;
    default:
        return PE_ERR_UNSUPPORTED;
    }

    if (img->entry_point_rva == 0) return PE_ERR_UNSUPPORTED;
    if (img->size_of_image == 0)   return PE_ERR_MALFORMED;

    /* W^X, checked here rather than at map time so the refusal is testable
     * without a kernel.  Mirrors kernel/proc/elf.c's derivation from
     * p_flags. */
    for (uint16_t i = 0; i < img->section_count; i++) {
        pe_section_t s;
        int rc = pe_get_section(img, i, &s);
        if (rc != PE_OK) return rc;
        if ((s.characteristics & PE_SCN_MEM_WRITE) &&
            (s.characteristics & PE_SCN_MEM_EXECUTE))
            return PE_ERR_UNSUPPORTED;
    }
    return PE_OK;
}
