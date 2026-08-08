#ifndef AURALITE_LIBC_SIGNAL_H
#define AURALITE_LIBC_SIGNAL_H

/*
 * signal.h — POSIX.1-2017 signals for AuraLite user programs.
 *
 * The `struct sigaction` layout MUST match the kernel's
 * (kernel/proc/signal.h).  Handlers are registered with a libc-supplied
 * sa_restorer trampoline (__sigreturn) that the kernel pushes as the handler's
 * return address.
 */

#include <stdint.h>

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define NSIG     32

#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTART   0x10000000
#define SA_SIGINFO   0x00000004

/* si_code values for siginfo_t (M5 SA_SIGINFO).  Must match the kernel. */
#define SI_USER      0      /* sent via kill/raise */
#define SI_KERNEL    0x80   /* sent by the kernel */
#define ILL_ILLOPC   1      /* illegal opcode */
#define FPE_INTDIV   1      /* integer divide by zero */
#define SEGV_MAPERR  1      /* address not mapped to object */
#define SEGV_ACCERR  2      /* invalid permissions for mapped object */
#define BUS_ADRALN   1      /* invalid address alignment */
#define TRAP_BRKPT   1      /* process breakpoint */

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef uint32_t sigset_t;

/* siginfo_t — passed to an SA_SIGINFO handler as the 2nd argument (M5).
 * Layout MUST match kernel/proc/signal.h.  si_addr is the faulting address
 * for SEGV/BUS/FPE/ILL; si_pid/si_uid identify the sender for SI_USER. */
typedef struct {
    int      si_signo;
    int      si_errno;
    int      si_code;
    int      _pad;
    /* Anonymous union (C11): si_addr / si_pid / si_uid are all accessible
     * directly (siginfo.si_addr) without accessor macros.  Layout MUST match
     * kernel/proc/signal.h. */
    union {
        void *si_addr;
        struct { int si_pid; uint32_t si_uid; };
    };
} siginfo_t;

/* ucontext_t — 3rd argument to an SA_SIGINFO handler (M5).  uc_mcontext
 * carries the interrupted register set; uc_sigmask is restored on sigreturn.
 * Layout MUST match kernel/proc/signal.h. */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx;
    uint64_t rip, rflags, rsp;
    uint64_t cs, ss;
} mcontext_t;

typedef struct ucontext ucontext_t;
struct ucontext {
    uint64_t      uc_flags;
    ucontext_t   *uc_link;
    mcontext_t    uc_mcontext;
    sigset_t      uc_sigmask;
};

/* sa_handler and sa_sigaction share one slot (a union), exactly as POSIX
 * requires; the bytes are identical to the kernel's single sa_handler field,
 * so the ABI is unchanged.  Set sa_sigaction + SA_SIGINFO for a 3-arg handler. */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    uint32_t  sa_mask;
    int       sa_flags;
    void    (*sa_restorer)(void);
};

/* sigset_t manipulation (POSIX). */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signo);
int sigdelset(sigset_t *set, int signo);
int sigismember(const sigset_t *set, int signo);

/* Signal management. */
void (*signal(int signo, void (*handler)(int)))(int);
int  sigaction(int signo, const struct sigaction *act, struct sigaction *old);
int  kill(int64_t pid, int signo);
int  raise(int signo);
int  sigprocmask(int how, const sigset_t *set, sigset_t *old);
int  sigpending(sigset_t *set);
int  sigsuspend(const sigset_t *mask);
unsigned alarm(unsigned seconds);
int  pause(void);

/* Q16: sig2str/str2sig — signal names (POSIX.1-2024 Issue 8). */
#define SIG2STR_MAX 32

int sig2str(int signum, char *str);
int str2sig(const char *restrict str, int *restrict signum);

/* POSIX.1 realtime signal extension for mqueue notification. */
union sigval {
    int    sival_int;
    void  *sival_ptr;
};

struct sigevent {
    int             sigev_notify;
    int             sigev_signo;
    union sigval    sigev_value;
    void          (*sigev_notify_function)(union sigval);
    void          *sigev_notify_attributes;
};

#define SIGEV_NONE     0
#define SIGEV_SIGNAL   1
#define SIGEV_THREAD   2

#endif /* AURALITE_LIBC_SIGNAL_H */
