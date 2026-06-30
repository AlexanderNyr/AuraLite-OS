#include "kernel/mm/vma.h"
#include "kernel/mm/slab.h"
#include "kernel/mm/page_cache.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/limine_requests.h"

static slab_cache_t *vma_cache = NULL;

static vma_t *vma_find_nolock(vma_t *list, uint64_t va) {
    vma_t *curr = list;
    while (curr) {
        if (va >= curr->va_start && va < curr->va_end) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static void fill_page(uint64_t phys, void *arg) {
    struct {
        struct ofd *file;
        uint64_t offset;
        uint32_t flags;
    } *ctx = arg;

    if (!ctx) return;
    if (ctx->flags & VMA_FILE) {
        vfs_read_at_phys(ctx->file, ctx->offset, phys, 4096);
    } else {
        uint64_t hhdm = limine_get_hhdm_offset();
        memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
    }
}

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
    return vma_find_nolock(list, va);
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

        vma_t *left = (v->va_start < start) ? vma_alloc() : NULL;
        vma_t *right = (v->va_end > end) ? vma_alloc() : NULL;
        if ((v->va_start < start && !left) || (v->va_end > end && !right)) {
            if (left) vma_free(left);
            if (right) vma_free(right);
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
    uint64_t irqf = spinlock_acquire_irqsave(&cur->vma_lock);
    vma_t *vma = vma_find_nolock(cur->vma_list, page_va);
    if (!vma) {
        spinlock_release_irqrestore(&cur->vma_lock, irqf);
        return -1;
    }

    vma_t snapshot = *vma;
    spinlock_release_irqrestore(&cur->vma_lock, irqf);

    /* Permission check */
    if ((err_code & 0x02) && !(snapshot.flags & VMA_WRITE)) return -1;
    if ((err_code & 0x10) && !(snapshot.flags & VMA_EXEC)) return -1;

    uint64_t phys;
    uint64_t offset = snapshot.file_off + (page_va - snapshot.va_start);

    if (snapshot.flags & VMA_SHARED) {
        struct {
            struct ofd *file;
            uint64_t offset;
            uint32_t flags;
        } fill_ctx = { snapshot.file, offset, snapshot.flags };

        if (page_cache_get_or_alloc(snapshot.file, offset, &phys, fill_page, &fill_ctx) != 0) {
            return -1;
        }
    } else {
        phys = pmm_alloc_frame();
        if (!phys) return -1;
        if (snapshot.flags & VMA_FILE) {
            vfs_read_at_phys(snapshot.file, offset, phys, 4096);
        } else {
            uint64_t hhdm = limine_get_hhdm_offset();
            memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
        }
    }

    uint64_t pte = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (snapshot.flags & VMA_WRITE) pte |= PAGE_FLAG_WRITABLE;
    if (!(snapshot.flags & VMA_EXEC)) pte |= PAGE_FLAG_NO_EXEC;

    paging_map(page_va, phys, pte);
    return 0;
}
