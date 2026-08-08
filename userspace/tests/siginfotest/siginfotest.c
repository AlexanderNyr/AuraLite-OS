/* siginfotest.c — M5 (MATURITY_PLAN.md) SA_SIGINFO regression test.
 *
 * Until M5, an SA_SIGINFO handler received si_addr/si_code/si_pid as zero
 * (the kernel set rsi=rdx=0, "P4 follow-up").  This program installs a
 * three-arg handler and verifies the kernel now passes a real siginfo_t:
 *
 *   Phase 1 (SI_USER): kill(getpid(), SIGUSR1) -> the handler checks
 *     si_signo == SIGUSR1, si_code == SI_USER, si_pid == getpid().
 *
 *   Phase 2 (synchronous fault): dereference an unmapped address -> #PF ->
 *     SIGSEGV with si_code == SEGV_MAPERR and si_addr == the faulting
 *     address; the handler prints SIGINFO PASS and _exit()s (returning would
 *     re-run the faulting instruction forever).
 *
 * Prints "SIGINFO PASS" on success.
 */

#include "stdio.h"
#include "unistd.h"
#include "signal.h"
#include "string.h"

static volatile int g_signo, g_code, g_pid_ok;

static void usr_handler(int signo, siginfo_t *info, void *uc) {
    (void)signo; (void)uc;
    g_signo  = info->si_signo;
    g_code   = info->si_code;
    g_pid_ok = (info->si_pid == getpid());
}

static void segv_handler(int signo, siginfo_t *info, void *uc) {
    (void)signo; (void)uc;
    /* We dereferenced 0x1 (page 0 is unmapped): SEGV_MAPERR, si_addr == 0x1.
     * libc printf has no %p here, so report the raw bits on a mismatch. */
    if (info->si_signo == SIGSEGV && info->si_code == SEGV_MAPERR &&
        (unsigned long)info->si_addr == 0x1UL) {
        printf("SIGINFO PASS\n");
    } else {
        unsigned long a = (unsigned long)info->si_addr;
        printf("SIGINFO FAIL: segv signo=%d code=%d addr=0x%lx\n",
               info->si_signo, info->si_code, a);
    }
    fflush(stdout);
    _exit(0);   /* do not return: the faulting instruction would re-run */
}

int main(void) {
    struct sigaction sa;

    /* ---- Phase 1: SI_USER via self-kill ---- */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = usr_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("SIGINFO FAIL: sigaction SIGUSR1\n");
        return 1;
    }
    kill(getpid(), SIGUSR1);

    printf("[siginfo] usr1 signo=%d code=%d pid_ok=%d\n",
           (int)g_signo, (int)g_code, (int)g_pid_ok);
    fflush(stdout);
    if (!(g_signo == SIGUSR1 && g_code == SI_USER && g_pid_ok)) {
        printf("SIGINFO FAIL: USR1 siginfo wrong\n");
        return 1;
    }

    /* ---- Phase 2: synchronous SIGSEGV from a page fault ---- */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) < 0) {
        printf("SIGINFO FAIL: sigaction SIGSEGV\n");
        return 1;
    }
    printf("[siginfo] dereferencing 0x1 (expect handler -> SIGINFO PASS)\n");
    fflush(stdout);
    /* Hide the faulting address behind a volatile so the compiler's
     * -Warray-bounds cannot fold the 0x1 and warn/error on the literal. */
    volatile uintptr_t bad_addr = 0x1UL;
    volatile int *p = (volatile int *)bad_addr;
    *p = 42;                 /* #PF -> SIGSEGV -> handler -> _exit */

    printf("SIGINFO FAIL: deref returned without SIGSEGV\n");
    return 1;
}
