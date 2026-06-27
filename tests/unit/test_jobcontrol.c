/*
 * test_jobcontrol.c — host-side unit test for P6 wait-status macros, the
 * kernel status encoding, and the waitpid group-matching selector logic.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <stdint.h>
#include "libc/include/sys/wait.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* Mirror of kernel wait_status_of(). */
static int status_exit(int code) { return (code & 0xff) << 8; }
static int status_sig(int sig)   { return sig & 0x7f; }

/* Mirror of kernel wait_zombie_matches(). */
static int matches(int64_t zid, int64_t zpgid, int64_t pid, int64_t ppgid) {
    if (pid > 0)  return zid == pid;
    if (pid == 0) return zpgid == ppgid;
    if (pid == -1) return 1;
    return zpgid == -pid;
}

int main(void) {
    CK(WNOHANG == 1 && WUNTRACED == 2);

    /* Exit-status encoding. */
    int s = status_exit(42);
    CK(WIFEXITED(s) && !WIFSIGNALED(s) && WEXITSTATUS(s) == 42);
    s = status_exit(0);
    CK(WIFEXITED(s) && WEXITSTATUS(s) == 0);
    s = status_exit(255);
    CK(WIFEXITED(s) && WEXITSTATUS(s) == 255);

    /* Signal-death encoding. */
    s = status_sig(2);   /* SIGINT */
    CK(WIFSIGNALED(s) && !WIFEXITED(s) && WTERMSIG(s) == 2);
    s = status_sig(9);   /* SIGKILL */
    CK(WIFSIGNALED(s) && WTERMSIG(s) == 9);
    s = status_sig(11);  /* SIGSEGV */
    CK(WIFSIGNALED(s) && WTERMSIG(s) == 11);

    /* waitpid selector matching. */
    CK(matches(5, 3, 5, 9));        /* pid>0: exact pid */
    CK(!matches(6, 3, 5, 9));
    CK(matches(6, 9, 0, 9));        /* pid==0: caller's pgid */
    CK(!matches(6, 3, 0, 9));
    CK(matches(6, 3, -1, 9));       /* pid==-1: any */
    CK(matches(6, 7, -7, 9));       /* pid<-1: group |pid| */
    CK(!matches(6, 3, -7, 9));

    if (failures == 0) { printf("test_jobcontrol: ALL PASS\n"); return 0; }
    printf("test_jobcontrol: %d FAILURE(S)\n", failures);
    return 1;
}
