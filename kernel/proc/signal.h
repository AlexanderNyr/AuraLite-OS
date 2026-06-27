/*
 * signal.h — POSIX.1-2017 signal subsystem (kernel side).
 *
 * Phase P4 core: signal numbers, dispositions, per-process signal state, the
 * user-stack signal frame, and the delivery/return entry points.  Delivery is
 * driven from the return-to-user boundary (IRQ/exception return, which carries
 * a full `struct registers`, and the syscall-exit slow path which synthesises
 * one).  POSIX.1-2017 §2.4 "Signal Concepts".
 */
#ifndef AURALITE_KERNEL_PROC_SIGNAL_H
#define AURALITE_KERNEL_PROC_SIGNAL_H

#include <stdint.h>

struct registers;   /* kernel/arch/x86_64/isr.h */
struct tcb;

/* Standard signal numbers (POSIX / Linux asm-generic). */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9    /* cannot be caught/blocked/ignored */
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19    /* cannot be caught/blocked/ignored */
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGWINCH 28
#define NSIG     32    /* signals 1..31 are valid; bit (sig-1) in masks */

/* Dispositions for sa_handler. */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)

/* sa_flags. */
#define SA_NODEFER   0x40000000   /* don't auto-block the delivered signal */
#define SA_RESETHAND 0x80000000   /* reset to SIG_DFL on delivery (one-shot) */
#define SA_RESTART   0x10000000   /* restart interruptible syscalls (P4 follow-up) */
#define SA_SIGINFO   0x00000004   /* three-arg handler (frame still built; info P4+) */

/* sigprocmask how values. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef uint32_t sigset_t;   /* bit (signo-1); signals 1..31 fit in 32 bits */

/* Kernel/userspace shared sigaction layout — MUST match libc/include/signal.h.
 * sa_restorer is the address of the libc trampoline that the kernel pushes as
 * the handler's return address; on handler return it invokes SYS_SIGRETURN. */
struct sigaction {
    void    (*sa_handler)(int);   /* SIG_DFL / SIG_IGN / function pointer */
    uint32_t  sa_mask;            /* extra signals blocked during the handler */
    int       sa_flags;
    void    (*sa_restorer)(void); /* libc __sigreturn trampoline */
};

/*
 * Signal frame pushed onto the user stack before entering a handler.  Laid out
 * so rt_sigreturn can restore the full interrupted machine state.  The frame is
 * preceded on the stack by an 8-byte trampoline return address (libc
 * __sigreturn), so the handler sees RSP%16==8 at entry (SysV ABI §3.2.2).
 */
struct signal_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    uint64_t cs, ss;
    uint32_t saved_mask;          /* sigmask to restore on sigreturn */
    uint32_t signo;               /* which signal (diagnostic) */
};

/* Helpers for sigset_t bit (signo) <-> mask. */
static inline sigset_t sig_bit(int signo) {
    return (signo >= 1 && signo < NSIG) ? (1u << (signo - 1)) : 0u;
}

/* Signals that can never be caught, blocked, or ignored. */
#define SIG_UNCATCHABLE (sig_bit(SIGKILL) | sig_bit(SIGSTOP))

/* ---- API ---- */

/* Mark @signo pending on @target.  Safe to call from IRQ context. */
void signal_send(struct tcb *target, int signo);

/* Send @signo to the process identified by @pid (kill(2)):
 *   pid > 0  : that process;     pid == 0 : caller's process group;
 *   pid == -1: all processes;    pid < -1 : process group |pid|. */
int  signal_kill(int64_t pid, int signo);

/* Send @signo to every process in process group @pgid.  Returns 0 if at least
 * one process was found, -ESRCH otherwise.  Safe from IRQ context. */
int  signal_send_group(int64_t pgid, int signo);

/* Process-group / session syscalls. */
int64_t do_setsid(void);
int64_t do_setpgid(int64_t pid, int64_t pgid);
int64_t do_getpgid(int64_t pid);
int64_t do_getsid(int64_t pid);

/* Raise a synchronous signal on the current thread from a CPU exception,
 * forcing the default action if it is blocked/ignored.  @regs is the faulting
 * Ring-3 frame.  Returns 1 if a handler frame was set up on @regs (caller
 * should iret), 0 if the thread was terminated. */
int  signal_raise_fault(struct registers *regs, int signo);

/*
 * signal_deliver_iret() — if @regs returns to Ring 3 and a pending unblocked
 * signal exists, build a handler frame on the user stack and rewrite @regs to
 * enter the handler.  Called at the IRQ/exception return boundary with
 * interrupts disabled.  Returns 1 if a signal was set up, 0 otherwise.
 */
int  signal_deliver_iret(struct registers *regs);

/* Does the current thread have a deliverable (pending & ~masked) signal? */
int  signal_pending_current(void);

/* Timer hook: fire SIGALRM on any thread whose alarm deadline has elapsed.
 * @now is the current PIT tick count.  Safe to call from IRQ context. */
void signal_tick(uint64_t now);

/* alarm(2): arm SIGALRM after @seconds (0 cancels).  Returns the remaining
 * seconds of any previous alarm. */
unsigned do_alarm(unsigned seconds);

/* Syscall implementations (called from syscall_dispatch). */
int64_t do_sigaction(int signo, const struct sigaction *act, struct sigaction *old);
int64_t do_sigprocmask(int how, const sigset_t *set, sigset_t *old);
int64_t do_sigpending(sigset_t *out);
int64_t do_sigreturn(struct registers *regs);

/* pause(2): block until any signal is delivered; always returns -EINTR. */
int64_t do_pause(void);
/* sigsuspend(2): atomically install @mask, wait for a signal, restore mask. */
int64_t do_sigsuspend(const sigset_t *mask);

/* True if the current thread has a deliverable signal (used to interrupt the
 * yield/poll blocking loops in syscall handlers with -EINTR). */
int  signal_interrupted(void);

#endif /* AURALITE_KERNEL_PROC_SIGNAL_H */
