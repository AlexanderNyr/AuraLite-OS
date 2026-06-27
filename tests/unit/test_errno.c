/*
 * test_errno.c — host-side unit test for the P1 errno foundations.
 *
 * Verifies:
 *   1. All required E* constants are defined with the correct Linux ABI value.
 *   2. POSIX aliases (EWOULDBLOCK, EDEADLOCK, ENOTSUP) equal their targets.
 *   3. The in-band negative-errno decode contract: values in the reserved
 *      band [(unsigned long)-4095, (unsigned long)-1] decode to errno + (-1),
 *      while legitimately large values (offsets, addresses) do not.
 *
 * Built and run by `make test-unit` with the host compiler under
 * -std=c11 -Wall -Wextra -Werror.
 */
#include <stdint.h>
#include <stdio.h>

/* Pull in the public libc errno definitions under test. */
#include "libc/include/errno.h"

/* Provide the errno storage that libc/include/errno.h declares via the
 * __errno_location() accessor, so this test links standalone. */
static int test_errno_cell = 0;
int *__errno_location(void) { return &test_errno_cell; }

static int failures = 0;

#define CHECK(cond) do {                                              \
        if (cond) {                                                   \
            printf("PASS: %s\n", #cond);                              \
        } else {                                                      \
            printf("FAIL: %s\n", #cond);                              \
            failures++;                                               \
        }                                                             \
    } while (0)

#define CHECK_EQ(name, val) do {                                      \
        if ((name) == (val)) {                                        \
            printf("PASS: %s == %d\n", #name, (int)(val));            \
        } else {                                                      \
            printf("FAIL: %s == %d (got %d)\n", #name, (int)(val),    \
                   (int)(name));                                      \
            failures++;                                               \
        }                                                             \
    } while (0)

/* Replica of the libc in-band decoder (libc/src/libc.c:syscall_ret) so the
 * contract can be exercised without the syscall asm stub. */
#define SYSCALL_MAX_ERRNO 4095UL
static long decode(int64_t raw) {
    if ((unsigned long)raw >= (unsigned long)-SYSCALL_MAX_ERRNO) {
        errno = (int)(-raw);
        return -1;
    }
    return (long)raw;
}

int main(void) {
    /* --- 1. Linux ABI numeric values (errno-base) --- */
    CHECK_EQ(EPERM, 1);
    CHECK_EQ(ENOENT, 2);
    CHECK_EQ(ESRCH, 3);
    CHECK_EQ(EINTR, 4);
    CHECK_EQ(EIO, 5);
    CHECK_EQ(EBADF, 9);
    CHECK_EQ(ECHILD, 10);
    CHECK_EQ(EAGAIN, 11);
    CHECK_EQ(ENOMEM, 12);
    CHECK_EQ(EACCES, 13);
    CHECK_EQ(EFAULT, 14);
    CHECK_EQ(EEXIST, 17);
    CHECK_EQ(ENOTDIR, 20);
    CHECK_EQ(EISDIR, 21);
    CHECK_EQ(EINVAL, 22);
    CHECK_EQ(EMFILE, 24);
    CHECK_EQ(ENOSPC, 28);
    CHECK_EQ(ESPIPE, 29);
    CHECK_EQ(EPIPE, 32);
    CHECK_EQ(ERANGE, 34);

    /* --- generic --- */
    CHECK_EQ(ENAMETOOLONG, 36);
    CHECK_EQ(ENOSYS, 38);
    CHECK_EQ(ENOTEMPTY, 39);
    CHECK_EQ(EOVERFLOW, 75);
    CHECK_EQ(EILSEQ, 84);
    CHECK_EQ(EOPNOTSUPP, 95);
    CHECK_EQ(ETIMEDOUT, 110);
    CHECK_EQ(ECONNREFUSED, 111);

    /* --- 2. POSIX aliases --- */
    CHECK_EQ(EWOULDBLOCK, EAGAIN);
    CHECK_EQ(EDEADLOCK, EDEADLK);
    CHECK_EQ(ENOTSUP, EOPNOTSUPP);

    /* --- 3. in-band decode contract --- */
    errno = 0;
    CHECK(decode(0) == 0);                       /* success: zero */
    CHECK(decode(42) == 42);                     /* success: positive */
    CHECK(errno == 0);                           /* errno untouched on success */

    errno = 0;
    CHECK(decode(-2) == -1 && errno == ENOENT);  /* -ENOENT decodes */
    errno = 0;
    CHECK(decode(-EINVAL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(decode(-EFAULT) == -1 && errno == EFAULT);

    /* A legitimately large value (e.g. a near-max file offset or a high
     * mmap address) must NOT be misread as an error: it lies outside the
     * reserved [-4095, -1] band. */
    errno = 0;
    int64_t big_off = 0x7FFFFFFFFFFF0000LL;      /* huge positive offset */
    CHECK(decode(big_off) == (long)big_off && errno == 0);

    /* The boundary: -4095 is the largest encodable errno; -4096 is NOT in
     * the band and must be treated as a (bizarre but valid) success value. */
    errno = 0;
    CHECK(decode(-4095) == -1 && errno == 4095);
    errno = 0;
    CHECK(decode(-4096) == (long)(int64_t)-4096 && errno == 0);

    if (failures == 0) {
        printf("test_errno: ALL PASS\n");
        return 0;
    }
    printf("test_errno: %d FAILURE(S)\n", failures);
    return 1;
}
