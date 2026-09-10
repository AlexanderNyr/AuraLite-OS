/* tests/unit/test_lx_translate.c — LX_COMPAT L1/L2 host gate.
 *
 * Pins the lx number map to the measured facts (kernel/lx/lx_translate.c):
 *  - every identity row really is an identity (the native arm at that
 *    number has Linux's semantics AND its structures — ioctl/termios
 *    and open/O_* were measured for L2, not assumed);
 *  - every alias row lands on the native number with the same contract;
 *  - the lx-only arms carry their Linux numbers;
 *  - the COLLIDING numbers stay unmapped: 81 is Linux fchdir but
 *    native SPAWN, 82 is Linux rename but native DNS, 293 is Linux
 *    rseq-adjacent but native PIPE2 — a native process must keep
 *    reaching those native arms, so the lx map must NOT quietly
 *    reinterpret them.  (80, the L1 collision — Linux chdir vs native
 *    LISTDIR — now resolves through the 80->541 alias: translation
 *    runs BEFORE the dispatch, so a native process still reaches the
 *    native arm at 80.);
 *  - the /linux path-prefix rule matches exactly the subtree.
 *
 * This is the gate's "the native table is untouched" half: the map's
 * job is to keep two number spaces from ever aliasing by accident.
 *
 * Every Linux number below was checked against asm/unistd_64.h, not
 * recalled from memory: the first draft had set_tid_address at 96 (it
 * is 218; 96 is gettimeofday) and the L1 gate caught the swap.  The L2
 * pass fixed two labels L1 had written wrong — getcwd is 79 (not 80)
 * and chdir is 80 (not 81); the pinned numbers were what the map had
 * right all along.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/lx/lx.h"

static int fails = 0;

static void expect(uint32_t linux_nr, uint32_t want, const char *what) {
    uint32_t got = lx_translate(linux_nr);
    if (got != want) {
        printf("  FAIL: lx_translate(%u) = %u, want %u (%s)\n",
               linux_nr, got, want, what);
        fails++;
    }
}

int main(void) {
    printf("[lx] identity rows (native arm == Linux semantics)\n");
    expect(0,   0,   "read");
    expect(1,   1,   "write");
    expect(2,   2,   "open (O_* values match asm-generic in both vocabularies)");
    expect(3,   3,   "close");
    expect(8,   8,   "lseek");
    expect(9,   9,   "mmap (glibc static startup: TLS block)");
    expect(10,  10,  "mprotect (RELRO)");
    expect(11,  11,  "munmap (musl mallocng releases with it)");
    expect(14,  14,  "rt_sigprocmask (32-bit mask caveat is documented)");
    expect(16,  16,  "ioctl (TIOCGWINSZ/TCGETS: termios agrees on first 36 bytes)");
    expect(20,  20,  "writev (glibc stdio flush)");
    expect(72,  72,  "fcntl (F_SETFD/F_DUPFD_CLOEXEC: same command numbers)");
    expect(96,  96,  "gettimeofday (NOT set_tid_address: 96/218 were swapped in draft 1)");
    expect(35,  35,  "nanosleep");
    expect(60,  60,  "exit");
    expect(158, 158, "arch_prctl");
    expect(257, 257, "openat (native Q12 arm, AT_FDCWD join, same flags)");
    expect(228, 228, "clock_gettime (native 228; kernel_timespec == Linux timespec)");
    expect(32,  32,  "dup (native SYS_DUP, same shape)");
    expect(33,  33,  "dup2 (native SYS_DUP2, same argument order)");

    printf("[lx] lx-only arms\n");
    expect(63,  LX_ARM_UNAME,           "uname");
    expect(12,  LX_ARM_BRK,             "brk (Linux-exact return)");
    expect(186, LX_ARM_GETTID,          "gettid");
    expect(218, LX_ARM_SET_TID_ADDRESS, "set_tid_address (x86-64 nr 218)");
    expect(231, LX_ARM_EXIT_GROUP,      "exit_group");
    expect(267, LX_ARM_READLINKAT,      "readlinkat (/proc/self/exe: ENOENT)");
    expect(273, LX_ARM_SET_ROBUST_LIST, "set_robust_list (accept-and-store)");
    expect(302, LX_ARM_PRLIMIT64,       "prlimit64 (no limits enforced)");
    expect(4,   LX_ARM_STAT,            "stat (native arms speak struct vfs_stat: marshal)");
    expect(5,   LX_ARM_FSTAT,           "fstat (native 5 is the same trap: marshal)");
    expect(6,   LX_ARM_LSTAT,           "lstat (native SYS_LSTAT sits AT 6: marshal)");
    expect(217, LX_ARM_GETDENTS64,      "getdents64 (no native arm at 217)");
    expect(262, LX_ARM_NEWFSTATAT,      "newfstatat (native 262 fills vfs_stat: marshal)");

    printf("[lx] alias rows (same contract, native number)\n");
    expect(102, 500, "getuid -> SYS_GETUID");
    expect(104, 502, "getgid -> SYS_GETGID");
    expect(107, 501, "geteuid -> SYS_GETEUID");
    expect(108, 503, "getegid -> SYS_GETEGID");
    expect(318, 319, "getrandom -> SYS_GETRANDOM");
    expect(79,  540, "getcwd -> SYS_GETCWD (79 is getcwd on x86-64, not 80)");
    expect(80,  541, "chdir -> SYS_CHDIR (NOT native 80/LISTDIR: the L1 collision resolves here)");
    expect(105, 504, "setuid -> SYS_SETUID (busybox re-drops to its own uid)");
    expect(106, 505, "setgid -> SYS_SETGID (same shape)");

    printf("[lx] collisions stay unmapped (their native arms belong to "
           "native processes)\n");
    expect(81,  LX_UNMAPPED, "fchdir: native 81 is SPAWN (81 is fchdir on x86-64, not chdir)");
    expect(13,  LX_UNMAPPED, "rt_sigaction: the sigaction/sigframe marshal is L3's first job");
    expect(82,  LX_UNMAPPED, "rename: native 82 is DNS");
    expect(83,  LX_UNMAPPED, "mkdir: native 83 is NET_CONNECT");
    expect(40,  LX_UNMAPPED, "sendfile: busybox's copyfd falls back to read/write on ENOSYS");
    expect(293, LX_UNMAPPED, "rseq-pipe2 collision zone: native 293 is PIPE2; Linux rseq is 334 (L4)");
    expect(334, LX_UNMAPPED, "rseq: glibc falls back to plain sequences (L4)");
    expect(202, LX_UNMAPPED, "futex: native 530 (L4)");
    expect(439, LX_UNMAPPED, "faccessat2 (L3)");
    expect(9999, LX_UNMAPPED, "nowhere");

    printf("[lx] the /linux path-prefix rule\n");
    struct { const char *path; int want; } paths[] = {
        { "/linux/tests/hello",     1 },
        { "/linux/bin/busybox",     1 },
        { "/linux",                 0 },  /* the mount itself is not in */
        { "/linuxx/hello",          0 },  /* sibling prefix is not it */
        { "/bin/hello",             0 },
        { "linux/tests/hello",      0 },  /* relative paths never match */
        { "",                       0 },
        { NULL,                     0 },  /* NULL guard */
    };
    for (int i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
        int got = lx_path_is_lx(paths[i].path);
        if (got != paths[i].want) {
            printf("  FAIL: lx_path_is_lx(\"%s\") = %d, want %d\n",
                   paths[i].path ? paths[i].path : "(null)",
                   got, paths[i].want);
            fails++;
        }
    }

    if (fails == 0) {
        printf("test_lx_translate: all checks passed\n");
        return 0;
    }
    printf("test_lx_translate: %d check(s) failed\n", fails);
    return 1;
}
