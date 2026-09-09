/* tests/unit/test_lx_translate.c — LX_COMPAT L1 host gate.
 *
 * Pins the lx number map to the measured facts (kernel/lx/lx_translate.c):
 *  - every identity row really is an identity (the native arm at that
 *    number has Linux's semantics — each was checked in syscall.c);
 *  - every alias row lands on the native number with the same contract;
 *  - the lx-only arms carry their Linux numbers;
 *  - the COLLIDING numbers stay unmapped: 80 is Linux getcwd but native
 *    LISTDIR, 82 is Linux rename but native DNS, 293 is Linux rseq but
 *    native pipe2 — a native process must keep reaching those native
 *    arms, so the lx map must NOT quietly reinterpret them;
 *  - the /linux path-prefix rule matches exactly the subtree.
 *
 * This is the L1 gate's "the native table is untouched" half: the map's
 * job is to keep two number spaces from ever aliasing by accident.
 *
 * Every Linux number below was checked against asm/unistd_64.h, not
 * recalled from memory: the first draft had set_tid_address at 96 (it
 * is 218; 96 is gettimeofday) and the L1 gate caught the swap.
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
    expect(3,   3,   "close");
    expect(8,   8,   "lseek");
    expect(9,   9,   "mmap (glibc static startup: TLS block)");
    expect(10,  10,  "mprotect (RELRO)");
    expect(20,  20,  "writev (glibc stdio flush)");
    expect(96,  96,  "gettimeofday (NOT set_tid_address: 96/218 were swapped in draft 1)");
    expect(35,  35,  "nanosleep");
    expect(60,  60,  "exit");
    expect(158, 158, "arch_prctl");

    printf("[lx] lx-only arms\n");
    expect(63,  LX_ARM_UNAME,           "uname");
    expect(12,  LX_ARM_BRK,             "brk (Linux-exact return)");
    expect(186, LX_ARM_GETTID,          "gettid");
    expect(218, LX_ARM_SET_TID_ADDRESS, "set_tid_address (x86-64 nr 218)");
    expect(231, LX_ARM_EXIT_GROUP,      "exit_group");
    expect(267, LX_ARM_READLINKAT,      "readlinkat (/proc/self/exe: ENOENT)");
    expect(273, LX_ARM_SET_ROBUST_LIST, "set_robust_list (accept-and-store)");
    expect(302, LX_ARM_PRLIMIT64,       "prlimit64 (no limits enforced)");

    printf("[lx] alias rows (same contract, native number)\n");
    expect(102, 500, "getuid -> SYS_GETUID");
    expect(104, 502, "getgid -> SYS_GETGID");
    expect(107, 501, "geteuid -> SYS_GETEUID");
    expect(108, 503, "getegid -> SYS_GETEGID");
    expect(318, 319, "getrandom -> SYS_GETRANDOM");

    printf("[lx] collisions stay unmapped (their native arms belong to "
           "native processes)\n");
    expect(80,  LX_UNMAPPED, "getcwd: native 80 is LISTDIR (L2 owns it)");
    expect(81,  LX_UNMAPPED, "chdir: native 81 is SPAWN (L2)");
    expect(82,  LX_UNMAPPED, "rename: native 82 is DNS (L2)");
    expect(83,  LX_UNMAPPED, "mkdir: native 83 is NET_CONNECT (L2)");
    expect(217, LX_UNMAPPED, "getdents64 (L2)");
    expect(257, LX_UNMAPPED, "openat (L2)");
    expect(262, LX_UNMAPPED, "newfstatat (L2)");
    expect(293, LX_UNMAPPED, "rseq-pipe2 collision zone: native 293 is PIPE2; Linux rseq is 334 (L4)");
    expect(334, LX_UNMAPPED, "rseq: glibc falls back to plain sequences (L4)");
    expect(202, LX_UNMAPPED, "futex: native 530 (L4)");
    expect(439, LX_UNMAPPED, "faccessat2 (L2)");
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
