/* stoptest — FIX_R6 gate: kernel THREAD_STOPPED job-control semantics.
 *
 * Two modes:
 *
 *   stoptest            — the deterministic, self-checking gate.  A child
 *                         streams dots into a pipe; the parent SIGSTOPs it
 *                         and asserts the kernel's side of the contract
 *                         (see the six CHECKs inline).  Every verdict is
 *                         computed in-program.
 *
 *   stoptest tick [n]   — an interactive helper for the shell job-control
 *                         gate (integration case test_stopped.sh): print n
 *                         ticks, one per second, so Ctrl+Z has a live
 *                         foreground program to suspend and `fg` has
 *                         something to resume ("continues from where it
 *                         was" = the tick numbering survives the stop).
 *                         A forked stdin pump (same process group) consumes
 *                         the console's control bytes so the kernel's ISIG
 *                         handling can stop the group while the ticker
 *                         sleeps; it is reaped before exit.
 *
 * The kernel side under test: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU default to
 * stopping the thread (THREAD_STOPPED) instead of terminating it;
 * SIGCONT and SIGKILL wake a stopped thread from the SENDER side;
 * waitpid(WUNTRACED) reports each stop once with WIFSTOPPED/WSTOPSIG;
 * a stopped thread consumes no CPU (its stream freezes).
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "time.h"
#include "signal.h"
#include "sys/wait.h"
#include "sys/select.h"

static int failures = 0;
#define CHECK(cond, name) do {                                  \
    if (cond) printf("STOPTEST PASS %s\n", name);               \
    else      { printf("STOPTEST FAIL %s\n", name); failures++; } \
} while (0)

static int tick_mode(int argc, char **argv) {
    int n = 5;
    if (argc > 2) {
        n = atoi(argv[2]);
        if (n <= 0) n = 5;
    }
    /* The console has no IRQ-side line discipline: a ^Z/^C byte typed on the
     * serial console is interpreted INSIDE the fd-0 read() syscall (ISIG ->
     * the foreground process group).  A ticker that only sleeps therefore
     * never becomes readable input's consumer and Ctrl+Z could never reach
     * it.  Fork a stdin pump in the same process group: its blocking read(0)
     * is what hands the control byte to the kernel, so the whole foreground
     * group (pump AND ticker) receives SIGTSTP/SIGINT and stops/resumes/dies
     * together -- which is exactly the interactive job-control contract
     * test_stopped.sh asserts.  The pump never prints anything. */
    pid_t pump = fork();
    if (pump == 0) {
        char b;
        for (;;) {
            if (read(0, &b, 1) != 1) sleep(1);   /* EINTR after a resume: re-read */
        }
    }
    for (int i = 1; i <= n; i++) {
        printf("STOPTEST tick %d of %d\n", i, n);
        fflush(stdout);
        sleep(1);
    }
    /* Reap the pump before exiting so it cannot outlive the ticker and eat
     * the shell's next input line. */
    if (pump > 0) {
        int st = 0;
        kill(pump, SIGKILL);
        waitpid(pump, &st, 0);
    }
    printf("STOPTEST tick done\n");
    fflush(stdout);
    return 0;
}

/* Read everything currently queued on fd without ever blocking. */
static int drain(int fd) {
    char buf[64];
    int  total = 0;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 0;
        if (select(fd + 1, &rf, 0, 0, &tv) <= 0) break;
        int64_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        total += (int)n;
    }
    return total;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "tick") == 0) {
        return tick_mode(argc, argv);
    }

    int p[2];
    if (pipe(p) != 0) { printf("STOPTEST FAIL pipe\n"); return 1; }

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        for (;;) {
            (void)write(p[1], ".", 1);
            usleep(10000);          /* ~100 dots/s: never fills the pipe */
        }
        _exit(0);
    }
    close(p[1]);

    usleep(500000);                 /* let the child build a steady stream */
    int before = drain(p[0]);
    CHECK(before > 0, "child stream flows before the stop");

    /* (1) SIGSTOP stops it and waitpid(WUNTRACED) reports the stop with
     *     the right signal in WSTOPSIG. */
    kill(pid, SIGSTOP);
    int   st = 0;
    pid_t w  = waitpid(pid, &st, WUNTRACED);
    CHECK(w == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP,
          "waitpid(WUNTRACED) reports the stop");

    /* (2) Each stop is reported exactly once. */
    st = 0;
    CHECK(waitpid(pid, &st, WUNTRACED | WNOHANG) == 0,
          "the same stop is not reported twice");

    /* (3) A stopped thread consumes no CPU: with the pipe drained, its
     *     output stream stays frozen for two whole seconds. */
    (void)drain(p[0]);
    sleep(2);
    CHECK(drain(p[0]) == 0, "no output (= no CPU) while stopped");

    /* (4) SIGCONT resumes it (sender-side wake of a stopped thread). */
    kill(pid, SIGCONT);
    sleep(1);
    CHECK(drain(p[0]) > 0, "SIGCONT resumes the stopped child");

    /* (5) A fresh stop is reported again (stop_notified resets). */
    kill(pid, SIGTSTP);
    st = 0;
    w  = waitpid(pid, &st, WUNTRACED);
    CHECK(w == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP,
          "a second stop (SIGTSTP) is reported again");

    /* (6) SIGKILL terminates it even though it is stopped. */
    kill(pid, SIGKILL);
    st = 0;
    w  = waitpid(pid, &st, 0);
    CHECK(w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "SIGKILL kills a stopped child");

    close(p[0]);
    printf(failures ? "STOPTEST SOME FAILURES\n" : "STOPTEST ALL PASS\n");
    fflush(stdout);
    return failures ? 1 : 0;
}
