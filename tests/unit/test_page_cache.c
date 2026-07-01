#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "../../kernel/fs/vfs.h"
#include "../../kernel/lib/spinlock.h"

static _Thread_local int spinlock_depth;

void spinlock_acquire(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        sched_yield();
    }
    spinlock_depth++;
}

void spinlock_release(spinlock_t *l) {
    spinlock_depth--;
    __sync_lock_release(&l->locked);
}

static uint64_t next_phys = 0x1000;
static int pmm_alloc_calls;
static int pmm_free_calls;
static int kmalloc_fail_after = -1;
static int kmalloc_calls;
static int fill_calls;
static int write_calls;
static int64_t write_ret = 4096;
static uint8_t backing[4096 * 8];
static atomic_int fill_started;
static atomic_int allow_fill;
static atomic_int second_done;

uint64_t pmm_alloc_frame(void) {
    assert(spinlock_depth == 0);
    pmm_alloc_calls++;
    uint64_t p = next_phys;
    next_phys += 0x1000;
    return p;
}

void pmm_free_frame(uint64_t phys) {
    (void)phys;
    assert(spinlock_depth == 0);
    pmm_free_calls++;
}

int pmm_inc_frame_ref(uint64_t phys) { (void)phys; return 0; }

void *kmalloc(size_t n) {
    assert(spinlock_depth == 0);
    kmalloc_calls++;
    if (kmalloc_fail_after >= 0 && kmalloc_calls > kmalloc_fail_after) return NULL;
    return calloc(1, n);
}

void kfree(void *p) {
    assert(spinlock_depth == 0);
    free(p);
}

uint64_t limine_get_hhdm_offset(void) { return (uint64_t)(uintptr_t)backing - 0x1000; }
void kprintf(const char *fmt, ...) { (void)fmt; }

/* In unit tests <sched.h> already declares POSIX int sched_yield(void).
 * Avoid the kernel void declaration colliding with it. */
#define __SCHED_YIELD_DECLARED

/* Give the concurrent fill test plenty of spins before the new timeout
 * path fires; real kernel code uses the default 100000u. */
#define PAGE_CACHE_READY_SPINS 10000000u

#include "../../kernel/mm/page_cache.c"

static void fill_fn(uint64_t phys, void *arg) {
    (void)arg;
    fill_calls++;
    memset((void *)(uintptr_t)(limine_get_hhdm_offset() + phys), 0xAB, 4096);
}

static void blocking_fill_fn(uint64_t phys, void *arg) {
    (void)arg;
    fill_calls++;
    atomic_store(&fill_started, 1);
    while (!atomic_load(&allow_fill)) {
        sched_yield();
    }
    memset((void *)(uintptr_t)(limine_get_hhdm_offset() + phys), 0xCD, 4096);
}

static int64_t fake_write(struct vnode *vn, uint64_t off, const void *buf, uint64_t len) {
    (void)vn; (void)off; (void)buf; (void)len;
    write_calls++;
    return write_ret;
}

static void reset_state(void) {
    memset(cache_buckets, 0, sizeof(cache_buckets));
    memset(backing, 0, sizeof(backing));
    next_phys = 0x1000;
    pmm_alloc_calls = 0;
    pmm_free_calls = 0;
    kmalloc_fail_after = -1;
    kmalloc_calls = 0;
    fill_calls = 0;
    write_calls = 0;
    write_ret = 4096;
    atomic_store(&fill_started, 0);
    atomic_store(&allow_fill, 0);
    atomic_store(&second_done, 0);
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

static void test_get_or_alloc_never_allocates_under_spinlock(void) {
    reset_state();
    struct ofd file;
    memset(&file, 0, sizeof(file));
    uint64_t phys = 0;
    assert(page_cache_get_or_alloc(&file, 0, &phys, fill_fn, NULL) == 0);
    assert(phys == 0x1000);
    assert(kmalloc_calls == 1);
    assert(pmm_alloc_calls == 1);
}

struct alloc_thread_ctx {
    struct ofd *file;
    uint64_t offset;
    uint64_t phys;
    int rc;
};

static void *alloc_thread(void *arg) {
    struct alloc_thread_ctx *ctx = arg;
    ctx->rc = page_cache_get_or_alloc(ctx->file, ctx->offset, &ctx->phys,
                                      blocking_fill_fn, NULL);
    atomic_store(&second_done, 1);
    return NULL;
}

static void *fill_thread(void *arg) {
    struct alloc_thread_ctx *ctx = arg;
    ctx->rc = page_cache_get_or_alloc(ctx->file, ctx->offset, &ctx->phys,
                                      blocking_fill_fn, NULL);
    return NULL;
}

static void test_get_or_alloc_waits_for_ready_page(void) {
    reset_state();
    struct ofd file;
    memset(&file, 0, sizeof(file));

    pthread_t first;
    pthread_t second;
    struct alloc_thread_ctx first_ctx = { .file = &file, .offset = 0, .phys = 0, .rc = -1 };
    struct alloc_thread_ctx second_ctx = { .file = &file, .offset = 0, .phys = 0, .rc = -1 };

    assert(pthread_create(&first, NULL, fill_thread, &first_ctx) == 0);
    while (!atomic_load(&fill_started)) {
        sched_yield();
    }

    assert(pthread_create(&second, NULL, alloc_thread, &second_ctx) == 0);
    for (int i = 0; i < 10000 && !atomic_load(&second_done); i++) {
        sched_yield();
    }
    assert(atomic_load(&second_done) == 0);

    atomic_store(&allow_fill, 1);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);

    assert(first_ctx.rc == 0);
    assert(second_ctx.rc == 0);
    assert(first_ctx.phys == second_ctx.phys);
    assert(fill_calls == 1);
    assert(backing[0] == 0xCD);
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
    test_get_or_alloc_never_allocates_under_spinlock();
    test_get_or_alloc_waits_for_ready_page();
    test_flush_keeps_dirty_on_write_failure();
    return 0;
}
