/* elf.c — minimal ELF64 loader.
 *
 * Maps PT_LOAD segments from the file image into the current address space,
 * copies segment data, zero-fills .bss, and applies conservative final page
 * protections derived from ELF p_flags.
 */

#include <stdint.h>
#include "kernel/proc/elf.h"
#include "kernel/proc/usercopy.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

#define ELF_TAG "[elf]  "

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (PAGE_SIZE - 1)

static inline void *phys_to_hhdm(uint64_t phys) {
    return (void *)(uintptr_t)(limine_get_hhdm_offset() + phys);
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    uint64_t r = a + b;
    if (r < a) return 1;
    if (out) *out = r;
    return 0;
}

static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_MASK) & ~PAGE_MASK;
}

static int validate_elf(const struct elf64_ehdr *eh, uint64_t size) {
    uint64_t phdr_bytes;

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
    if (eh->e_ehsize < sizeof(struct elf64_ehdr)) {
        kprintf(ELF_TAG "unexpected ehdr size %u\n", eh->e_ehsize);
        return 0;
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) {
        kprintf(ELF_TAG "unexpected phdr size %u\n", eh->e_phentsize);
        return 0;
    }
    if (eh->e_entry == 0 || eh->e_entry >= USER_VADDR_TOP) {
        kprintf(ELF_TAG "bad entry address 0x%llx\n",
                (unsigned long long)eh->e_entry);
        return 0;
    }
    phdr_bytes = (uint64_t)eh->e_phnum * (uint64_t)sizeof(struct elf64_phdr);
    if (eh->e_phoff > size || phdr_bytes > size - eh->e_phoff) {
        kprintf(ELF_TAG "program headers out of file bounds\n");
        return 0;
    }
    return 1;
}

static int validate_segment(const struct elf64_phdr *ph, uint64_t image_size) {
    uint64_t file_end;
    uint64_t mem_end;

    if (ph->p_memsz < ph->p_filesz) {
        kprintf(ELF_TAG "segment memsz < filesz\n");
        return 0;
    }
    if (add_overflow_u64(ph->p_offset, ph->p_filesz, &file_end) || file_end > image_size) {
        kprintf(ELF_TAG "segment file range out of bounds\n");
        return 0;
    }
    if (ph->p_vaddr < PAGE_SIZE) {
        kprintf(ELF_TAG "refusing low user mapping at 0x%llx\n",
                (unsigned long long)ph->p_vaddr);
        return 0;
    }
    if (ph->p_memsz == 0) return 1;
    if (add_overflow_u64(ph->p_vaddr, ph->p_memsz - 1, &mem_end) || mem_end >= USER_VADDR_TOP) {
        kprintf(ELF_TAG "segment virtual range out of bounds\n");
        return 0;
    }
    return 1;
}

static uint64_t elf_page_flags(uint32_t p_flags) {
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (p_flags & PF_W) flags |= PAGE_FLAG_WRITABLE;
    if (!(p_flags & PF_X)) flags |= PAGE_FLAG_NO_EXEC;
    return flags;
}

static uint64_t merge_page_flags(uint64_t existing, uint64_t wanted) {
    uint64_t merged = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    int exec_existing = (existing & PAGE_FLAG_PRESENT) && !(existing & PAGE_FLAG_NO_EXEC);
    int exec_wanted   = !(wanted & PAGE_FLAG_NO_EXEC);

    if ((existing & PAGE_FLAG_WRITABLE) || (wanted & PAGE_FLAG_WRITABLE)) {
        merged |= PAGE_FLAG_WRITABLE;
    }
    if (!(exec_existing || exec_wanted)) {
        merged |= PAGE_FLAG_NO_EXEC;
    }
    return merged;
}

static int zero_new_user_page(uint64_t phys) {
    void *dst = phys_to_hhdm(phys);
    if (!dst) return 0;
    memset(dst, 0, PAGE_SIZE);
    return 1;
}

static int copy_into_user_mapping(uint64_t dst_virt, const uint8_t *src, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t virt = dst_virt + done;
        uint64_t page_virt = virt & ~PAGE_MASK;
        uint64_t phys = paging_get_phys(page_virt);
        uint64_t page_off = virt & PAGE_MASK;
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - done) chunk = len - done;
        if (phys == 0) return 0;
        memcpy((uint8_t *)phys_to_hhdm(phys) + page_off, src + done, (size_t)chunk);
        done += chunk;
    }
    return 1;
}

static int zero_user_mapping(uint64_t dst_virt, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t virt = dst_virt + done;
        uint64_t page_virt = virt & ~PAGE_MASK;
        uint64_t phys = paging_get_phys(page_virt);
        uint64_t page_off = virt & PAGE_MASK;
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - done) chunk = len - done;
        if (phys == 0) return 0;
        memset((uint8_t *)phys_to_hhdm(phys) + page_off, 0, (size_t)chunk);
        done += chunk;
    }
    return 1;
}

/*
 * Map and populate one PT_LOAD segment.
 *
 * New user frames are zeroed before exposure so userspace never inherits stale
 * contents from old kernel/user allocations through padding bytes in partially
 * filled pages.
 */
static int load_segment(const struct elf64_phdr *ph, const uint8_t *image,
                        uint64_t image_size) {
    uint64_t seg_start, seg_end, npages;
    uint64_t final_flags;

    if (!validate_segment(ph, image_size)) {
        return 0;
    }
    if (ph->p_memsz == 0) {
        return 1;
    }

    seg_start = ph->p_vaddr & ~PAGE_MASK;
    seg_end   = align_up_page(ph->p_vaddr + ph->p_memsz);
    npages    = (seg_end - seg_start) / PAGE_SIZE;
    final_flags = elf_page_flags(ph->p_flags);

    for (uint64_t i = 0; i < npages; i++) {
        uint64_t virt = seg_start + i * PAGE_SIZE;
        uint64_t old_flags = paging_get_flags(virt);
        uint64_t merged = merge_page_flags(old_flags, final_flags);

        if (old_flags & PAGE_FLAG_PRESENT) {
            if (paging_protect(virt, merged) != 0) {
                kprintf(ELF_TAG "failed to update page flags at 0x%llx\n",
                        (unsigned long long)virt);
                return 0;
            }
            continue;
        }

        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf(ELF_TAG "OOM mapping segment at 0x%llx\n",
                    (unsigned long long)virt);
            return 0;
        }
        if (!zero_new_user_page(phys)) {
            kprintf(ELF_TAG "failed to zero new user page\n");
            return 0;
        }
        paging_map(virt, phys, merged);
    }

    if (ph->p_filesz) {
        const uint8_t *src = image + ph->p_offset;
        if (!copy_into_user_mapping(ph->p_vaddr, src, ph->p_filesz)) {
            kprintf(ELF_TAG "failed to copy PT_LOAD bytes\n");
            return 0;
        }
    }

    if (ph->p_memsz > ph->p_filesz) {
        if (!zero_user_mapping(ph->p_vaddr + ph->p_filesz, ph->p_memsz - ph->p_filesz)) {
            kprintf(ELF_TAG "failed to zero PT_LOAD bss\n");
            return 0;
        }
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
            if (!load_segment(&phdrs[i], (const uint8_t *)image, size)) {
                return 0;
            }
            segs_loaded++;
            if (phdrs[i].p_memsz) {
                uint64_t end = align_up_page(phdrs[i].p_vaddr + phdrs[i].p_memsz);
                if (end > highest_end) highest_end = end;
            }
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
