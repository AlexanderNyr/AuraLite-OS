/* fdshare.c -- M5 (MATURITY_PLAN.md) shared-open-file-description gate.
 *
 * POSIX fork() and dup() share the underlying open file description (OFD), so
 * the seek offset is common to every fd that refers to it.  Until M5 this was
 * only verified for same-process dup(); the fork case was deferred.  This test
 * exercises the textbook scenario:
 *
 *   parent opens a 12-byte file, reads 4 bytes (offset -> 4);
 *   fork();
 *   child reads 4 bytes and MUST see bytes [4..8) (shared offset), not [0..4);
 *   parent's dup(fd) shares the same OFD, so it reads bytes [8..12).
 *
 * It also sanity-checks close_range() (new in M5).  Prints "FDSHARE PASS".
 */

#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"

static int fails = 0;

int main(void) {
    /* 1) Create a 12-byte file with distinct bytes. */
    int wfd = open("/tmp/fdshare", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) { printf("FDSHARE FAIL: create /tmp/fdshare\n"); return 1; }
    if (write(wfd, "0123456789AB", 12) != 12) {
        printf("FDSHARE FAIL: write 12 bytes\n"); return 1;
    }
    close(wfd);

    int fd = open("/tmp/fdshare", O_RDONLY);
    if (fd < 0) { printf("FDSHARE FAIL: reopen\n"); return 1; }

    /* 2) Parent reads the first 4 bytes; the shared offset advances to 4. */
    char p[5] = {0};
    if (read(fd, p, 4) != 4 || memcmp(p, "0123", 4) != 0) {
        printf("FDSHARE FAIL: parent initial read '%.4s'\n", p); return 1;
    }

    /* 3) fork(): the child inherits the SAME OFD, so its read continues at
     *    offset 4 and must return "4567".  If fork gave the child a private
     *    offset it would re-read "0123". */
    pid_t pid = fork();
    if (pid < 0) { printf("FDSHARE FAIL: fork\n"); return 1; }
    if (pid == 0) {
        char c[5] = {0};
        int n = read(fd, c, 4);
        if (n != 4 || memcmp(c, "4567", 4) != 0) {
            printf("FDSHARE FAIL: child got '%.4s' (offset NOT shared)\n", c);
            _exit(1);
        }
        _exit(0);
    }
    int status = 1;
    waitpid(pid, &status, 0);
    int fork_shared = (status == 0);
    printf("[fdshare] fork shared-offset: %s\n", fork_shared ? "ok" : "FAIL");
    if (!fork_shared) fails++;

    /* 4) dup(): shares the same OFD.  The child's read advanced the shared
     *    offset to 8, so the dup'd fd reads "89AB". */
    int fd2 = dup(fd);
    char d[5] = {0};
    int dn = read(fd2, d, 4);
    int dup_shared = (dn == 4 && memcmp(d, "89AB", 4) == 0);
    printf("[fdshare] dup shared-offset: %s (read=%d '%.4s')\n",
           dup_shared ? "ok" : "FAIL", dn, d);
    if (!dup_shared) fails++;

    close(fd);
    close(fd2);

    /* 5) close_range(): open two fds, close them as a range, both must be bad. */
    int fa = open("/tmp/fdshare", O_RDONLY);
    int fb = open("/tmp/fdshare", O_RDONLY);
    if (fa < 0 || fb < 0) { printf("FDSHARE FAIL: reopen for close_range\n"); return 1; }
    int cr = close_range((unsigned)fa, (unsigned)fb, 0);
    char probe[1];
    int after_a = read(fa, probe, 1);   /* EBADF -> -1 */
    int after_b = read(fb, probe, 1);
    int cr_ok = (cr == 0 && after_a < 0 && after_b < 0);
    printf("[fdshare] close_range: %s (rc=%d a=%d b=%d)\n",
           cr_ok ? "ok" : "FAIL", cr, after_a, after_b);
    if (!cr_ok) fails++;

    if (fails == 0) { printf("FDSHARE PASS\n"); return 0; }
    printf("FDSHARE FAIL\n");
    return 1;
}
