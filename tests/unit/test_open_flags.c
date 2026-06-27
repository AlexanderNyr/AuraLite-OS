/*
 * test_open_flags.c — host-side unit test for P2 open()/fcntl() flag ABI.
 *
 * Verifies the O_* / F_* / FD_CLOEXEC numeric values match the Linux
 * asm-generic ABI and that the access-mode field behaves as an enumerated
 * field under O_ACCMODE (not independent bits).  The kernel-side and libc
 * headers must agree on these values.
 *
 * Built and run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include "libc/include/fcntl.h"

/* fcntl.h declares open/creat/fcntl; provide trivial stubs so the test links
 * standalone (we only exercise the constants here). */
int open(const char *path, int flags, ...)  { (void)path; (void)flags; return -1; }
int creat(const char *path, int mode)        { (void)path; (void)mode; return -1; }
int fcntl(int fd, int cmd, ...)              { (void)fd; (void)cmd; return -1; }

static int failures = 0;

#define EQ(name, val) do {                                              \
        if ((name) == (val)) printf("PASS: %s == 0x%x\n", #name, (val)); \
        else { printf("FAIL: %s == 0x%x (got 0x%x)\n", #name, (val),     \
                      (unsigned)(name)); failures++; }                   \
    } while (0)

#define CHECK(cond) do {                                                \
        if (cond) printf("PASS: %s\n", #cond);                          \
        else { printf("FAIL: %s\n", #cond); failures++; }               \
    } while (0)

/* Mirror the kernel's access-mode classification. */
static int readable(int flags) {
    int a = flags & O_ACCMODE;
    return a == O_RDONLY || a == O_RDWR;
}
static int writable(int flags) {
    int a = flags & O_ACCMODE;
    return a == O_WRONLY || a == O_RDWR;
}

int main(void) {
    /* Access modes + mask. */
    EQ(O_RDONLY, 0x0000);
    EQ(O_WRONLY, 0x0001);
    EQ(O_RDWR,   0x0002);
    EQ(O_ACCMODE,0x0003);

    /* Creation / status flags. */
    EQ(O_CREAT,     0x0040);
    EQ(O_EXCL,      0x0080);
    EQ(O_TRUNC,     0x0200);
    EQ(O_APPEND,    0x0400);
    EQ(O_NONBLOCK,  0x0800);
    EQ(O_DIRECTORY, 0x10000);
    EQ(O_CLOEXEC,   0x80000);

    /* fcntl commands + fd flag. */
    EQ(F_DUPFD, 0);
    EQ(F_GETFD, 1);
    EQ(F_SETFD, 2);
    EQ(F_GETFL, 3);
    EQ(F_SETFL, 4);
    EQ(F_DUPFD_CLOEXEC, 1030);
    EQ(FD_CLOEXEC, 1);

    /* The O_RDONLY-is-zero pitfall: `flags & O_RDONLY` is ALWAYS false. */
    CHECK((O_RDONLY & O_RDONLY) == 0);
    CHECK(((O_RDWR) & O_RDONLY) == 0);   /* would-be bug if used for "readable" */

    /* Access-mode classification. */
    CHECK(readable(O_RDONLY)  && !writable(O_RDONLY));
    CHECK(!readable(O_WRONLY) &&  writable(O_WRONLY));
    CHECK(readable(O_RDWR)    &&  writable(O_RDWR));
    CHECK(readable(O_RDONLY | O_CREAT | O_TRUNC));   /* flags don't disturb acc */

    /* Distinct bit positions (no overlaps among the flag set). */
    CHECK((O_CREAT & O_EXCL) == 0);
    CHECK((O_TRUNC & O_APPEND) == 0);
    CHECK((O_APPEND & O_NONBLOCK) == 0);
    CHECK((O_CLOEXEC & O_ACCMODE) == 0);

    if (failures == 0) { printf("test_open_flags: ALL PASS\n"); return 0; }
    printf("test_open_flags: %d FAILURE(S)\n", failures);
    return 1;
}
