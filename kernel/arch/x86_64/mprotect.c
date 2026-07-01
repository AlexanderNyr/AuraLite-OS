#include "kernel/arch/x86_64/mprotect.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/paging.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static uint32_t mprotect_build_vma_flags(uint32_t old_flags, uint64_t prot) {
    uint32_t vflags = old_flags & ~(VMA_READ | VMA_WRITE | VMA_EXEC);
    if (prot & PROT_READ)  vflags |= VMA_READ;
    if (prot & PROT_WRITE) vflags |= VMA_WRITE;
    if (prot & PROT_EXEC)  vflags |= VMA_EXEC;
    return vflags;
}

static uint64_t mprotect_build_pte_flags(uint64_t prot) {
    uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & PROT_WRITE) pte_flags |= PAGE_FLAG_WRITABLE;
    if (!(prot & PROT_EXEC)) pte_flags |= PAGE_FLAG_NO_EXEC;
    return pte_flags;
}

int mprotect_update_vma_range(vma_t *list, uint64_t addr, uint64_t len, uint64_t prot) {
    uint64_t end = addr + len;
    uint64_t covered = addr;
    vma_t *v = vma_find(list, addr);

    while (v && covered < end) {
        if (v->va_start > covered) {
            return -1;
        }
        covered = v->va_end;
        v = v->next;
    }

    if (covered < end) {
        return -1;
    }

    v = vma_find(list, addr);
    while (v && v->va_start < end) {
        v->flags = mprotect_build_vma_flags(v->flags, prot);
        v = v->next;
    }

    return 0;
}

#ifndef MPROTECT_INVLPG
#define MPROTECT_INVLPG(va) invlpg(va)
#endif

void mprotect_remap_present_pages(uint64_t addr, uint64_t len, uint64_t prot) {
    uint64_t end = addr + len;
    uint64_t pte_flags = mprotect_build_pte_flags(prot);
    int changed = 0;

    for (uint64_t va = addr; va < end; va += PAGE_SIZE_BYTES) {
        uint64_t phys = paging_get_phys(va);
        if (!phys) continue;

        paging_map(va, phys, pte_flags);
        MPROTECT_INVLPG(va);
        changed = 1;
    }

    if (changed) {
        lapic_send_ipi_all_excluding_self(IPI_TLB_SHOOTDOWN_VECTOR);
    }
}
