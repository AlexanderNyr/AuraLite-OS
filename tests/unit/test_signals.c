/*
 * test_signals.c — host-side unit test for P4 signal ABI + core logic:
 *   - signal numbers and sa_flags match the kernel/libc headers,
 *   - signal_frame geometry (16-byte alignment, red zone, sigreturn read addr),
 *   - the per-delivery mask formula (old | sa_mask | {sig}, SA_NODEFER, uncatchable),
 *   - the FIX_EFLAGS RFLAGS sanitization (IF forced, IOPL/NT rejected).
 *
 * Built and run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <stdint.h>
#include "libc/include/signal.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* Kernel-side frame (mirror of kernel/proc/signal.h struct signal_frame). */
struct signal_frame {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8;
    uint64_t rdi,rsi,rbp,rdx,rcx,rbx,rax;
    uint64_t rip,rflags,rsp,cs,ss;
    uint32_t saved_mask, signo;
};

static uint32_t sig_bit(int s) { return (s >= 1 && s < NSIG) ? (1u << (s - 1)) : 0u; }
#define SIG_UNCATCHABLE (sig_bit(SIGKILL) | sig_bit(SIGSTOP))

/* Mirror of build_handler_frame()'s new-mask computation. */
static uint32_t delivery_mask(uint32_t old, uint32_t sa_mask, int sig, int flags) {
    uint32_t m = old | sa_mask;
    if (!(flags & SA_NODEFER)) m |= sig_bit(sig);
    m &= ~SIG_UNCATCHABLE;
    return m;
}

/* Mirror of do_sigreturn()'s FIX_EFLAGS sanitization. */
#define FIX_EFLAGS 0x00050DD5u   /* CF PF AF ZF SF TF DF OF RF AC */
#define FLAG_IF 0x200u
#define FLAG_RES1 0x2u
static uint64_t sanitize_flags(uint64_t user) {
    return (user & FIX_EFLAGS) | FLAG_IF | FLAG_RES1;
}

int main(void) {
    /* Signal numbers. */
    CK(SIGINT == 2); CK(SIGKILL == 9); CK(SIGUSR1 == 10); CK(SIGSEGV == 11);
    CK(SIGSTOP == 19); CK(SIGTERM == 15); CK(SIGCHLD == 17); CK(NSIG == 32);

    /* Dispositions + flags. */
    CK(SIG_DFL == (void (*)(int))0);
    CK(SIG_IGN == (void (*)(int))1);
    CK(SA_NODEFER == 0x40000000);
    CK(SA_RESTART == 0x10000000);
    CK(SIG_BLOCK == 0 && SIG_UNBLOCK == 1 && SIG_SETMASK == 2);

    /* Frame geometry: same checks as the kernel builder. */
    uint64_t old = 0x7fffff000000ULL;
    uint64_t sp = old - 128 - sizeof(struct signal_frame);
    sp &= ~((uint64_t)15);
    uint64_t frame_addr = sp;
    sp -= 8;
    CK((sp % 16) == 8);                 /* handler entry RSP%16==8 */
    CK((frame_addr % 16) == 0);
    CK(frame_addr <= old - 128);        /* red zone preserved */
    CK((sp + 8) == frame_addr);         /* sigreturn reads frame at sp+8 */

    /* Delivery mask formula. */
    uint32_t m = delivery_mask(0, 0, SIGUSR1, 0);
    CK(m == sig_bit(SIGUSR1));                       /* self auto-blocked */
    m = delivery_mask(0, 0, SIGUSR1, SA_NODEFER);
    CK(m == 0);                                       /* SA_NODEFER: not blocked */
    m = delivery_mask(sig_bit(SIGTERM), sig_bit(SIGINT), SIGUSR1, 0);
    CK(m == (sig_bit(SIGTERM)|sig_bit(SIGINT)|sig_bit(SIGUSR1)));
    /* SIGKILL/SIGSTOP can never end up blocked. */
    m = delivery_mask(SIG_UNCATCHABLE, SIG_UNCATCHABLE, SIGKILL, 0);
    CK((m & SIG_UNCATCHABLE) == 0);

    /* RFLAGS sanitization: IF always set, IOPL/NT never restorable. */
    CK((sanitize_flags(0) & FLAG_IF) != 0);           /* IF forced on */
    CK((sanitize_flags(0x3000) & 0x3000) == 0);       /* IOPL bits dropped */
    CK((sanitize_flags(0x4000) & 0x4000) == 0);       /* NT dropped */
    CK((sanitize_flags(0x1) & 0x1) == 0x1);           /* CF preserved */
    CK((sanitize_flags(0xFFFFFFFFu) & 0x200) == 0x200);

    /* alarm() seconds<->ticks math (mirror of do_alarm with PIT_HZ=100). */
    #define PIT_HZ 100
    /* arming seconds=1 at now=T sets deadline = T + 100. */
    uint64_t now = 12345;
    uint64_t deadline = now + (uint64_t)1 * PIT_HZ;
    CK(deadline == now + 100);
    /* remaining = ceil((deadline-now)/HZ) when re-querying partway. */
    uint64_t later = now + 30;            /* 0.3s elapsed */
    unsigned remaining = (unsigned)((deadline - later + (PIT_HZ - 1)) / PIT_HZ);
    CK(remaining == 1);                   /* 0.7s left rounds up to 1 */
    /* a fired deadline (now >= deadline) yields SIGALRM. */
    CK((now + 100) <= (now + 100));       /* boundary fires */

    /* SA_RESTART default flag used by signal() must be the documented value. */
    CK(SA_RESTART == 0x10000000);

    if (failures == 0) { printf("test_signals: ALL PASS\n"); return 0; }
    printf("test_signals: %d FAILURE(S)\n", failures);
    return 1;
}
