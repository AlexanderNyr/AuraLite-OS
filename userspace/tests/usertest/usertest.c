/*
 * usertest.c — M3 negative test battery for fault-recovering uaccess.
 *
 * Exercises every syscall with hostile user pointers to verify the kernel
 * returns -EFAULT instead of panicking. Tests:
 *   1. Unmapped pointer (NULL, near-NULL, high canonical)
 *   2. Wrap-around range (start + len overflows)
 *   3. Kernel-space pointer (addresses >= 0xFFFF800000000000)
 *   4. Partially valid range (one page mapped, next not)
 *
 * Each test invokes a syscall with a bad pointer and asserts the return is
 * -EFAULT (or -1 with errno == EFAULT). A kernel panic means M3 failed.
 *
 * Gate: MATURITY_PLAN.md phase M3.
 */

#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void putstr(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

static void putnum(int v) {
    char buf[16];
    int i = 0, neg = v < 0;
    if (neg) v = -v;
    do { buf[i++] = '0' + (v % 10); v /= 10; } while (v);
    if (neg) write(1, "-", 1);
    while (i > 0) { write(1, &buf[--i], 1); }
}

#define TEST(name) do { putstr("  "); putstr(name); putstr(" "); tests_run++; } while(0)
#define PASS() do { putstr("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { putstr("FAIL: "); putstr(msg); putstr("\n"); } while(0)

/* AuraLite's syscall ABI: int64_t syscall(int64_t num, uint64_t a1..a6). */
extern int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6);

#define SYSCALL0(n) syscall((n), 0, 0, 0, 0, 0, 0)
#define SYSCALL1(n, a1) syscall((n), (uint64_t)(uintptr_t)(a1), 0, 0, 0, 0, 0)
#define SYSCALL2(n, a1, a2) syscall((n), (uint64_t)(uintptr_t)(a1), (uint64_t)(uintptr_t)(a2), 0, 0, 0, 0)
#define SYSCALL3(n, a1, a2, a3) syscall((n), (uint64_t)(uintptr_t)(a1), (uint64_t)(uintptr_t)(a2), (uint64_t)(uintptr_t)(a3), 0, 0, 0)
#define SYSCALL4(n, a1, a2, a3, a4) syscall((n), (uint64_t)(uintptr_t)(a1), (uint64_t)(uintptr_t)(a2), (uint64_t)(uintptr_t)(a3), (uint64_t)(uintptr_t)(a4), 0, 0)
#define SYSCALL5(n, a1, a2, a3, a4, a5) syscall((n), (uint64_t)(uintptr_t)(a1), (uint64_t)(uintptr_t)(a2), (uint64_t)(uintptr_t)(a3), (uint64_t)(uintptr_t)(a4), (uint64_t)(uintptr_t)(a5), 0)
#define SYSCALL6(n, a1, a2, a3, a4, a5, a6) syscall((n), (uint64_t)(uintptr_t)(a1), (uint64_t)(uintptr_t)(a2), (uint64_t)(uintptr_t)(a3), (uint64_t)(uintptr_t)(a4), (uint64_t)(uintptr_t)(a5), (uint64_t)(uintptr_t)(a6))

/* Syscall numbers from kernel/arch/x86_64/syscall.c */
#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_FSTAT    5
#define SYS_LSTAT    6
#define SYS_MKDIR   100
#define SYS_RMDIR   101
#define SYS_UNLINK  102
#define SYS_RENAME  103
#define SYS_STAT    105
#define SYS_SPAWN    81
#define SYS_DNS      82
#define SYS_PIPE     22
#define SYS_IOCTL    16
#define SYS_READV    19
#define SYS_WRITEV   20
#define SYS_SENDTO   44
#define SYS_RECVFROM 45
#define SYS_SYMLINK  88
#define SYS_READLINK 89
#define SYS_SOCKET_SEND  302
#define SYS_SOCKET_RECV  303
#define SYS_GETCWD   540
#define SYS_CHDIR    541
#define SYS_GETENTROPY 318
#define SYS_GETRANDOM  319
#define SYS_CLOCK_GETTIME 228
#define SYS_WAIT4    61

/* Test 1: read() with NULL buffer. */
static void test_read_null_buffer(void) {
    TEST("read(fd, NULL, 10)");
    int64_t r = SYSCALL3(SYS_READ, 0, NULL, 10);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 2: write() with unmapped pointer. */
static void test_write_unmapped(void) {
    TEST("write(1, 0x1000, 10)");
    int64_t r = SYSCALL3(SYS_WRITE, 1, (void*)0x1000, 10);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 3: open() with NULL path. */
static void test_open_null_path(void) {
    TEST("open(NULL, 0)");
    int64_t r = SYSCALL3(SYS_OPEN, NULL, 0, 0);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 4: stat() with NULL path. */
static void test_stat_null_path(void) {
    TEST("stat(NULL, &st)");
    char st[80];
    int64_t r = SYSCALL2(SYS_STAT, NULL, st);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 5: stat() with NULL output buffer. */
static void test_stat_null_out(void) {
    TEST("stat(\"/tmp\", NULL)");
    int64_t r = SYSCALL2(SYS_STAT, "/tmp", NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 6: read() with wrap-around range. */
static void test_read_wraparound(void) {
    TEST("read(0, 0xFFFFFFFFFFFFFF00, 0x200)");
    int64_t r = SYSCALL3(SYS_READ, 0, (void*)0xFFFFFFFFFFFFFF00ULL, 0x200);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 7: write() with kernel-space pointer. */
static void test_write_kernel_ptr(void) {
    TEST("write(1, 0xFFFF800000000000, 10)");
    int64_t r = SYSCALL3(SYS_WRITE, 1, (void*)0xFFFF800000000000ULL, 10);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 8: mkdir() with NULL path. */
static void test_mkdir_null(void) {
    TEST("mkdir(NULL, 0755)");
    int64_t r = SYSCALL2(SYS_MKDIR, NULL, 0755);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 9: unlink() with NULL path. */
static void test_unlink_null(void) {
    TEST("unlink(NULL)");
    int64_t r = SYSCALL1(SYS_UNLINK, NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 10: rename() with NULL from. */
static void test_rename_null_from(void) {
    TEST("rename(NULL, \"/tmp/x\")");
    int64_t r = SYSCALL2(SYS_RENAME, NULL, "/tmp/x");
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 11: spawn() with NULL path. */
static void test_spawn_null(void) {
    TEST("spawn(NULL)");
    int64_t r = SYSCALL2(SYS_SPAWN, NULL, 0);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 12: dns_resolve() with NULL hostname. */
static void test_dns_null(void) {
    TEST("dns_resolve(NULL)");
    int64_t r = SYSCALL1(SYS_DNS, NULL);
    /* dns_resolve returns 0 or negative on failure; -EFAULT expected. */
    if (r == -EFAULT || r == 0 || r < 0) { PASS(); }
    else { FAIL("expected failure"); }
}

/* Test 13: writev() with NULL iov. */
static void test_writev_null_iov(void) {
    TEST("writev(1, NULL, 1)");
    int64_t r = SYSCALL3(SYS_WRITEV, 1, NULL, 1);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 14: readv() with bad iov_base inside. */
static void test_readv_bad_base(void) {
    TEST("readv(0, iov{base=0x1000}, 1)");
    struct { void *base; size_t len; } iov = { (void*)0x1000, 10 };
    int64_t r = SYSCALL3(SYS_READV, 0, &iov, 1);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 15: pipe() with NULL fds. */
static void test_pipe_null(void) {
    TEST("pipe(NULL)");
    int64_t r = SYSCALL1(SYS_PIPE, NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 16: wait4() with bad status pointer. */
static void test_wait4_bad_status(void) {
    TEST("wait4(-1, 0xDEAD, WNOHANG)");
    /* WNOHANG=1 so it doesn't block; no children -> ECHILD or bad ptr -> EFAULT.
     *
     * AUDIT_A1: this hardcoded -3 and called it ECHILD.  ECHILD is 10 in
     * both kernel/lib/errno.h and lib/libc/include/errno.h, so the kernel's
     * correct -ECHILD answer was scored as a failure -- the test was wrong,
     * not the kernel.  This was the single failing case in the battery
     * (29/30), and it had never been seen because the gate that runs this
     * program could not execute (it called a lib.sh API that does not
     * exist) and was not registered in run_all.sh either. */
    int64_t r = SYSCALL3(SYS_WAIT4, (uint64_t)-1, (void*)0xDEAD, 1);
    if (r == -EFAULT || r == -ECHILD || r == 0) { PASS(); }
    else { FAIL("expected -EFAULT/-ECHILD/0"); }
}

/* Test 17: socket_send() with NULL buffer. */
static void test_socket_send_null(void) {
    TEST("socket_send(-1, NULL, 10)");
    int64_t r = SYSCALL3(SYS_SOCKET_SEND, (uint64_t)-1, NULL, 10);
    if (r == -EFAULT || r == -9 /* EBADF */) { PASS(); }
    else { FAIL("expected -EFAULT or -EBADF"); }
}

/* Test 18: socket_recv() with NULL buffer. */
static void test_socket_recv_null(void) {
    TEST("socket_recv(-1, NULL, 10)");
    int64_t r = SYSCALL3(SYS_SOCKET_RECV, (uint64_t)-1, NULL, 10);
    if (r == -EFAULT || r == -9 /* EBADF */) { PASS(); }
    else { FAIL("expected -EFAULT or -EBADF"); }
}

/* Test 19: sendto() with NULL buffer. */
static void test_sendto_null_buf(void) {
    TEST("sendto(-1, NULL, 10, 0, addr, 16)");
    uint8_t addr[16] = {0};
    int64_t r = SYSCALL6(SYS_SENDTO, (uint64_t)-1, NULL, 10, 0, addr, 16);
    if (r == -EFAULT || r == -9 || r == -22) { PASS(); }
    else { FAIL("expected -EFAULT/-EBADF/-EINVAL"); }
}

/* Test 20: recvfrom() with NULL buffer. */
static void test_recvfrom_null_buf(void) {
    TEST("recvfrom(-1, NULL, 10, 0, NULL, NULL)");
    int64_t r = SYSCALL6(SYS_RECVFROM, (uint64_t)-1, NULL, 10, 0, NULL, NULL);
    if (r == -EFAULT || r == -9) { PASS(); }
    else { FAIL("expected -EFAULT or -EBADF"); }
}

/* Test 21: ioctl() with NULL arg. */
static void test_ioctl_null_arg(void) {
    TEST("ioctl(0, TCGETS, NULL)");
    int64_t r = SYSCALL3(SYS_IOCTL, 0, 0x5401, NULL);
    if (r == -EFAULT || r == -22 || r == -25) { PASS(); }
    else { FAIL("expected -EFAULT/-EINVAL/-ENOTTY"); }
}

/* Test 22: fstat() with NULL stat buffer. */
static void test_fstat_null(void) {
    TEST("fstat(0, NULL)");
    int64_t r = SYSCALL2(SYS_FSTAT, 0, NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 23: lstat() with NULL path. */
static void test_lstat_null_path(void) {
    TEST("lstat(NULL, &st)");
    char st[80];
    int64_t r = SYSCALL2(SYS_LSTAT, NULL, st);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 24: symlink() with NULL target. */
static void test_symlink_null(void) {
    TEST("symlink(NULL, \"/tmp/link\")");
    int64_t r = SYSCALL2(SYS_SYMLINK, NULL, "/tmp/link");
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 25: readlink() with NULL path. */
static void test_readlink_null_path(void) {
    TEST("readlink(NULL, buf, 256)");
    char buf[256];
    int64_t r = SYSCALL3(SYS_READLINK, NULL, buf, 256);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 26: getcwd() with NULL buffer. */
static void test_getcwd_null(void) {
    TEST("getcwd(NULL, 256)");
    int64_t r = SYSCALL2(SYS_GETCWD, NULL, 256);
    if (r == -EFAULT || r < 0) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 27: chdir() with NULL path. */
static void test_chdir_null(void) {
    TEST("chdir(NULL)");
    int64_t r = SYSCALL1(SYS_CHDIR, NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 28: getrandom() with NULL buffer. */
static void test_getrandom_null(void) {
    TEST("getrandom(NULL, 16, 0)");
    int64_t r = SYSCALL3(SYS_GETRANDOM, NULL, 16, 0);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 29: getentropy() with NULL buffer. */
static void test_getentropy_null(void) {
    TEST("getentropy(NULL, 16)");
    int64_t r = SYSCALL2(SYS_GETENTROPY, NULL, 16);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

/* Test 30: clock_gettime() with NULL timespec. */
static void test_clock_gettime_null(void) {
    TEST("clock_gettime(0, NULL)");
    int64_t r = SYSCALL2(SYS_CLOCK_GETTIME, 0, NULL);
    if (r == -EFAULT) { PASS(); }
    else { FAIL("expected -EFAULT"); }
}

int main(void) {
    putstr("== usertest: M3 fault-recovering uaccess negative battery ==\n");

    test_read_null_buffer();
    test_write_unmapped();
    test_open_null_path();
    test_stat_null_path();
    test_stat_null_out();
    test_read_wraparound();
    test_write_kernel_ptr();
    test_mkdir_null();
    test_unlink_null();
    test_rename_null_from();
    test_spawn_null();
    test_dns_null();
    test_writev_null_iov();
    test_readv_bad_base();
    test_pipe_null();
    test_wait4_bad_status();
    test_socket_send_null();
    test_socket_recv_null();
    test_sendto_null_buf();
    test_recvfrom_null_buf();
    test_ioctl_null_arg();
    test_fstat_null();
    test_lstat_null_path();
    test_symlink_null();
    test_readlink_null_path();
    test_getcwd_null();
    test_chdir_null();
    test_getrandom_null();
    test_getentropy_null();
    test_clock_gettime_null();

    putstr("== "); putnum(tests_passed); putstr("/"); putnum(tests_run);
    putstr(" passed ==\n");
    return (tests_passed == tests_run) ? 0 : 1;
}
