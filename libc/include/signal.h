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
#define SIGWINCH 28
#define NSIG     32

#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTART   0x10000000
#define SA_SIGINFO   0x00000004

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef uint32_t sigset_t;

struct sigaction {
    void    (*sa_handler)(int);
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

#endif /* AURALITE_LIBC_SIGNAL_H */
