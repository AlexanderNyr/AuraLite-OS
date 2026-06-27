/*
 * signal.c — POSIX.1-2017 signal subsystem (P4 core).
 *
 * Delivery model: a signal becomes "pending" (a bit in tcb_t::sig_pending) when
 * sent.  It is delivered only at a return-to-user boundary with interrupts
 * disabled (IRQ/exception return, or the syscall-exit slow path), by building a
 * signal frame on the user stack and rewriting the outgoing register frame to
 * enter the handler.  The handler "returns" through a libc trampoline that
 * invokes SYS_SIGRETURN, which restores the saved frame and sigmask atomically.
 *
 * References: POSIX.1-2017 §2.4; x86-64 SysV psABI §3.2.2 (16-byte alignment,
 * 128-byte red zone); Linux arch/x86/kernel/signal_64.c (FIX_EFLAGS, CS/SS
 * pinning, forced-default for blocked synchronous faults).
 */

#include <stdint.h>
#include "kernel/proc/signal.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/usercopy.h"
#include "kernel/arch/x86_64/isr.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "drivers/timer/pit.h"

#define USER_CS 0x23
#define USER_SS 0x1B

/* PIT tick rate (Hz).  alarm() converts seconds<->ticks with this. */
#define PIT_HZ  PIT_DEFAULT_FREQUENCY

/* RFLAGS bits the user is permitted to influence via sigreturn (Linux
 * FIX_EFLAGS): CF PF AF ZF SF TF DF OF RF AC.  IF/IOPL/NT/reserved are taken
 * from the kernel baseline, never from the user frame. */
#define FLAG_CF  0x00000001u
#define FLAG_PF  0x00000004u
#define FLAG_AF  0x00000010u
#define FLAG_ZF  0x00000040u
#define FLAG_SF  0x00000080u
#define FLAG_TF  0x00000100u
#define FLAG_IF  0x00000200u
#define FLAG_DF  0x00000400u
#define FLAG_OF  0x00000800u
#define FLAG_RF  0x00010000u
#define FLAG_AC  0x00040000u
#define FLAG_RESERVED1 0x00000002u   /* always 1 */
#define FIX_EFLAGS (FLAG_CF|FLAG_PF|FLAG_AF|FLAG_ZF|FLAG_SF|FLAG_TF| \
                    FLAG_DF|FLAG_OF|FLAG_RF|FLAG_AC)

/* Default action classes. */
enum { DFL_TERM, DFL_IGN, DFL_STOP, DFL_CONT };

static int default_action(int signo) {
    switch (signo) {
    case SIGCHLD: case SIGWINCH: case SIGCONT:
        return DFL_IGN;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return DFL_STOP;
    default:
        return DFL_TERM;   /* HUP/INT/QUIT/ILL/ABRT/FPE/KILL/SEGV/PIPE/ALRM/TERM/... */
    }
}

/* Pick the lowest-numbered deliverable (pending & ~masked) signal, or 0. */
static int next_deliverable(tcb_t *t) {
    uint32_t deliverable = t->sig_pending & ~t->sig_mask;
    /* SIGKILL/SIGSTOP bypass the mask. */
    deliverable |= (t->sig_pending & SIG_UNCATCHABLE);
    if (!deliverable) return 0;
    for (int s = 1; s < NSIG; s++) {
        if (deliverable & sig_bit(s)) return s;
    }
    return 0;
}

void signal_send(tcb_t *target, int signo) {
    if (!target || signo < 1 || signo >= NSIG) return;
    target->sig_pending |= sig_bit(signo);
}

int signal_send_group(int64_t pgid, int signo) {
    tcb_t *list[64];
    int n = thread_get_all(list, 64);
    int delivered = 0;
    for (int i = 0; i < n; i++) {
        if (list[i]->pml4_phys == 0) continue;        /* skip kernel threads */
        if (list[i]->pgid != pgid) continue;
        if (signo != 0) signal_send(list[i], signo);
        delivered = 1;
    }
    return delivered ? 0 : -ESRCH;
}

int signal_kill(int64_t pid, int signo) {
    if (signo < 0 || signo >= NSIG) return -EINVAL;
    if (pid > 0) {
        tcb_t *t = thread_get_by_pid((uint64_t)pid);
        if (!t) return -ESRCH;
        if (signo != 0) signal_send(t, signo);   /* signo==0: existence check */
        return 0;
    }
    if (pid == 0) {
        /* Caller's own process group. */
        tcb_t *self = sched_current();
        return signal_send_group(self ? self->pgid : 0, signo);
    }
    if (pid < -1) {
        /* Process group |pid|. */
        return signal_send_group(-pid, signo);
    }
    /* pid == -1: broadcast to all user processes except the sender and init. */
    tcb_t *list[64];
    int n = thread_get_all(list, 64);
    tcb_t *self = sched_current();
    int delivered = 0;
    for (int i = 0; i < n; i++) {
        if (list[i] == self || list[i]->id == 1) continue;
        if (list[i]->pml4_phys == 0) continue;
        if (signo != 0) signal_send(list[i], signo);
        delivered = 1;
    }
    return delivered ? 0 : -ESRCH;
}

/* ---- process groups / sessions ---- */

int64_t do_setsid(void) {
    tcb_t *t = sched_current();
    if (!t) return -EINVAL;
    /* EPERM if the caller is already a process group leader (pgid == pid). */
    if (t->pgid == (int64_t)t->id) return -EPERM;
    t->sid  = (int64_t)t->id;
    t->pgid = (int64_t)t->id;
    t->is_session_leader = 1;
    t->ctty = 0;                  /* new session has no controlling terminal */
    return (int64_t)t->id;
}

int64_t do_setpgid(int64_t pid, int64_t pgid) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    if (pgid < 0) return -EINVAL;
    tcb_t *t = (pid == 0) ? self : thread_get_by_pid((uint64_t)pid);
    if (!t) return -ESRCH;
    /* May only change a process in the caller's own session. */
    if (t->sid != self->sid) return -EPERM;
    /* A session leader cannot change its process group. */
    if (t->is_session_leader) return -EPERM;
    t->pgid = (pgid == 0) ? (int64_t)t->id : pgid;
    return 0;
}

int64_t do_getpgid(int64_t pid) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    tcb_t *t = (pid == 0) ? self : thread_get_by_pid((uint64_t)pid);
    if (!t) return -ESRCH;
    return t->pgid;
}

int64_t do_getsid(int64_t pid) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    tcb_t *t = (pid == 0) ? self : thread_get_by_pid((uint64_t)pid);
    if (!t) return -ESRCH;
    return t->sid;
}

int signal_pending_current(void) {
    tcb_t *t = sched_current();
    return t ? (next_deliverable(t) != 0) : 0;
}

/* A blocking syscall loop should abort with -EINTR if a catchable, unblocked
 * signal is pending (a handler that will run on return, or a default-terminate).
 * Returns 1 if the current syscall should be interrupted. */
int signal_interrupted(void) {
    tcb_t *t = sched_current();
    if (!t) return 0;
    return next_deliverable(t) != 0;
}

void signal_tick(uint64_t now) {
    /* Fire SIGALRM on any thread whose armed deadline has elapsed. */
    tcb_t *list[64];
    int n = thread_get_all(list, 64);
    for (int i = 0; i < n; i++) {
        tcb_t *t = list[i];
        if (t->alarm_deadline != 0 && now >= t->alarm_deadline) {
            t->alarm_deadline = 0;
            signal_send(t, SIGALRM);
        }
    }
}

unsigned do_alarm(unsigned seconds) {
    tcb_t *t = sched_current();
    if (!t) return 0;
    uint64_t now = timer_get_ticks();
    unsigned remaining = 0;
    if (t->alarm_deadline != 0 && t->alarm_deadline > now) {
        remaining = (unsigned)((t->alarm_deadline - now + (PIT_HZ - 1)) / PIT_HZ);
    }
    if (seconds == 0) {
        t->alarm_deadline = 0;            /* cancel */
    } else {
        t->alarm_deadline = now + (uint64_t)seconds * PIT_HZ;
    }
    return remaining;
}

int64_t do_pause(void) {
    /* Block until any signal is delivered, then return -EINTR (POSIX pause()).
     * Interrupts are enabled while we yield so the timer/IRQ can post signals
     * and the eventual return-to-user boundary delivers them. */
    tcb_t *t = sched_current();
    if (!t) return -EINTR;
    while (!next_deliverable(t)) {
        __asm__ volatile ("sti" ::: "memory");
        sched_yield();
        __asm__ volatile ("pause");
    }
    return -EINTR;
}

int64_t do_sigsuspend(const sigset_t *mask) {
    tcb_t *t = sched_current();
    if (!t) return -EINTR;
    sigset_t newmask;
    if (copy_from_user(&newmask, mask, sizeof(newmask)) != 0) return -EFAULT;
    newmask &= ~SIG_UNCATCHABLE;
    sigset_t saved = t->sig_mask;
    /* Install the temporary mask and arm restore: the next handler frame built
     * for the woken signal will record @saved as its saved_mask, so sigreturn
     * restores the original mask after the handler runs (POSIX sigsuspend). */
    t->sig_mask = newmask;
    t->sig_suspend_active = 1;
    t->sig_suspend_restore = saved;
    while (!next_deliverable(t)) {
        __asm__ volatile ("sti" ::: "memory");
        sched_yield();
        __asm__ volatile ("pause");
    }
    /* If the woken signal is default-terminate/ignore (no frame built), or it
     * was consumed without a handler, restore the original mask here. */
    if (t->sig_suspend_active) {
        t->sig_mask = saved;
        t->sig_suspend_active = 0;
    }
    return -EINTR;
}

/* Terminate the current thread because of an uncaught/default-terminate signal.
 * Encodes the signal in the exit code (128 + signo), matching shell convention. */
static void terminate_by_signal(int signo) {
    kprintf("[signal] terminate pid=%llu by signal %d\n",
            (unsigned long long)(sched_current() ? sched_current()->id : 0), signo);
    thread_exit_with_signal(signo);   /* records WIFSIGNALED status */
}

/*
 * build_handler_frame() — common frame builder for both the IRET and syscall
 * delivery paths.  @regs is the outgoing Ring-3 register frame; on success it
 * is rewritten to enter the handler and the function returns 1.  On failure
 * (bad user stack) the process is terminated and the function does not return.
 */
static int build_handler_frame(tcb_t *t, int signo, struct registers *regs) {
    struct sigaction *sa = &t->sig_actions[signo];

    /* Subtract the 128-byte red zone, reserve the frame, 16-align, then leave
     * room for the 8-byte trampoline return address so the handler sees
     * RSP%16==8 at entry. */
    uint64_t sp = regs->rsp;
    sp -= 128;                                   /* red zone */
    sp -= sizeof(struct signal_frame);
    sp &= ~((uint64_t)15);                        /* 16-align the frame */
    uint64_t frame_addr = sp;
    sp -= 8;                                      /* trampoline return address slot */
    /* Now sp%16 == 8 (frame_addr was 16-aligned), as a `call` would leave it. */

    /* Validate the whole frame + return slot lies in user space and is writable. */
    if (!validate_user_range((void *)(uintptr_t)sp,
                             (uint64_t)(frame_addr - sp) + sizeof(struct signal_frame),
                             1)) {
        kprintf("[signal] bad user stack for signal %d (rsp=%llx); forcing SIGSEGV\n",
                signo, (unsigned long long)regs->rsp);
        terminate_by_signal(SIGSEGV);            /* does not return */
        return 0;
    }

    /* Snapshot the interrupted context into a kernel-local frame, then copy out. */
    struct signal_frame f;
    f.r15 = regs->r15; f.r14 = regs->r14; f.r13 = regs->r13; f.r12 = regs->r12;
    f.r11 = regs->r11; f.r10 = regs->r10; f.r9 = regs->r9;  f.r8  = regs->r8;
    f.rdi = regs->rdi; f.rsi = regs->rsi; f.rbp = regs->rbp; f.rdx = regs->rdx;
    f.rcx = regs->rcx; f.rbx = regs->rbx; f.rax = regs->rax;
    f.rip = regs->rip; f.rflags = regs->rflags; f.rsp = regs->rsp;
    f.cs = regs->cs; f.ss = regs->ss;
    /* During sigsuspend the frame must record the ORIGINAL mask so sigreturn
     * restores it after the handler (not the temporary suspend mask). */
    if (t->sig_suspend_active) {
        f.saved_mask = t->sig_suspend_restore;
        t->sig_suspend_active = 0;
    } else {
        f.saved_mask = t->sig_mask;
    }
    f.signo = (uint32_t)signo;

    if (copy_to_user((void *)(uintptr_t)frame_addr, &f, sizeof(f)) != 0) {
        terminate_by_signal(SIGSEGV);
        return 0;
    }
    /* Push the libc trampoline as the handler's return address.  When the
     * handler executes `ret`, it lands in __sigreturn, which issues
     * SYS_SIGRETURN with RSP pointing at the signal_frame just above. */
    uint64_t trampoline = (uint64_t)(uintptr_t)sa->sa_restorer;
    if (!trampoline || trampoline >= USER_VADDR_TOP) {
        terminate_by_signal(SIGSEGV);            /* no usable restorer */
        return 0;
    }
    if (copy_to_user((void *)(uintptr_t)sp, &trampoline, sizeof(trampoline)) != 0) {
        terminate_by_signal(SIGSEGV);
        return 0;
    }

    /* Compute the new blocked mask for the handler:
     *   new = old | sa_mask | {signo}   (the last term unless SA_NODEFER),
     * with SIGKILL/SIGSTOP never blockable. */
    uint32_t new_mask = t->sig_mask | sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER)) new_mask |= sig_bit(signo);
    new_mask &= ~SIG_UNCATCHABLE;
    t->sig_mask = new_mask;

    /* SA_RESETHAND / one-shot: reset disposition to default before running. */
    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
        sa->sa_flags &= ~SA_RESETHAND;
    }

    /* Clear the pending bit for the signal we are about to deliver. */
    t->sig_pending &= ~sig_bit(signo);

    /* Rewrite the outgoing frame to enter the handler (SysV: rdi = signo). */
    regs->rip = (uint64_t)(uintptr_t)sa->sa_handler;
    regs->rsp = sp;
    regs->rdi = (uint64_t)signo;
    regs->rsi = 0;                       /* &siginfo (SA_SIGINFO: P4 follow-up) */
    regs->rdx = 0;                       /* &ucontext */
    regs->rax = 0;                       /* varargs convention */
    /* Sanitize RFLAGS for handler entry: clear DF/RF/TF, force IF set. */
    regs->rflags = (regs->rflags & ~(uint64_t)(FLAG_DF | FLAG_RF | FLAG_TF)) | FLAG_IF;
    /* Pin Ring-3 selectors. */
    regs->cs = USER_CS;
    regs->ss = USER_SS;
    return 1;
}

int signal_deliver_iret(struct registers *regs) {
    /* Only deliver when returning to Ring 3. */
    if ((regs->cs & 3) != 3) return 0;
    tcb_t *t = sched_current();
    if (!t) return 0;
    int signo = next_deliverable(t);
    if (!signo) return 0;

    /* SIGKILL / default-terminate / stop without a catcher. */
    struct sigaction *sa = &t->sig_actions[signo];
    void (*h)(int) = sa->sa_handler;

    if (signo == SIGKILL || signo == SIGSTOP || h == SIG_DFL) {
        t->sig_pending &= ~sig_bit(signo);
        switch (default_action(signo)) {
        case DFL_IGN:  return 0;                 /* default-ignore: drop */
        case DFL_STOP: /* job control lands in P6; treat as terminate for now */
        case DFL_TERM:
        default:
            terminate_by_signal(signo);          /* no return */
            return 0;
        }
    }
    if (h == SIG_IGN) {
        t->sig_pending &= ~sig_bit(signo);       /* explicitly ignored */
        return 0;
    }
    /* Caught: build the handler frame. */
    return build_handler_frame(t, signo, regs);
}

int signal_raise_fault(struct registers *regs, int signo) {
    tcb_t *t = sched_current();
    if (!t) { terminate_by_signal(signo); return 0; }
    struct sigaction *sa = &t->sig_actions[signo];
    void (*h)(int) = sa->sa_handler;

    /* Synchronous fault: if blocked or ignored, force the default action
     * (POSIX permits this; otherwise the instruction re-faults forever). */
    int blocked = (t->sig_mask & sig_bit(signo)) != 0;
    if (h == SIG_DFL || h == SIG_IGN || blocked) {
        terminate_by_signal(signo);              /* no return */
        return 0;
    }
    t->sig_pending |= sig_bit(signo);
    return build_handler_frame(t, signo, regs);
}

/* ---- syscalls ---- */

int64_t do_sigaction(int signo, const struct sigaction *act, struct sigaction *old) {
    tcb_t *t = sched_current();
    if (!t) return -EINVAL;
    if (signo < 1 || signo >= NSIG) return -EINVAL;
    /* SIGKILL/SIGSTOP cannot be caught or ignored. */
    if (act && (signo == SIGKILL || signo == SIGSTOP)) return -EINVAL;

    struct sigaction kact, kold;
    if (act) {
        if (copy_from_user(&kact, act, sizeof(kact)) != 0) return -EFAULT;
    }
    kold = t->sig_actions[signo];
    if (act) {
        t->sig_actions[signo] = kact;
    }
    if (old) {
        if (copy_to_user(old, &kold, sizeof(kold)) != 0) return -EFAULT;
    }
    return 0;
}

int64_t do_sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    tcb_t *t = sched_current();
    if (!t) return -EINVAL;
    sigset_t cur = t->sig_mask;
    if (old) {
        if (copy_to_user(old, &cur, sizeof(cur)) != 0) return -EFAULT;
    }
    if (set) {
        sigset_t s;
        if (copy_from_user(&s, set, sizeof(s)) != 0) return -EFAULT;
        switch (how) {
        case SIG_BLOCK:   cur |= s;  break;
        case SIG_UNBLOCK: cur &= ~s; break;
        case SIG_SETMASK: cur = s;   break;
        default: return -EINVAL;
        }
        cur &= ~SIG_UNCATCHABLE;     /* SIGKILL/SIGSTOP can never be blocked */
        t->sig_mask = cur;
    }
    return 0;
}

int64_t do_sigpending(sigset_t *out) {
    tcb_t *t = sched_current();
    if (!t) return -EINVAL;
    /* POSIX: the set of pending-and-blocked signals. */
    sigset_t p = t->sig_pending & t->sig_mask;
    if (copy_to_user(out, &p, sizeof(p)) != 0) return -EFAULT;
    return 0;
}

int64_t do_sigreturn(struct registers *regs) {
    tcb_t *t = sched_current();
    if (!t) return -EINVAL;

    /* The frame sits just above the current user RSP (the trampoline popped the
     * return address, so RSP now points at the signal_frame). */
    uint64_t frame_addr = regs->rsp;
    if (!validate_user_range((void *)(uintptr_t)frame_addr,
                             sizeof(struct signal_frame), 0)) {
        terminate_by_signal(SIGSEGV);
        return 0;
    }
    struct signal_frame f;
    if (copy_from_user(&f, (void *)(uintptr_t)frame_addr, sizeof(f)) != 0) {
        terminate_by_signal(SIGSEGV);
        return 0;
    }

    /* Restore the blocked mask atomically with the register state. */
    t->sig_mask = f.saved_mask & ~SIG_UNCATCHABLE;

    /* Restore GPRs. */
    regs->r15 = f.r15; regs->r14 = f.r14; regs->r13 = f.r13; regs->r12 = f.r12;
    regs->r10 = f.r10; regs->r9 = f.r9;   regs->r8 = f.r8;
    regs->rdi = f.rdi; regs->rsi = f.rsi; regs->rbp = f.rbp; regs->rdx = f.rdx;
    regs->rbx = f.rbx; regs->rax = f.rax; regs->rcx = f.rcx; regs->r11 = f.r11;

    /* Validate RIP/RSP are canonical user addresses (security). */
    if (f.rip >= USER_VADDR_TOP || f.rsp >= USER_VADDR_TOP) {
        terminate_by_signal(SIGSEGV);
        return 0;
    }
    regs->rip = f.rip;
    regs->rsp = f.rsp;
    /* RFLAGS: whitelist user-settable bits; force IF set, IOPL/NT/reserved safe. */
    regs->rflags = (f.rflags & FIX_EFLAGS) | FLAG_IF | FLAG_RESERVED1;
    /* Pin Ring-3 selectors regardless of frame contents. */
    regs->cs = USER_CS;
    regs->ss = USER_SS;

    /* The dispatcher will route this return through the iret path; the return
     * value in RAX has already been restored to the interrupted RAX above. */
    return (int64_t)regs->rax;
}
