/* elf.c — minimal ELF64 loader.
 *
 * Maps PT_LOAD segments from the file image into the current address space,
 * copies segment data, and zero-fills .bss. Returns the entry point.
 */

#include <stdint.h>
#include "kernel/proc/elf.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

#define ELF_TAG "[elf]  "

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (PAGE_SIZE - 1)

static int validate_elf(const struct elf64_ehdr *eh, uint64_t size) {
    if (size < sizeof(struct elf64_ehdr)) {
        kprintf(ELF_TAG "too small (%llu bytes)\n", (unsigned long long)size);
        return 0;
    }
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf(ELF_TAG "bad ELF magic\n");
        return 0;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf(ELF_TAG "not 64-bit\n");
        return 0;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf(ELF_TAG "not little-endian\n");
        return 0;
    }
    if (eh->e_machine != EM_X86_64) {
        kprintf(ELF_TAG "not x86_64 (e_machine=%u)\n", eh->e_machine);
        return 0;
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
        kprintf(ELF_TAG "unexpected phdr size %u\n", eh->e_phentsize);
        return 0;
    }
    return 1;
}

/*
 * Map and populate one PT_LOAD segment. Pages are mapped writable during copy
 * (regardless of the segment's p_flags) to simplify the load; proper read-only
 * protection is a TODO.
 */
static int load_segment(const struct elf64_phdr *ph, const uint8_t *image) {
    uint64_t seg_start = ph->p_vaddr & ~PAGE_MASK;          /* page-align down */
    uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_MASK) & ~PAGE_MASK;
    uint64_t npages    = (seg_end - seg_start) / PAGE_SIZE;

    /* Map each page with USER | PRESENT | WRITABLE — but only if not already
     * mapped. Two segments may share a page (e.g. text + rodata within the
     * same 4 KiB); we must not overwrite the first segment's mapping. */
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t virt = seg_start + i * PAGE_SIZE;
        if (paging_get_phys(virt) != 0) {
            continue;   /* page already mapped by a previous segment */
        }
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf(ELF_TAG "OOM mapping segment at 0x%llx\n",
                    (unsigned long long)virt);
            return 0;
        }
        paging_map(virt, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER);
    }

    /* Copy the file image (p_filesz bytes) into the mapped virtual address. */
    const uint8_t *src = image + ph->p_offset;
    uint8_t *dst = (uint8_t *)seg_start;
    for (uint64_t i = 0; i < ph->p_filesz; i++) {
        dst[ph->p_vaddr - seg_start + i] = src[i];
    }

    /* Zero-fill .bss (p_memsz - p_filesz bytes, already mapped + writable). */
    for (uint64_t i = ph->p_filesz; i < ph->p_memsz; i++) {
        dst[ph->p_vaddr - seg_start + i] = 0;
    }

    return 1;
}

uint64_t elf_load(const void *image, uint64_t size, uint64_t *out_brk) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    if (!validate_elf(eh, size)) {
        return 0;
    }

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff);

    int segs_loaded = 0;
    uint64_t highest_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (!load_segment(&phdrs[i], (const uint8_t *)image)) {
                return 0;
            }
            segs_loaded++;
            uint64_t end = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE_MASK) & ~PAGE_MASK;
            if (end > highest_end) highest_end = end;
        }
    }

    if (segs_loaded == 0) {
        kprintf(ELF_TAG "no PT_LOAD segments found\n");
        return 0;
    }

    if (out_brk) *out_brk = highest_end;

    kprintf(ELF_TAG "loaded %d segment(s), entry 0x%llx\n",
            segs_loaded, (unsigned long long)eh->e_entry);
    return eh->e_entry;
}
