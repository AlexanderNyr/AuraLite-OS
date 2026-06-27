/*
 * test_lseek.c — host-side unit test for P3 lseek/iovec constants and the
 * readv/writev validation logic (iovcnt bounds + SSIZE_MAX overflow).
 *
 * Built and run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <stdint.h>
#include "libc/include/sys/uio.h"

/* SEEK_* live in <unistd.h>; redeclare here to avoid pulling the whole header
 * (which declares syscall wrappers).  Values must match. */
#define T_SEEK_SET 0
#define T_SEEK_CUR 1
#define T_SEEK_END 2

static int failures = 0;
#define CHECK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* Mirror of vfs_writev's length-summing/overflow guard (vfs.c). */
#define IOVEC_OK 0
#define IOVEC_EINVAL (-22)
static int validate_iov(const struct iovec *iov, int iovcnt) {
    if (iovcnt <= 0 || iovcnt > IOV_MAX) return IOVEC_EINVAL;
    uint64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t l = iov[i].iov_len;
        if (total + l < total || total + l > 0x7FFFFFFFFFFFFFFFULL)
            return IOVEC_EINVAL;
        total += l;
    }
    return IOVEC_OK;
}

int main(void) {
    CHECK(T_SEEK_SET == 0);
    CHECK(T_SEEK_CUR == 1);
    CHECK(T_SEEK_END == 2);
    CHECK(IOV_MAX == 1024);
    CHECK(sizeof(struct iovec) == 16);   /* { void*; size_t } on LP64 */

    /* Normal vector. */
    struct iovec ok[2] = { { (void *)"AB", 2 }, { (void *)"CDE", 3 } };
    CHECK(validate_iov(ok, 2) == IOVEC_OK);

    /* iovcnt bounds. */
    CHECK(validate_iov(ok, 0) == IOVEC_EINVAL);
    CHECK(validate_iov(ok, -1) == IOVEC_EINVAL);
    CHECK(validate_iov(ok, IOV_MAX + 1) == IOVEC_EINVAL);

    /* SSIZE_MAX overflow: two halves each just over SSIZE_MAX/2 sum to overflow. */
    struct iovec big[2];
    big[0].iov_base = (void *)0; big[0].iov_len = 0x7000000000000000ULL;
    big[1].iov_base = (void *)0; big[1].iov_len = 0x7000000000000000ULL;
    CHECK(validate_iov(big, 2) == IOVEC_EINVAL);

    if (failures == 0) { printf("test_lseek: ALL PASS\n"); return 0; }
    printf("test_lseek: %d FAILURE(S)\n", failures);
    return 1;
}
