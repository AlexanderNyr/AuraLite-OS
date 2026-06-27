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

/* P6a model: wait_child_matches */
static int wait_child_matches_model(int64_t parent_pgid, int64_t child_id, int64_t child_pgid, int64_t pid, int is_child) {
    if (!is_child) return 0;
    if (pid > 0)  return child_id == pid;
    if (pid == -1) return 1;
    if (pid == 0)  return child_pgid == parent_pgid;
    return child_pgid == -pid;
}

/* Simplified do_waitpid model */
#define ECHILD 10
#define EINVAL 22
struct mock_child { int64_t id; int64_t pgid; int zombie; int waited; int exit_code; };
static int mock_waitpid(struct mock_child *children, int n, int64_t parent_pgid,
                        int64_t pid, int options, int *status, int64_t *out_pid)
{
    if (options & ~(WNOHANG | WUNTRACED)) return -EINVAL;
    /* find matching zombie */
    for (int i=0;i<n;i++) {
        if (!children[i].zombie || children[i].waited) continue;
        if (!wait_child_matches_model(parent_pgid, children[i].id, children[i].pgid, pid, 1)) continue;
        if (status) *status = (children[i].exit_code & 0xff) << 8;
        if (out_pid) *out_pid = children[i].id;
        children[i].waited = 1;
        return 0; /* success */
    }
    /* child existence scan, skip already-waited zombies */
    int have = 0;
    for (int i=0;i<n;i++) {
        if (children[i].zombie && children[i].waited) continue;
        if (wait_child_matches_model(parent_pgid, children[i].id, children[i].pgid, pid, 1)) { have=1; break; }
    }
    if (!have) return -ECHILD;
    if (options & WNOHANG) { if (out_pid) *out_pid = 0; return 0; }
    return 0; /* would block */
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

    /* P6a: do_waitpid model tests */
    {
        int status = 0;
        int64_t out = -1;
        /* no child -> -ECHILD */
        struct mock_child none[1] = {0};
        CK(mock_waitpid(none, 0, 100, -1, WNOHANG, &status, &out) == -ECHILD);
        /* nonmatching group with WNOHANG -> -ECHILD */
        struct mock_child c1[] = { { .id=5, .pgid=10, .zombie=0, .waited=0 } };
        CK(mock_waitpid(c1, 1, 100, -20, WNOHANG, &status, &out) == -ECHILD);
        /* matching running child with WNOHANG -> 0 */
        struct mock_child c2[] = { { .id=6, .pgid=100, .zombie=0, .waited=0 } };
        out = 99;
        CK(mock_waitpid(c2, 1, 100, 0, WNOHANG, &status, &out) == 0 && out == 0);
        /* matching zombie -> pid + WIFEXITED/WEXITSTATUS */
        struct mock_child c3[] = { { .id=7, .pgid=100, .zombie=1, .waited=0, .exit_code=42 } };
        status = 0; out = 0;
        CK(mock_waitpid(c3, 1, 100, 7, 0, &status, &out) == 0 && out == 7 && WIFEXITED(status) && WEXITSTATUS(status)==42);
        /* invalid options -> -EINVAL */
        struct mock_child c4[] = { { .id=8, .pgid=100, .zombie=0, .waited=0 } };
        CK(mock_waitpid(c4, 1, 100, -1, 0xFF, &status, &out) == -EINVAL);
        /* already-waited zombie does not count as child */
        struct mock_child c5[] = { { .id=9, .pgid=100, .zombie=1, .waited=1 } };
        CK(mock_waitpid(c5, 1, 100, 9, WNOHANG, &status, &out) == -ECHILD);
    }

    if (failures == 0) { printf("test_jobcontrol: ALL PASS\n"); return 0; }
    printf("test_jobcontrol: %d FAILURE(S)\n", failures);
    return 1;
}
