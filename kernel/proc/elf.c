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
#include "kernel/mm/vma.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

#define ELF_TAG "[elf]  "

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (PAGE_SIZE - 1)

static inline void *phys_to_hhdm(uint64_t phys) {
    return (void *)(uintptr_t)(boot_get_hhdm_offset() + phys);
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    uint64_t r = a + b;
    if (r < a) return 1;
    if (out) *out = r;
    return 0;
}

static uint64_t align_up_page(uint64_t v) {    return (v + PAGE_MASK) & ~PAGE_MASK;
}

static uint64_t align_down_page(uint64_t v) {
    return v & ~PAGE_MASK;
}

static int validate_elf(const struct elf64_ehdr *eh, uint64_t size,
                        uint64_t bias) {
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
        /* A5c: the fourth tenant's machine named, like the others --
         * a /bina64 binary reaching this loader is a staging bug, and
         * the refusal should say so instead of printing a bare number. */
        kprintf(ELF_TAG "not x86_64 (e_machine=%u%s)\n", eh->e_machine,
                eh->e_machine == 183 ? " -- aarch64, the /bina64 tenant" :
                eh->e_machine == 243 ? " -- riscv64, the /binrv tenant" : "");
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
    /* LX_COMPAT L4: for an ET_DYN image e_entry is a bias-relative
     * offset, so the "does it land in user space" check applies the bias
     * (a PIE entry of 0x1040 at bias 0x555555554000 is fine; without the
     * bias the same image would be misread as an empty entry). */
    uint64_t entry_abs;
    if (add_overflow_u64((uint64_t)eh->e_entry, bias, &entry_abs)) {
        kprintf(ELF_TAG "entry+bias overflow\n");
        return 0;
    }
    if (entry_abs == 0 || entry_abs >= USER_VADDR_TOP) {
        kprintf(ELF_TAG "bad entry address 0x%llx\n",
                (unsigned long long)entry_abs);
        return 0;
    }
    phdr_bytes = (uint64_t)eh->e_phnum * (uint64_t)sizeof(struct elf64_phdr);
    if (eh->e_phoff > size || phdr_bytes > size - eh->e_phoff) {
        kprintf(ELF_TAG "program headers out of file bounds\n");
        return 0;
    }
    return 1;
}

static int validate_segment(const struct elf64_phdr *ph, uint64_t image_size,
                            uint64_t bias) {
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
    if (bias + ph->p_vaddr < PAGE_SIZE) {
        kprintf(ELF_TAG "refusing low user mapping at 0x%llx\n",
                (unsigned long long)(bias + ph->p_vaddr));
        return 0;
    }
    if (ph->p_memsz == 0) return 1;
    if (add_overflow_u64(bias + ph->p_vaddr, ph->p_memsz - 1, &mem_end) ||
        mem_end >= USER_VADDR_TOP) {
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
                        uint64_t image_size, uint64_t bias) {
    uint64_t seg_start, seg_end, npages;
    uint64_t final_flags;

    if (!validate_segment(ph, image_size, bias)) {
        return 0;
    }
    if (ph->p_memsz == 0) {
        return 1;
    }

    /* LX_COMPAT L4: every virtual address is bias-relative for an ET_DYN
     * image; ET_EXEC passes bias 0 and nothing changes. */
    uint64_t vaddr = bias + ph->p_vaddr;

    seg_start = vaddr & ~PAGE_MASK;
    seg_end   = align_up_page(vaddr + ph->p_memsz);
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
        if (!copy_into_user_mapping(vaddr, src, ph->p_filesz)) {
            kprintf(ELF_TAG "failed to copy PT_LOAD bytes\n");
            return 0;
        }
    }

    if (ph->p_memsz > ph->p_filesz) {
        if (!zero_user_mapping(vaddr + ph->p_filesz, ph->p_memsz - ph->p_filesz)) {
            kprintf(ELF_TAG "failed to zero PT_LOAD bss\n");
            return 0;
        }
    }

    return 1;
}

uint64_t elf_load_at(const void *image, uint64_t size, uint64_t bias,
                     uint64_t *out_brk, uint64_t *out_phdr, uint64_t *out_phnum) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    if (!validate_elf(eh, size, bias)) {
        return 0;
    }

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff);

    int segs_loaded = 0;
    uint64_t highest_end = 0;
    uint64_t phdr_vaddr = 0;          /* AT_PHDR (M5 auxv) */

    for (int i = 0; i < eh->e_phnum; i++) {
        /* PT_PHDR explicitly locates the program-header table in memory. */
        if (phdrs[i].p_type == PT_PHDR) {
            phdr_vaddr = phdrs[i].p_vaddr;
        }
        if (phdrs[i].p_type == PT_LOAD) {
            if (!load_segment(&phdrs[i], (const uint8_t *)image, size, bias)) {
                return 0;
            }
            segs_loaded++;
            if (phdrs[i].p_memsz) {
                uint64_t end = align_up_page(bias + phdrs[i].p_vaddr +
                                             phdrs[i].p_memsz);
                if (end > highest_end) highest_end = end;
                /* LX_COMPAT L1: also DESCRIBE the segment in the owning
                 * thread's VMA list, with the p_flags protections.  The
                 * pages are already present (load_segment copied them),
                 * so nothing ever faults here -- but mprotect() refuses
                 * any range its VMA walk cannot cover, and glibc's
                 * static startup turns the RELRO region read-only with
                 * mprotect() after relocation ("cannot apply additional
                 * memory protection after relocation" was this, the
                 * loader mapping pages without describing them).  Native
                 * binaries never noticed: nothing native calls mprotect
                 * on its own image.  ANON rather than FILE: there is no
                 * ofd to read from, the bytes are already in place. */
                tcb_t *owner = sched_current();
                if (owner) {
                    uint32_t vflags = VMA_ANON;
                    if (phdrs[i].p_flags & PF_R) vflags |= VMA_READ;
                    if (phdrs[i].p_flags & PF_W) vflags |= VMA_WRITE;
                    if (phdrs[i].p_flags & PF_X) vflags |= VMA_EXEC;
                    uint64_t vf = spinlock_acquire_irqsave(&owner->vma_lock);
                    (void)vma_insert(&owner->vma_list,
                                     align_down_page(bias + phdrs[i].p_vaddr),
                                     end, vflags, NULL, 0);
                    spinlock_release_irqrestore(&owner->vma_lock, vf);
                }
            }
            /* Fallback AT_PHDR: the PT_LOAD whose file range covers e_phoff
             * maps the header table. p_vaddr + (e_phoff - p_offset). */
            if (phdr_vaddr == 0 &&
                eh->e_phoff >= phdrs[i].p_offset &&
                eh->e_phoff <  phdrs[i].p_offset + phdrs[i].p_filesz) {
                phdr_vaddr = phdrs[i].p_vaddr +
                             (eh->e_phoff - phdrs[i].p_offset);
            }
        }
    }

    if (segs_loaded == 0) {
        kprintf(ELF_TAG "no PT_LOAD segments found\n");
        return 0;
    }

    if (out_brk)  *out_brk  = highest_end;
    if (out_phdr) *out_phdr = bias + phdr_vaddr;
    if (out_phnum)*out_phnum= (uint64_t)eh->e_phnum;

    kprintf(ELF_TAG "loaded %d segment(s), entry 0x%llx\n",
            segs_loaded, (unsigned long long)(bias + eh->e_entry));
    return bias + eh->e_entry;
}

/* Back-compat wrapper (bias 0): the native static binaries. */
uint64_t elf_load(const void *image, uint64_t size, uint64_t *out_brk,
                  uint64_t *out_phdr, uint64_t *out_phnum) {
    return elf_load_at(image, size, 0, out_brk, out_phdr, out_phnum);
}

/* LX_COMPAT L4: locate the PT_INTERP segment and copy its path.  The
 * segment's p_filesz bytes are a NUL-terminated string IN the image
 * (e.g. "/lib64/ld-linux-x86-64.so.2"); the copy is bounded by both the
 * segment's file range and @cap.  Returns 1 and fills @out when present,
 * 0 otherwise (a static image). */
int elf_interp_path(const void *image, uint64_t size, char *out, uint64_t cap) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    /* Header bounds only — validate_elf's full pass is the loader's job;
     * here a malformed header simply means "no interpreter". */
    if (size < sizeof(struct elf64_ehdr)) return 0;
    if (eh->e_phnum == 0) return 0;
    uint64_t phbytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    if (eh->e_phoff > size || phbytes > size - eh->e_phoff) return 0;

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type != PT_INTERP) continue;
        if (phdrs[i].p_filesz == 0 || phdrs[i].p_filesz >= cap) return 0;
        if (phdrs[i].p_offset > size ||
            phdrs[i].p_filesz > size - phdrs[i].p_offset) return 0;
        const char *s = (const char *)image + phdrs[i].p_offset;
        memcpy(out, s, (size_t)phdrs[i].p_filesz);
        out[phdrs[i].p_filesz] = '\0';
        return 1;
    }
    return 0;
}

/* LX_COMPAT L4: PT_GNU_STACK's PF_X bit decides whether the initial user
 * stack is executable.  Absent segment (or a non-X one) -> 0 -> NX, the
 * default every modern glibc image ships. */
int elf_gnu_stack_x(const void *image, uint64_t size) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (size < sizeof(struct elf64_ehdr)) return 0;
    if (eh->e_phnum == 0) return 0;
    uint64_t phbytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    if (eh->e_phoff > size || phbytes > size - eh->e_phoff) return 0;

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type == PT_GNU_STACK)
            return (phdrs[i].p_flags & PF_X) ? 1 : 0;
    }
    return 0;
}
