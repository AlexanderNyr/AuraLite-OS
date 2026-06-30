#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/fs/vfs.h"
#include "../../kernel/lib/spinlock.h"

void spinlock_acquire(spinlock_t *l) { l->locked++; }
void spinlock_release(spinlock_t *l) { l->locked--; }

static uint64_t next_phys = 0x1000;
static int pmm_alloc_calls;
static int pmm_free_calls;
static int kmalloc_fail_after = -1;
static int kmalloc_calls;
static int fill_calls;
static int write_calls;
static int64_t write_ret = 4096;
static uint8_t backing[4096 * 8];

uint64_t pmm_alloc_frame(void) { pmm_alloc_calls++; uint64_t p = next_phys; next_phys += 0x1000; return p; }
void pmm_free_frame(uint64_t phys) { (void)phys; pmm_free_calls++; }
int pmm_inc_frame_ref(uint64_t phys) { (void)phys; return 0; }
void *kmalloc(size_t n) { kmalloc_calls++; if (kmalloc_fail_after >= 0 && kmalloc_calls > kmalloc_fail_after) return NULL; return calloc(1, n); }
void kfree(void *p) { free(p); }
uint64_t limine_get_hhdm_offset(void) { return (uint64_t)(uintptr_t)backing - 0x1000; }
void kprintf(const char *fmt, ...) { (void)fmt; }

#include "../../kernel/mm/page_cache.c"

static void fill_fn(uint64_t phys, void *arg) {
    (void)arg;
    fill_calls++;
    memset((void *)(uintptr_t)(limine_get_hhdm_offset() + phys), 0xAB, 4096);
}

static int64_t fake_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len) {
    (void)vn; (void)off; (void)buf; (void)len;
    write_calls++;
    return write_ret;
}

static void reset_state(void) {
    memset(cache_buckets, 0, sizeof(cache_buckets));
    next_phys = 0x1000;
    pmm_alloc_calls = 0;
    pmm_free_calls = 0;
    kmalloc_fail_after = -1;
    kmalloc_calls = 0;
    fill_calls = 0;
    write_calls = 0;
    write_ret = 4096;
}

static void test_get_or_alloc_reuses_existing(void) {
    reset_state();
    struct vfs_ops ops;
    struct vnode vn;
    struct ofd file;
    memset(&ops, 0, sizeof(ops));
    memset(&vn, 0, sizeof(vn));
    memset(&file, 0, sizeof(file));
    ops.write = fake_write;
    vn.ops = &ops;
    file.vn = &vn;
    uint64_t phys1 = 0, phys2 = 0;
    assert(page_cache_get_or_alloc(&file, 0, &phys1, fill_fn, NULL) == 0);
    assert(page_cache_get_or_alloc(&file, 0, &phys2, fill_fn, NULL) == 0);
    assert(phys1 == phys2);
    assert(pmm_alloc_calls == 1);
    assert(fill_calls == 1);
}

static void test_flush_keeps_dirty_on_write_failure(void) {
    reset_state();
    struct vfs_ops ops;
    struct vnode vn;
    struct ofd file;
    memset(&ops, 0, sizeof(ops));
    memset(&vn, 0, sizeof(vn));
    memset(&file, 0, sizeof(file));
    ops.write = fake_write;
    vn.ops = &ops;
    file.vn = &vn;
    page_cache_put(&file, 0, 0x1000);
    cache_buckets[hash_page(&file, 0)]->dirty = 1;
    write_ret = -1;
    page_cache_flush(&file);
    assert(write_calls == 1);
    assert(cache_buckets[hash_page(&file, 0)]->dirty == 1);
    write_ret = 4096;
    page_cache_flush(&file);
    assert(cache_buckets[hash_page(&file, 0)]->dirty == 0);
}

int main(void) {
    test_get_or_alloc_reuses_existing();
    test_flush_keeps_dirty_on_write_failure();
    return 0;
}
