#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/mm/slab.h"
#include "../../kernel/mm/vma.h"
#include "../../kernel/lib/spinlock.h"

static int fail_after = -1;
static int alloc_count = 0;

slab_cache_t *slab_create(const char *name, uint64_t object_size, uint64_t align) {
    (void)name; (void)object_size; (void)align;
    return (slab_cache_t *)1;
}

void *slab_alloc(slab_cache_t *cache) {
    (void)cache;
    if (fail_after >= 0 && alloc_count >= fail_after) return NULL;
    alloc_count++;
    return calloc(1, sizeof(vma_t));
}

void slab_free(slab_cache_t *cache, void *ptr) {
    (void)cache;
    free(ptr);
}

uint64_t page_cache_get(struct ofd *file, uint64_t offset) { (void)file; (void)offset; return 0; }
void page_cache_put(struct ofd *file, uint64_t offset, uint64_t phys) { (void)file; (void)offset; (void)phys; }
int page_cache_get_or_alloc(struct ofd *file, uint64_t offset, uint64_t *phys_out,
                            void (*fill_fn)(uint64_t phys, void *arg), void *fill_arg) {
    (void)file; (void)offset; (void)phys_out; (void)fill_fn; (void)fill_arg; return -1;
}
uint64_t pmm_alloc_frame(void) { return 0; }
int pmm_inc_frame_ref(uint64_t phys) { (void)phys; return 0; }
int64_t vfs_read_at_phys(struct ofd *ofd, uint64_t off, uint64_t phys, uint64_t len) {
    (void)ofd; (void)off; (void)phys; (void)len; return 0;
}
uint64_t limine_get_hhdm_offset(void) { return 0; }
void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) { (void)virt; (void)phys; (void)flags; }
void kprintf(const char *fmt, ...) { (void)fmt; }
void spinlock_init(spinlock_t *lock) { (void)lock; }
uint64_t spinlock_acquire_irqsave(spinlock_t *lock) { (void)lock; return 0; }
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) { (void)lock; (void)flags; }
struct tcb *sched_current(void) { return NULL; }

#include "../../kernel/mm/vma.c"

static void free_list(vma_t *v) {
    while (v) {
        vma_t *n = v->next;
        free(v);
        v = n;
    }
}

static void reset_alloc(void) {
    fail_after = -1;
    alloc_count = 0;
    vma_init();
}

static void test_split_middle_range(void) {
    reset_alloc();
    vma_t *head = NULL;
    assert(vma_insert(&head, 0x1000, 0x5000, VMA_FILE | VMA_READ, NULL, 0) == 0);
    vma_remove_range(&head, 0x2000, 0x4000);
    assert(head);
    assert(head->va_start == 0x1000 && head->va_end == 0x2000);
    assert(head->file_off == 0);
    assert(head->next);
    assert(head->next->va_start == 0x4000 && head->next->va_end == 0x5000);
    assert(head->next->file_off == 0x3000);
    assert(head->next->next == NULL);
    free_list(head);
}

static void test_oom_keeps_original_vma(void) {
    reset_alloc();
    vma_t *head = NULL;
    assert(vma_insert(&head, 0x1000, 0x5000, VMA_FILE | VMA_READ, NULL, 0) == 0);
    fail_after = alloc_count;
    vma_remove_range(&head, 0x2000, 0x4000);
    assert(head);
    assert(head->va_start == 0x1000 && head->va_end == 0x5000);
    assert(head->next == NULL);
    free_list(head);
}

int main(void) {
    test_split_middle_range();
    test_oom_keeps_original_vma();
    return 0;
}
