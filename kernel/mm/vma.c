#include "kernel/mm/vma.h"
#include "kernel/mm/shmem.h"
#include "kernel/mm/slab.h"
#include "kernel/mm/page_cache.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/boot_info.h"

static slab_cache_t *vma_cache = NULL;

static void vma_retain_file(uint32_t flags, struct ofd *file) {
    if ((flags & VMA_FILE) && file) vfs_ofd_get(file);
}

static void vma_drop_file(uint32_t flags, struct ofd *file) {
    if ((flags & VMA_FILE) && file) vfs_ofd_put(file);
}

/* Acquire the backing-object reference represented by one VMA node. */
static int vma_retain_backing(uint32_t flags, struct ofd *file, int shmid) {
    if (flags & VMA_SHMEM) {
        if ((flags & VMA_FILE) || file || shmid <= 0)
            return -1;
        return shmem_get(shmid);
    }
    if ((flags & VMA_FILE) && !file)
        return -1;
    vma_retain_file(flags, file);
    return 0;
}

/* Drop exactly one backing-object reference owned by a VMA node. */
static void vma_drop_backing(uint32_t flags, struct ofd *file, int shmid) {
    if (flags & VMA_SHMEM)
        shmem_put(shmid);
    else
        vma_drop_file(flags, file);
}

static vma_t *vma_find_nolock(vma_t *list, uint64_t va) {
    vma_t *curr = list;
    while (curr) {
        if (va >= curr->va_start && va < curr->va_end)
            return curr;
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
        uint64_t hhdm = boot_get_hhdm_offset();
        memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
    }
}

void vma_init(void) {
    if (!vma_cache)
        vma_cache = slab_create("vma", sizeof(vma_t), 0);
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
    if (!list_head || start >= end || (flags & VMA_SHMEM)) return -1;
    if ((flags & VMA_FILE) && !file) return -1;

    vma_t *new_vma = vma_alloc();
    if (!new_vma) return -1;

    new_vma->va_start = start;
    new_vma->va_end = end;
    new_vma->flags = flags;
    new_vma->file = file;
    new_vma->file_off = file_off;
    new_vma->shmid = 0;
    new_vma->next = NULL;
    vma_retain_file(flags, file);

    vma_t **curr = list_head;
    while (*curr && (*curr)->va_start < start)
        curr = &((*curr)->next);

    new_vma->next = *curr;
    *curr = new_vma;
    return 0;
}

/* Insert a VMA backed by an anonymous shared-memory object. The VMA owns one
 * object reference from the moment it is linked into the list. */
int vma_insert_shmem(vma_t **list_head, uint64_t start, uint64_t end,
                     uint32_t flags, int shmid) {
    if (!list_head || start >= end || (flags & VMA_FILE) || shmid <= 0)
        return -1;
    if (shmem_get(shmid) != 0)
        return -1;

    uint64_t object_size = shmem_size(shmid);
    if (object_size == 0 || end - start > object_size) {
        shmem_put(shmid);
        return -1;
    }

    vma_t *new_vma = vma_alloc();
    if (!new_vma) {
        shmem_put(shmid);
        return -1;
    }

    new_vma->va_start = start;
    new_vma->va_end = end;
    /* A shmem VMA is shared by definition; do not let fork turn it into COW. */
    new_vma->flags = flags | VMA_SHMEM | VMA_SHARED;
    new_vma->file = NULL;
    new_vma->file_off = 0;
    new_vma->shmid = shmid;
    new_vma->next = NULL;

    vma_t **curr = list_head;
    while (*curr && (*curr)->va_start < start)
        curr = &((*curr)->next);
    new_vma->next = *curr;
    *curr = new_vma;
    return 0;
}

/* Clone one complete VMA record, preserving shmid and non-zero backing
 * offsets. The destination list is serialized by its owner/caller. */
int vma_clone_entry(vma_t **list_head, const vma_t *source) {
    if (!list_head || !source || source->va_start >= source->va_end)
        return -1;

    vma_t *copy = vma_alloc();
    if (!copy) return -1;

    *copy = *source;
    copy->next = NULL;
    if (vma_retain_backing(copy->flags, copy->file, copy->shmid) != 0) {
        vma_free(copy);
        return -1;
    }

    vma_t **curr = list_head;
    while (*curr && (*curr)->va_start < copy->va_start)
        curr = &((*curr)->next);
    copy->next = *curr;
    *curr = copy;
    return 0;
}

void vma_remove_range(vma_t **list_head, uint64_t start, uint64_t end) {
    if (!list_head || start >= end) return;

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
        if ((v->va_start < start && !left) ||
            (v->va_end > end && !right)) {
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
        int sid = v->shmid;

        /* Retain each replacement fragment before dropping the original VMA.
         * If retaining unexpectedly fails, leave the original list intact. */
        if (left) {
            left->va_start = v_start;
            left->va_end = start;
            left->flags = flags;
            left->file = file;
            left->file_off = off;
            left->shmid = sid;
            left->next = NULL;
            if (vma_retain_backing(flags, file, sid) != 0) {
                vma_free(left);
                if (right) vma_free(right);
                curr = &v->next;
                continue;
            }
        }
        if (right) {
            right->va_start = end;
            right->va_end = v_end;
            right->flags = flags;
            right->file = file;
            right->file_off = off + (end - v_start);
            right->shmid = sid;
            right->next = NULL;
            if (vma_retain_backing(flags, file, sid) != 0) {
                if (left) {
                    vma_drop_backing(flags, file, sid);
                    vma_free(left);
                }
                vma_free(right);
                curr = &v->next;
                continue;
            }
        }

        *curr = v->next;
        if (left) {
            left->next = *curr;
            *curr = left;
            curr = &left->next;
        }
        if (right) {
            right->next = *curr;
            *curr = right;
            curr = &right->next;
        }

        vma_drop_backing(flags, file, sid);
        vma_free(v);
    }
}

void vma_free_all(vma_t **list_head) {
    if (!list_head) return;
    vma_t *curr = *list_head;
    while (curr) {
        vma_t *next = curr->next;
        vma_drop_backing(curr->flags, curr->file, curr->shmid);
        vma_free(curr);
        curr = next;
    }
    *list_head = NULL;
}

__attribute__((unused)) static void vma_release_fault_snapshot(const vma_t *snapshot,
                                       int file_retained,
                                       int shmem_retained) {
    if (file_retained) vfs_ofd_put(snapshot->file);
    if (shmem_retained) shmem_put(snapshot->shmid);
}

/*
 * handle_user_page_fault() — resolve a #PF for a lazy VMA.
 * Returns 0 on success, -1 on failure (SIGSEGV).
 */
int handle_user_page_fault(uint64_t cr2, uint64_t err_code) {
    tcb_t *cur = sched_current();
    if (!cur) return -1;

    uint64_t page_va = cr2 & ~0xFFFULL;
    vma_t snapshot;
    int file_ref = 0;
    int shmem_ref = 0;

    /* Snapshot the descriptor and pin every backing object before dropping
     * vma_lock. munmap/exec can then remove the VMA without invalidating the
     * in-flight I/O or SHMEM lookup. */
    uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
    vma_t *vma = vma_find_nolock(cur->vma_list, page_va);
    if (!vma) {
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        return -1;
    }

    snapshot = *vma;
    if ((snapshot.flags & VMA_FILE) && snapshot.file) {
        vfs_ofd_get(snapshot.file);
        file_ref = 1;
    }
    if (snapshot.flags & VMA_SHMEM) {
        if (shmem_get(snapshot.shmid) != 0) {
            spinlock_release_irqrestore(&cur->vma_lock, vf);
            if (file_ref) vfs_ofd_put(snapshot.file);
            return -1;
        }
        shmem_ref = 1;
    }
    spinlock_release_irqrestore(&cur->vma_lock, vf);

    /* Present write/execute bits in the x86 page-fault error code describe
     * the access which must be authorised by the VMA. A plain data read is
     * rejected for PROT_NONE and write-only mappings. */
    if ((err_code & 0x02) && !(snapshot.flags & VMA_WRITE)) goto fault_fail;
    if ((err_code & 0x10) && !(snapshot.flags & VMA_EXEC))  goto fault_fail;
    if (!(err_code & (0x02 | 0x10)) && !(snapshot.flags & VMA_READ))
        goto fault_fail;

    uint64_t offset = snapshot.file_off + (page_va - snapshot.va_start);
    uint64_t phys = 0;

    if (snapshot.flags & VMA_SHMEM) {
        if (shmem_get_or_alloc(snapshot.shmid, offset, &phys) != 0)
            goto fault_fail;
    } else if ((snapshot.flags & VMA_SHARED) &&
               (snapshot.flags & VMA_FILE)) {
        if (!snapshot.file) goto fault_fail;

        struct {
            struct ofd *file;
            uint64_t offset;
            uint32_t flags;
        } fill_ctx = { snapshot.file, offset, snapshot.flags };

        if (page_cache_get_or_alloc(snapshot.file, offset, &phys,
                                     fill_page, &fill_ctx) != 0)
            goto fault_fail;

        /* Track the first write through a shared file mapping. */
        if (err_code & 0x02)
            page_cache_mark_dirty(snapshot.file, offset);
    } else {
        phys = pmm_alloc_frame();
        if (!phys) goto fault_fail;
        if (snapshot.flags & VMA_FILE) {
            if (!snapshot.file) {
                pmm_free_frame(phys);
                goto fault_fail;
            }
            vfs_read_at_phys(snapshot.file, offset, phys, 4096);
        } else {
            uint64_t hhdm = boot_get_hhdm_offset();
            memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
        }
    }

    uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (snapshot.flags & VMA_WRITE) pte_flags |= PAGE_FLAG_WRITABLE;
    if (!(snapshot.flags & VMA_EXEC)) pte_flags |= PAGE_FLAG_NO_EXEC;

    /* A shared writable file page is initially mapped read-only. Its first
     * store faults again and marks the page-cache entry dirty. */
    if ((snapshot.flags & VMA_SHARED) && (snapshot.flags & VMA_FILE) &&
        !(err_code & 0x02))
        pte_flags &= ~PAGE_FLAG_WRITABLE;

    /* Revalidate under vma_lock before publishing the PTE. If munmap or
     * MAP_FIXED removed/replaced this VMA while backing I/O ran, do not
     * resurrect the old mapping. Comparing logical backing offset also
     * permits a concurrent split which leaves this page in a surviving
     * fragment of the same mapping. */
    vf = spinlock_acquire_irqsave(&cur->vma_lock);
    vma_t *live = vma_find_nolock(cur->vma_list, page_va);
    int same = 0;
    if (live && live->flags == snapshot.flags &&
        live->file == snapshot.file && live->shmid == snapshot.shmid &&
        page_va >= live->va_start && page_va < live->va_end) {
        uint64_t live_off = live->file_off + (page_va - live->va_start);
        same = (live_off == offset);
    }

    if (!same) {
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        pmm_free_frame(phys);
        goto fault_fail;
    }

    int map_rc = paging_map_if_absent(page_va, phys, pte_flags);
    if (map_rc == 0) {
        /* A concurrent fault already installed a page. If this was the
         * shared-file first-write fault and both lookups resolved to the same
         * page-cache frame, promote that PTE to writable now. */
        uint64_t existing = paging_get_phys(page_va);
        if (existing == phys && (err_code & 0x02) &&
            (snapshot.flags & VMA_SHARED) &&
            (snapshot.flags & VMA_FILE) &&
            (snapshot.flags & VMA_WRITE)) {
            uint64_t existing_flags = paging_get_flags(page_va);
            if (!(existing_flags & PAGE_FLAG_WRITABLE))
                (void)paging_protect(page_va,
                                     existing_flags | PAGE_FLAG_WRITABLE);
        }
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        pmm_free_frame(phys); /* this fault's extra backing-frame reference */
        if (file_ref) vfs_ofd_put(snapshot.file);
        if (shmem_ref) shmem_put(snapshot.shmid);
        return 0;
    }

    spinlock_release_irqrestore(&cur->vma_lock, vf);
    if (map_rc < 0) pmm_free_frame(phys);
    /* map_rc == 1 transfers this PMM reference to the installed PTE. */
    if (file_ref) vfs_ofd_put(snapshot.file);
    if (shmem_ref) shmem_put(snapshot.shmid);
    return map_rc == 1 ? 0 : -1;

fault_fail:
    if (file_ref) vfs_ofd_put(snapshot.file);
    if (shmem_ref) shmem_put(snapshot.shmid);
    return -1;
}