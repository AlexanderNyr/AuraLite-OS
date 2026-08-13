/* pe.c — PE32+ image loader, WIN32_PLAN.md phase W32-3.
 *
 * Structure decoding is not repeated here: this file calls into
 * w32/src/w32_pe.c, the parser that already has a host unit test and a fuzz
 * corpus behind it (WIN32_PLAN.md D2).  Everything below is the part that has
 * to run in Ring 0 — allocating frames, mapping them, copying bytes and
 * applying relocations — and it follows kernel/proc/elf.c's habits so it
 * inherits a reviewed design rather than inventing one:
 *
 *   - final page flags come from the section characteristics, the way elf.c
 *     derives them from p_flags;
 *   - a page two sections share gets the union of their permissions, so a
 *     later section cannot silently drop an earlier one's write bit;
 *   - every newly allocated frame is zeroed before user space can see it, so
 *     padding in a partly-filled page never leaks old kernel data;
 *   - nothing is mapped below one page or at/above USER_VADDR_TOP.
 *
 * The one thing PE does that ELF here does not: an image whose preferred
 * ImageBase is unavailable can be moved, because it carries a relocation
 * table.  That is handled by choosing a base first and then fixing up.
 */

#include <stdint.h>
#include "kernel/proc/pe.h"
#include "kernel/proc/usercopy.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"
#include "w32/w32_pe.h"

#define PE_TAG "[pe]   "

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (PAGE_SIZE - 1)

/* Where a PE goes when its preferred base is unusable.  Chosen to sit well
 * clear of AuraLite's own fixed 0x40000000 program base and of the user stack
 * near the top of the canonical range. */
#define PE_FALLBACK_BASE 0x0000000180000000ULL

/* Upper bound on relocations the kernel will apply.  A static buffer keeps
 * pe_load() allocation-free on its error paths; see the relocation block. */
#define PE_MAX_KERNEL_RELOCS 4096

static inline void *phys_to_hhdm(uint64_t phys) {
    return (void *)(uintptr_t)(boot_get_hhdm_offset() + phys);
}

static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_MASK) & ~PAGE_MASK;
}

/* Section characteristics -> page flags.  Mirrors elf_page_flags().
 *
 * A section with neither READ nor WRITE nor EXECUTE set still has to be
 * mapped present, or the image's own layout would have holes in it; we simply
 * do not grant it more than PRESENT|USER|NX. */
static uint64_t pe_page_flags(uint32_t ch) {
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (ch & PE_SCN_MEM_WRITE)     flags |= PAGE_FLAG_WRITABLE;
    if (!(ch & PE_SCN_MEM_EXECUTE)) flags |= PAGE_FLAG_NO_EXEC;
    return flags;
}

/* Identical rule to elf.c's merge_page_flags(): permissions accumulate. */
static uint64_t merge_page_flags(uint64_t existing, uint64_t wanted) {
    uint64_t merged = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    int exec_existing = (existing & PAGE_FLAG_PRESENT) && !(existing & PAGE_FLAG_NO_EXEC);
    int exec_wanted   = !(wanted & PAGE_FLAG_NO_EXEC);

    if ((existing & PAGE_FLAG_WRITABLE) || (wanted & PAGE_FLAG_WRITABLE))
        merged |= PAGE_FLAG_WRITABLE;
    if (!(exec_existing || exec_wanted))
        merged |= PAGE_FLAG_NO_EXEC;
    return merged;
}

/* Map [virt, virt+len) with `flags`, allocating and zeroing as needed. */
static int map_range(uint64_t virt, uint64_t len, uint64_t flags) {
    uint64_t start = virt & ~PAGE_MASK;
    uint64_t end   = align_up_page(virt + len);

    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t old = paging_get_flags(p);
        uint64_t merged = merge_page_flags(old, flags);

        if (old & PAGE_FLAG_PRESENT) {
            if (paging_protect(p, merged) != 0) {
                kprintf(PE_TAG "failed to update flags at 0x%llx\n",
                        (unsigned long long)p);
                return 0;
            }
            continue;
        }
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf(PE_TAG "OOM mapping 0x%llx\n", (unsigned long long)p);
            return 0;
        }
        memset(phys_to_hhdm(phys), 0, PAGE_SIZE);
        paging_map(p, phys, merged);
    }
    return 1;
}

/* Copy into an already-mapped user range, page by page through the HHDM.
 * Same shape as elf.c's copy_into_user_mapping(). */
static int copy_into_mapping(uint64_t dst_virt, const uint8_t *src, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t virt = dst_virt + done;
        uint64_t phys = paging_get_phys(virt & ~PAGE_MASK);
        uint64_t off  = virt & PAGE_MASK;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > len - done) chunk = len - done;
        if (phys == 0) return 0;
        memcpy((uint8_t *)phys_to_hhdm(phys) + off, src + done, (size_t)chunk);
        done += chunk;
    }
    return 1;
}

/* Read/modify/write a 64-bit value in a mapped user page.  Relocation targets
 * are not guaranteed to be 8-byte aligned, and may straddle a page boundary,
 * so this goes byte at a time rather than casting a pointer. */
static int fixup_u64(uint64_t virt, uint64_t delta) {
    uint8_t bytes[8];

    for (int i = 0; i < 8; i++) {
        uint64_t v = virt + (uint64_t)i;
        uint64_t phys = paging_get_phys(v & ~PAGE_MASK);
        if (phys == 0) return 0;
        bytes[i] = *((uint8_t *)phys_to_hhdm(phys) + (v & PAGE_MASK));
    }
    uint64_t val = 0;
    for (int i = 7; i >= 0; i--) val = (val << 8) | bytes[i];
    val += delta;
    for (int i = 0; i < 8; i++) {
        uint64_t v = virt + (uint64_t)i;
        uint64_t phys = paging_get_phys(v & ~PAGE_MASK);
        if (phys == 0) return 0;
        *((uint8_t *)phys_to_hhdm(phys) + (v & PAGE_MASK)) =
            (uint8_t)((val >> (i * 8)) & 0xFF);
    }
    return 1;
}

/* Is [base, base+span) a usable user range with nothing already mapped in it? */
static int range_is_free(uint64_t base, uint64_t span) {
    if (base < PAGE_SIZE) return 0;
    if (span == 0) return 0;
    if (base + span < base) return 0;                 /* wrap */
    if (base + span >= USER_VADDR_TOP) return 0;

    for (uint64_t p = base & ~PAGE_MASK; p < align_up_page(base + span);
         p += PAGE_SIZE) {
        if (paging_get_flags(p) & PAGE_FLAG_PRESENT) return 0;
    }
    return 1;
}

int pe_image_probe(const void *image, uint64_t size) {
    const uint8_t *p = image;
    if (!p || size < 2) return 0;
    return (p[0] == 0x4D && p[1] == 0x5A);            /* "MZ" */
}

uint64_t pe_load(const void *image, uint64_t size, uint64_t *out_brk,
                 uint64_t *out_base) {
    pe_image_t img;
    int rc = pe_parse((const uint8_t *)image, (size_t)size, &img);
    if (rc != PE_OK) {
        kprintf(PE_TAG "parse failed: %s\n", pe_strerror(rc));
        return 0;
    }

    /* Policy — subsystem, W^X, machine — lives in one place and is already
     * unit-tested.  This is what refuses AuraLite's own BOOTX64.EFI. */
    rc = pe_check_loadable(&img);
    if (rc != PE_OK) {
        kprintf(PE_TAG "refused: %s (subsystem=%u)\n",
                pe_strerror(rc), img.subsystem);
        return 0;
    }

    uint64_t span = align_up_page(img.size_of_image);
    if (span == 0 || span >= USER_VADDR_TOP) {
        kprintf(PE_TAG "implausible SizeOfImage 0x%x\n", img.size_of_image);
        return 0;
    }

    /* Choose a base.  Prefer the image's own; fall back only if we may
     * relocate, which requires a relocation table that has not been stripped. */
    uint64_t base = img.image_base;
    int need_reloc = 0;

    if (!range_is_free(base, span)) {
        int has_relocs = (img.num_directories > PE_DIR_BASERELOC) &&
                         img.dir[PE_DIR_BASERELOC].rva != 0 &&
                         img.dir[PE_DIR_BASERELOC].size != 0;
        int stripped = (img.characteristics & 0x0001u) != 0;   /* RELOCS_STRIPPED */

        if (!has_relocs || stripped) {
            kprintf(PE_TAG "base 0x%llx unavailable and image is not "
                    "relocatable\n", (unsigned long long)base);
            return 0;
        }
        if (!range_is_free(PE_FALLBACK_BASE, span)) {
            kprintf(PE_TAG "no free range for a %llu KiB image\n",
                    (unsigned long long)(span / 1024));
            return 0;
        }
        base = PE_FALLBACK_BASE;
        need_reloc = 1;
    }

    /* --- headers ---------------------------------------------------------
     * A PE image expects its own headers to be readable at its base: code
     * reaches them through the module handle.  Map them read-only. */
    uint64_t hdr_len = img.size_of_headers;
    if (hdr_len > size) hdr_len = size;
    if (hdr_len) {
        if (!map_range(base, hdr_len,
                       PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_NO_EXEC))
            return 0;
        if (!copy_into_mapping(base, (const uint8_t *)image, hdr_len)) {
            kprintf(PE_TAG "failed to copy headers\n");
            return 0;
        }
    }

    /* --- sections -------------------------------------------------------- */
    int mapped = 0;
    uint64_t highest = base + align_up_page(hdr_len);

    for (uint16_t i = 0; i < img.section_count; i++) {
        pe_section_t s;
        if (pe_get_section(&img, i, &s) != PE_OK) {
            kprintf(PE_TAG "section %u unreadable\n", i);
            return 0;
        }

        /* A section's memory size is the larger of its virtual and raw sizes:
         * .bss-style sections declare virtual_size with no file bytes, and
         * some linkers round raw_size up past virtual_size. */
        uint64_t vsz = s.virtual_size;
        if (vsz < s.raw_size) vsz = s.raw_size;
        if (vsz == 0) continue;

        uint64_t va = base + s.virtual_address;
        if (va < PAGE_SIZE || va + vsz >= USER_VADDR_TOP || va + vsz < va) {
            kprintf(PE_TAG "section %u out of user range\n", i);
            return 0;
        }
        /* The section must lie inside the image the header declared, or the
         * loader would map beyond what SizeOfImage promised. */
        if ((uint64_t)s.virtual_address + vsz > span) {
            kprintf(PE_TAG "section %u exceeds SizeOfImage\n", i);
            return 0;
        }

        if (!map_range(va, vsz, pe_page_flags(s.characteristics))) return 0;

        if (s.raw_size) {
            /* pe_parse() has already proven raw_offset+raw_size is in the
             * file; this is belt and braces at the point of use. */
            if ((uint64_t)s.raw_offset + s.raw_size > size) {
                kprintf(PE_TAG "section %u raw data out of file\n", i);
                return 0;
            }
            if (!copy_into_mapping(va, (const uint8_t *)image + s.raw_offset,
                                   s.raw_size)) {
                kprintf(PE_TAG "failed to copy section %u\n", i);
                return 0;
            }
        }
        /* Bytes beyond raw_size are already zero: map_range() zeroes every
         * frame it allocates, which is also what makes .bss correct. */

        uint64_t end = align_up_page(va + vsz);
        if (end > highest) highest = end;
        mapped++;
    }

    if (mapped == 0) {
        kprintf(PE_TAG "no loadable sections\n");
        return 0;
    }

    /* --- relocations -----------------------------------------------------
     *
     * pe_relocations() fills the caller's buffer and reports the true total,
     * so a table larger than the buffer would need either a heap allocation
     * or a paging API the parser does not have.  Rather than invent one in
     * Ring 0, the loader caps what it will relocate at a buffer it owns and
     * refuses anything larger.  PE_MAX_KERNEL_RELOCS is far above what a
     * freestanding mingw-w64 .exe produces (typically tens of entries), and
     * refusing is safe where truncating silently would not be. */
    if (need_reloc) {
        static pe_reloc_t relocs[PE_MAX_KERNEL_RELOCS];
        uint64_t delta = base - img.image_base;
        size_t total = 0;

        rc = pe_relocations(&img, relocs,
                            sizeof relocs / sizeof relocs[0], &total);
        if (rc != PE_OK) {
            kprintf(PE_TAG "relocation table malformed: %s\n", pe_strerror(rc));
            return 0;
        }
        if (total > sizeof relocs / sizeof relocs[0]) {
            kprintf(PE_TAG "too many relocations (%llu > %u)\n",
                    (unsigned long long)total,
                    (unsigned)(sizeof relocs / sizeof relocs[0]));
            return 0;
        }

        for (size_t k = 0; k < total; k++) {
            if (relocs[k].type != PE_REL_DIR64) {
                kprintf(PE_TAG "unsupported relocation type %u\n",
                        relocs[k].type);
                return 0;
            }
            if ((uint64_t)relocs[k].rva + 8 > span) {
                kprintf(PE_TAG "relocation outside image\n");
                return 0;
            }
            if (!fixup_u64(base + relocs[k].rva, delta)) {
                kprintf(PE_TAG "relocation target not mapped\n");
                return 0;
            }
        }
        kprintf(PE_TAG "relocated %llu entries by 0x%llx\n",
                (unsigned long long)total, (unsigned long long)delta);
    }

    uint64_t entry = base + img.entry_point_rva;
    if (entry < PAGE_SIZE || entry >= USER_VADDR_TOP) {
        kprintf(PE_TAG "entry point out of range\n");
        return 0;
    }

    if (out_brk)  *out_brk  = highest;
    if (out_base) *out_base = base;

    kprintf(PE_TAG "loaded %d section(s) at 0x%llx, entry 0x%llx%s\n",
            mapped, (unsigned long long)base, (unsigned long long)entry,
            need_reloc ? " (relocated)" : "");
    return entry;
}
