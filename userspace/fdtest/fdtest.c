/* fdtest.c — single-process FD lifecycle smoke test.
 *
 * Covers dup, dup2, fcntl(F_GETFD/F_SETFD/FD_CLOEXEC) and pipe.  Cross-
 * process FD isolation is asserted separately via:
 *   * the boot-time process_self_test (kernel-side spawn /hello starts with
 *     an empty FD table; if it didn't, /hello would crash).
 *   * test_fd_isolation.sh which runs us under the shell and asserts the
 *     kernel still has no FD leaks after our exit.
 */

#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

static int fails = 0;
static void say(const char *name, int ok) {
    if (ok) printf("FDTEST PASS: %s\n", name);
    else  { printf("FDTEST FAIL: %s\n", name); fails++; }
}

int main(void) {
    int fd_a = open("/hello", O_RDONLY);
    say("parent opens /hello", fd_a >= 3);

    int fd_b = open("/hello", O_RDONLY);
    say("parent opens /hello again", fd_b >= 3 && fd_b != fd_a);

    /* dup. */
    int nd = dup(fd_a);
    say("dup returns new fd >= 3", nd >= 3 && nd != fd_a && nd != fd_b);
    if (nd >= 3) close(nd);

    /* dup2 to a chosen number. */
    int target = (fd_b > fd_a ? fd_b : fd_a) + 4;
    int dr = dup2(fd_a, target);
    say("dup2 to higher fd", dr == target);
    if (dr == target) close(target);

    /* FD_CLOEXEC round-trip. */
    say("F_GETFD initial 0", fcntl(fd_a, F_GETFD, 0) == 0);
    say("F_SETFD CLOEXEC",   fcntl(fd_a, F_SETFD, FD_CLOEXEC) == 0);
    say("F_GETFD reads back CLOEXEC", fcntl(fd_a, F_GETFD, 0) == FD_CLOEXEC);

    close(fd_a);
    close(fd_b);

    /* pipe round-trip (the read may complete across multiple syscalls). */
    int pfds[2] = { -1, -1 };
    int pr = pipe(pfds);
    say("pipe creates two fds", pr == 0 && pfds[0] >= 3 && pfds[1] >= 3);
    if (pr == 0) {
        const char *msg = "ping!";
        say("pipe write 5 bytes", write(pfds[1], msg, 5) == 5);
        char rb[6] = {0};
        int got = 0;
        for (int attempt = 0; attempt < 4 && got < 5; attempt++) {
            ssize_t r = read(pfds[0], rb + got, 5 - got);
            if (r > 0) got += (int)r;
        }
        say("pipe read 5 bytes", got == 5 && memcmp(rb, "ping!", 5) == 0);
        close(pfds[0]); close(pfds[1]);
    }

    /* pipe rejects bad userspace out buffer. */
    int bad = pipe((int *)0xFFFF800000000000ULL);
    say("pipe rejects kernel out buffer", bad < 0);

    if (fails == 0) printf("FDTEST ALL PASS\n");
    else            printf("FDTEST %d FAILURES\n", fails);
    return fails;
}
