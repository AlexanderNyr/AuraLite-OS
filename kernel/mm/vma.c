#include "kernel/mm/vma.h"
#include "kernel/mm/slab.h"
#include "kernel/mm/page_cache.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

static slab_cache_t *vma_cache = NULL;

void vma_init(void) {
    if (!vma_cache) {
        vma_cache = slab_create("vma", sizeof(vma_t), 0);
    }
}

vma_t *vma_alloc(void) {
    if (!vma_cache) vma_init();
    return (vma_t *)slab_alloc(vma_cache);
}

void vma_free(vma_t *v) {
    if (!vma_cache) vma_init();
    slab_free(vma_cache, v);
}

vma_t *vma_find(vma_t *list, uint64_t va) {
    vma_t *curr = list;
    while (curr) {
        if (va >= curr->va_start && va < curr->va_end) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

int vma_insert(vma_t **list_head, uint64_t start, uint64_t end,
               uint32_t flags, struct ofd *file, uint64_t file_off) {
    vma_t *new_vma = vma_alloc();
    if (!new_vma) return -1;

    new_vma->va_start = start;
    new_vma->va_end = end;
    new_vma->flags = flags;
    new_vma->file = file;
    new_vma->file_off = file_off;
    new_vma->next = NULL;

    vma_t **curr = list_head;
    while (*curr && (*curr)->va_start < start) {
        curr = &((*curr)->next);
    }

    new_vma->next = *curr;
    *curr = new_vma;

    return 0;
}

void vma_remove_range(vma_t **list_head, uint64_t start, uint64_t end) {
    vma_t **curr = list_head;
    while (*curr) {
        vma_t *v = *curr;
        if (v->va_start >= end) break;
        if (v->va_end <= start) {
            curr = &v->next;
            continue;
        }

        uint64_t v_start = v->va_start;
        uint64_t v_end = v->va_end;
        uint32_t flags = v->flags;
        struct ofd *file = v->file;
        uint64_t off = v->file_off;

        *curr = v->next;
        vma_free(v);

        if (v_start < start) {
            vma_t *left = vma_alloc();
            if (left) {
                left->va_start = v_start;
                left->va_end = start;
                left->flags = flags;
                left->file = file;
                left->file_off = off;
                left->next = *curr;
                *curr = left;
                curr = &left->next;
            }
        }

        if (v_end > end) {
            vma_t *right = vma_alloc();
            if (right) {
                right->va_start = end;
                right->va_end = v_end;
                right->flags = flags;
                right->file = file;
                right->file_off = off + (end - v_start);
                right->next = *curr;
                *curr = right;
                curr = &right->next;
            }
        }
    }
}

void vma_free_all(vma_t **list_head) {
    vma_t *curr = *list_head;
    while (curr) {
        vma_t *next = curr->next;
        vma_free(curr);
        curr = next;
    }
    *list_head = NULL;
}

/*
 * handle_user_page_fault() — resolve a #PF for a lazy VMA.
 * Returns 0 on success, -1 on failure (SIGSEGV).
 */
int handle_user_page_fault(uint64_t cr2, uint64_t err_code) {
    tcb_t *cur = sched_current();
    if (!cur) return -1;

    uint64_t page_va = cr2 & ~0xFFFULL;
    vma_t *vma = vma_find(cur->vma_list, page_va);
    if (!vma) return -1;

    /* Permission check */
    if ((err_code & 2) && !(vma->flags & VMA_WRITE)) return -1;
    if ((err_code & 0x10) && !(vma->flags & VMA_EXEC)) return -1;

    uint64_t phys;
    uint64_t offset = vma->file_off + (page_va - vma->va_start);

    if (vma->flags & VMA_SHARED) {
        phys = page_cache_get(vma->file, offset);
        if (!phys) {
            phys = pmm_alloc_frame();
            if (!phys) return -1;
            if (vma->flags & VMA_FILE) {
                vfs_read_at_phys(vma->file, offset, phys, 4096);
            } else {
                uint64_t hhdm = limine_get_hhdm_offset();
                memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
            }
            page_cache_put(vma->file, offset, phys);
        }
        pmm_inc_frame_ref(phys);
    } else {
        phys = pmm_alloc_frame();
        if (!phys) return -1;
        if (vma->flags & VMA_FILE) {
            vfs_read_at_phys(vma->file, offset, phys, 4096);
        } else {
            uint64_t hhdm = limine_get_hhdm_offset();
            memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
        }
    }

    uint64_t pte = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (vma->flags & VMA_WRITE) pte |= PAGE_FLAG_WRITABLE;
    if (!(vma->flags & VMA_EXEC)) pte |= PAGE_FLAG_NO_EXEC;

    paging_map(page_va, phys, pte);
    return 0;
}
