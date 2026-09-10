/* tests/unit/test_lx_sig.c — LX_COMPAT L3 host gate.
 *
 * Pins the Linux x86-64 signal structures in kernel/lx/lx_sig.h to the
 * ABI they marshal, so the rt_sigframe build and parse sides (signal.c)
 * cannot drift apart or off the Linux layout.  The sizes/offsets are the
 * measured facts from Linux's asm/sigcontext.h + kernel siginfo:
 *
 *   struct kernel_sigaction = 32   (handler, flags, restorer, mask)
 *   struct sigcontext_64   = 256   (r8..rsp, rip/eflags, cs/gs/fs, ..., fpstate, reserved)
 *   struct ucontext        = 936   (flags, link, stack, mcontext, sigmask, 30 ints, 512 fpregs)
 *   kernel siginfo_t       = 128
 *   rt_sigframe            = 1072  (pretcode + uc + info)
 *
 * The critical cross-side contracts are the offsets the build and parse
 * halves share: the ucontext at +8 (right after pretcode, which doubles as
 * the handler's return address) and the siginfo at +944.  musl's
 * __restore_rt issues rt_sigreturn with RSP = frame+8, so the parse side
 * reads uc exactly at RSP — an offset mistake here would silently corrupt
 * the interrupted register state, which is exactly the class of bug a host
 * pin exists to catch.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "kernel/lx/lx_sig.h"

static int fails = 0;

#define PIN(what, got, want)                                             \
    do {                                                                 \
        if ((uint64_t)(got) != (uint64_t)(want)) {                       \
            printf("  FAIL: %s = %llu, want %llu\n", what,               \
                   (unsigned long long)(got), (unsigned long long)(want)); \
            fails++;                                                     \
        }                                                                \
    } while (0)

int main(void) {
    printf("[lx-sig] kernel_sigaction (rt_sigaction 13)\n");
    PIN("sizeof(kernel_sigaction)", sizeof(struct lx_kernel_sigaction), 32);
    PIN("k_sa_handler", offsetof(struct lx_kernel_sigaction, k_sa_handler), 0);
    PIN("sa_flags",     offsetof(struct lx_kernel_sigaction, sa_flags),     8);
    PIN("sa_restorer",  offsetof(struct lx_kernel_sigaction, sa_restorer), 16);
    PIN("sa_mask",      offsetof(struct lx_kernel_sigaction, sa_mask),     24);

    printf("[lx-sig] sigcontext_64 (inside the ucontext)\n");
    PIN("sizeof(sigcontext)", sizeof(struct lx_sigcontext), 256);
    PIN("mcontext.r8",   offsetof(struct lx_sigcontext, r8),    0);
    PIN("mcontext.rsp",  offsetof(struct lx_sigcontext, rsp),  120);
    PIN("mcontext.rip",  offsetof(struct lx_sigcontext, rip),  128);
    PIN("mcontext.eflags", offsetof(struct lx_sigcontext, eflags), 136);
    PIN("mcontext.cs",   offsetof(struct lx_sigcontext, cs),   144);

    printf("[lx-sig] ucontext (the parse/build payload)\n");
    PIN("sizeof(ucontext)", sizeof(struct lx_ucontext), 936);
    PIN("uc_mcontext",   offsetof(struct lx_ucontext, uc_mcontext), 40);
    PIN("uc_sigmask",    offsetof(struct lx_ucontext, uc_sigmask), 296);

    printf("[lx-sig] siginfo + rt_sigframe\n");
    PIN("sizeof(siginfo)", sizeof(struct lx_siginfo), 128);
    PIN("sizeof(rt_sigframe)", sizeof(struct lx_rt_sigframe), 1072);
    PIN("frame->pretcode", LX_SIGFRAME_PRETCODE_OFF, 0);
    PIN("frame->uc",       LX_SIGFRAME_UC_OFF,        8);
    PIN("frame->info",     LX_SIGFRAME_INFO_OFF,      944);
    PIN("frame size",      LX_SIGFRAME_SIZE,          1072);

    /* The cross-side contract: the parse side reads the ucontext exactly at
     * RSP (musl's restorer leaves RSP = frame+8 after the handler's ret). */
    PIN("uc offset == 8 (parse side reads uc at RSP)", LX_SIGFRAME_UC_OFF, 8);

    if (fails == 0) {
        printf("test_lx_sig: all checks passed\n");
        return 0;
    }
    printf("test_lx_sig: %d check(s) failed\n", fails);
    return 1;
}
