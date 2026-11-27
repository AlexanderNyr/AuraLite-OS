/* Exact shipped SYS_WRITE helper, mocked uaccess/VFS/heap; no copied algorithm. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/proc/write_bounce.h"

static uint8_t user_bytes[65536], sink[65536];
static struct vnode node;
static int range_ok, devfs, invalid_fd, oom;
static int copy_calls, write_calls, allocations, frees, live_allocations, validations;
static int copy_fault_at, write_fault_at, short_at, zero_at, impossible_at;
static uint64_t max_chunk, accepted, allocated_size;

int validate_user_range(const void *p, uint64_t n, int write_required) {
    (void)p; (void)n; assert(!write_required); validations++; return range_ok;
}
int copy_from_user(void *dst, const void *src, uint64_t n) {
    copy_calls++;
    if (copy_calls == copy_fault_at) {
        memset(dst, 0xAA, (size_t)(n / 2)); return -1;
    }
    assert((const uint8_t *)src >= user_bytes);
    assert((const uint8_t *)src + n <= user_bytes + sizeof(user_bytes));
    memcpy(dst, src, (size_t)n); return 0;
}
struct vnode *vfs_get_vnode(int fd) { (void)fd; return invalid_fd ? NULL : &node; }
int vfs_fd_is_devfs(int fd) { (void)fd; return devfs; }
void *kmalloc(uint64_t n) {
    allocations++; allocated_size = n;
    if (oom) return NULL;
    void *p = malloc((size_t)n); assert(p); live_allocations++; return p;
}
void kfree(void *p) { assert(p && live_allocations == 1); live_allocations--; frees++; free(p); }
int64_t vfs_write(int fd, const void *p, uint64_t n) {
    (void)fd;
    write_calls++;
    assert((uintptr_t)p < (uintptr_t)user_bytes ||
           (uintptr_t)p >= (uintptr_t)user_bytes + sizeof(user_bytes));
    if (n > max_chunk) max_chunk = n;
    if (invalid_fd) return -EBADF;
    if (write_calls == write_fault_at) return -EIO;
    if (write_calls == impossible_at) return (int64_t)n + 1;
    if (write_calls == zero_at) return 0;
    uint64_t take = write_calls == short_at ? n / 2 : n;
    assert(take <= sizeof(sink) - accepted);
    memcpy(sink + accepted, p, (size_t)take); accepted += take;
    return (int64_t)take;
}
static void reset(void) {
    assert(!live_allocations);
    memset(&node, 0, sizeof(node)); node.type = VFS_TYPE_FILE;
    range_ok = 1; devfs = invalid_fd = oom = 0;
    copy_calls = write_calls = allocations = frees = validations = 0;
    copy_fault_at = write_fault_at = short_at = zero_at = impossible_at = 0;
    max_chunk = accepted = allocated_size = 0;
    memset(sink, 0, sizeof(sink));
}
static void check_clean(void) { assert(!live_allocations); }
static void check_prefix(uint64_t n) { assert(accepted == n && memcmp(user_bytes, sink, (size_t)n) == 0); }
int main(void) {
    for (unsigned i = 0; i < sizeof(user_bytes); i++) user_bytes[i] = (uint8_t)(i * 17 + 3);
    const uint64_t cap = SYSCALL_FILE_WRITE_CHUNK;
    reset(); assert(syscall_vfs_write(-1, NULL, 0) == 0);
    assert(!validations && !allocations && !write_calls);
    reset(); range_ok = 0;
    assert(syscall_vfs_write(3, user_bytes, 4096) == -EFAULT);
    assert(!allocations && !copy_calls && !write_calls);
    reset(); assert(syscall_vfs_write(3, user_bytes, (uint64_t)INT64_MAX + 1) == -EINVAL);
    assert(!validations && !allocations && !write_calls);

    reset(); assert(syscall_vfs_write(3, user_bytes, 4096) == 4096);
    assert(write_calls == (int)((4096 + cap - 1) / cap));
    assert(max_chunk == (cap < 4096 ? cap : 4096)); check_prefix(4096); check_clean();
    assert(allocations == (cap > 256 ? 1 : 0) && frees == allocations);
    reset(); assert(syscall_vfs_write(3, user_bytes, 257) == 257);
    if (cap > 256) assert(allocated_size == 257);
    check_prefix(257); check_clean();
    reset(); assert(syscall_vfs_write(3, user_bytes, 128) == 128);
    assert(!allocations && write_calls == 1); check_prefix(128);
    reset(); assert(syscall_vfs_write(3, user_bytes, 2 * cap + 17) == (int64_t)(2 * cap + 17));
    assert(write_calls == 3); check_prefix(2 * cap + 17); check_clean();

    reset(); copy_fault_at = 1;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == -EFAULT);
    assert(!write_calls); check_clean();
    reset(); copy_fault_at = 2;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == (int64_t)cap);
    assert(write_calls == 1); check_prefix(cap); check_clean();
    reset(); write_fault_at = 1;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == -EIO); check_clean();
    reset(); write_fault_at = 2;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == (int64_t)cap);
    check_prefix(cap); check_clean();
    reset(); short_at = 1;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == (int64_t)(cap / 2));
    assert(write_calls == 1); check_prefix(cap / 2); check_clean();
    reset(); zero_at = 1;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == 0 && write_calls == 1); check_clean();
    reset(); impossible_at = 1;
    assert(syscall_vfs_write(3, user_bytes, cap + 17) == -EIO); check_clean();
    reset(); invalid_fd = 1;
    assert(syscall_vfs_write(3, user_bytes, 4096) == -EBADF && !allocations); check_clean();

    reset(); oom = 1;
    assert(syscall_vfs_write(3, user_bytes, 4096) == 4096);
    assert(write_calls == 16 && max_chunk == 256 && !frees);
    check_prefix(4096); check_clean();
    reset(); node.type = VFS_TYPE_FIFO;
    assert(syscall_vfs_write(3, user_bytes, 4096) == 4096 && write_calls == 16 && !allocations);
    check_clean();
    reset(); node.type = VFS_TYPE_CHARDEV;
    assert(syscall_vfs_write(3, user_bytes, 4096) == 4096 && write_calls == 16 && !allocations);
    reset(); devfs = 1; /* even a file-shaped device must remain excluded */
    assert(syscall_vfs_write(1, user_bytes, 4096) == 4096 && write_calls == 16 && !allocations);
    reset(); node.type = VFS_TYPE_FIFO; copy_fault_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 4096) == -EFAULT);
    assert(write_calls == 1 && !allocations); check_prefix(256); check_clean();
    reset(); devfs = 1; copy_fault_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 4096) == -EFAULT);
    assert(write_calls == 1 && !allocations); check_prefix(256); check_clean();
    reset(); oom = 1; copy_fault_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 4096) == 256);
    assert(write_calls == 1); check_prefix(256); check_clean();
    reset(); short_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 2 * cap + 17) == (int64_t)(cap + cap / 2));
    assert(write_calls == 2); check_prefix(cap + cap / 2); check_clean();
    reset(); zero_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 2 * cap + 17) == (int64_t)cap);
    assert(write_calls == 2); check_prefix(cap); check_clean();
    reset(); impossible_at = 2;
    assert(syscall_vfs_write(3, user_bytes, 2 * cap + 17) == (int64_t)cap);
    assert(write_calls == 2); check_prefix(cap); check_clean();
    reset(); /* redirected stdout is an ordinary file, not the console */
    assert(syscall_vfs_write(1, user_bytes, 4096) == 4096);
    assert(write_calls == (int)((4096 + cap - 1) / cap)); check_prefix(4096); check_clean();
    printf("write bounce: PASS (chunk=%u, payload, short/error/fault prefixes, zero, OOM, cleanup, endpoint policy)\n",
           (unsigned)SYSCALL_FILE_WRITE_CHUNK);
    return 0;
}
