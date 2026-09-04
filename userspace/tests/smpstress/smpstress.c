/* smpstress.c — the RESIDUE2 T1 SMP-sweep stress gate.
 *
 * Run under -smp 4 (tests/integration/cases/test_smp_procstress.sh).  Three
 * storms hammer exactly the structures the T1 sweep made safe:
 *
 *   1. O_APPEND concurrency: six children append fixed 32-byte records to
 *      ONE file through six independent OFDs.  Before the per-vnode append
 *      lock, two CPUs could read the same EOF and overwrite each other's
 *      records — the file came up short or with torn records.  The parent
 *      verifies both the size and every record's payload.
 *   2. fork/waitpid churn: 100 fork+exit cycles through the parent/child
 *      process table (link at fork, waitpid match, reap unlink) with
 *      precise exit codes.
 *   3. kill/sig_pending storm: children count SIGUSR1 deliveries while the
 *      parent raises them from another CPU (atomic pending bits).
 *
 * Prints "SMPSTRESS PASS" and exits 0, or names the failure and exits 1.
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "fcntl.h"
#include "errno.h"
#include "signal.h"
#include "time.h"      /* usleep */
#include "sys/wait.h"

#define N_APPENDERS  6
#define N_RECORDS    200
#define REC_LEN      32
#define N_FORKS      100
#define N_SIGNALS    10

static volatile int sig_count;
static void usr1_handler(int signo) {
    (void)signo;
    sig_count++;
}

int main(void) {
    int fails = 0;

    /* ---- 1. O_APPEND storm ---------------------------------------- */
    unlink("/tmp/smpapp.bin");
    /* The parent creates the file; the appenders must open an EXISTING
     * file (O_APPEND without O_CREAT) so the storm measures append
     * concurrency, not create races. */
    {
        int fd0 = open("/tmp/smpapp.bin", O_WRONLY | O_CREAT, 0644);
        if (fd0 < 0) {
            printf("SMPSTRESS FAIL: cannot create /tmp/smpapp.bin\n");
            return 1;
        }
        close(fd0);
    }
    for (int id = 0; id < N_APPENDERS; id++) {
        pid_t c = fork();
        if (c == 0) {
            int fd = open("/tmp/smpapp.bin", O_WRONLY | O_APPEND, 0644);
            if (fd < 0) exit(2);
            for (int seq = 0; seq < N_RECORDS; seq++) {
                unsigned char rec[REC_LEN];
                memset(rec, 0, sizeof(rec));
                rec[0]  = (unsigned char)(0xA0 | id);
                rec[1]  = (unsigned char)seq;
                rec[REC_LEN - 1] = (unsigned char)((id * 31 + seq) & 0xff);
                if (write(fd, rec, REC_LEN) != REC_LEN) exit(3);
            }
            close(fd);
            exit(0);
        }
    }
    /* Reap all six; any non-zero exit is a failure. */
    for (int id = 0; id < N_APPENDERS; id++) {
        int st = 0;
        pid_t w = waitpid(-1, &st, 0);
        if (w <= 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("SMPSTRESS FAIL: appender pid %lld status %d\n",
                   (long long)w, st);
            fails++;
        }
    }
    {
        int fd = open("/tmp/smpapp.bin", O_RDONLY, 0);
        if (fd < 0) {
            printf("SMPSTRESS FAIL: append file missing\n");
            fails++;
        } else {
            static unsigned char buf[N_APPENDERS * N_RECORDS * REC_LEN];
            long n = read(fd, buf, sizeof(buf));
            close(fd);
            long expect = (long)N_APPENDERS * N_RECORDS * REC_LEN;
            if (n != expect) {
                printf("SMPSTRESS FAIL: append size %ld != %ld "
                       "(records overwrote each other)\n", n, expect);
                fails++;
            } else {
                int seen[N_APPENDERS];
                memset(seen, 0, sizeof(seen));
                long bad = 0;
                for (long off = 0; off < n; off += REC_LEN) {
                    unsigned char *r = buf + off;
                    int id = r[0] & 0x0f;
                    if ((r[0] & 0xf0) != 0xa0 || id >= N_APPENDERS ||
                        r[REC_LEN - 1] !=
                            (unsigned char)((id * 31 + r[1]) & 0xff)) {
                        bad++;
                        continue;
                    }
                    seen[id]++;
                }
                for (int id = 0; id < N_APPENDERS; id++) {
                    if (seen[id] != N_RECORDS) {
                        printf("SMPSTRESS FAIL: appender %d wrote %d/%d "
                               "intact records (%ld torn)\n",
                               id, seen[id], N_RECORDS, bad);
                        fails++;
                    }
                }
                if (fails == 0) printf("SMPSTRESS: O_APPEND %d x %d x %d B "
                                       "intact\n", N_APPENDERS, N_RECORDS,
                                       REC_LEN);
            }
        }
    }

    /* ---- 2. fork/waitpid churn ------------------------------------ */
    for (int i = 0; i < N_FORKS; i++) {
        pid_t c = fork();
        if (c == 0) exit(20 + (i & 0x1f));
        int st = 0;
        pid_t w = waitpid(c, &st, 0);
        if (w != c || !WIFEXITED(st) || WEXITSTATUS(st) != 20 + (i & 0x1f)) {
            printf("SMPSTRESS FAIL: fork cycle %d -> pid %lld st %d\n",
                   i, (long long)w, st);
            fails++;
            break;
        }
    }
    if (fails == 0) printf("SMPSTRESS: %d fork/wait cycles precise\n", N_FORKS);

    /* ---- 3. sig_pending storm -------------------------------------- */
    for (int i = 0; i < 8 && fails == 0; i++) {
        pid_t c = fork();
        if (c == 0) {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = usr1_handler;
            sigaction(SIGUSR1, &sa, NULL);
            sig_count = 0;
            /* Wait in usleep, not a user-mode spin: a compute-bound child
             * only drains pending signals at timer-tick boundaries (~10 ms
             * at 99 Hz), so kills paced faster than one tick COALESCE (the
             * pending bit is already set -- correct standard-signal
             * behaviour, fatal to a delivery count).  2 ms sleeps let each
             * tick deliver exactly one pending SIGUSR1; the guard bounds a
             * wedged run (lost signal) to a 5 s exit(4). */
            int guard;
            for (guard = 0; sig_count < N_SIGNALS && guard < 2500; guard++)
                usleep(2000);
            exit(sig_count == N_SIGNALS ? 0 : 4);
        }
        for (int k = 0; k < N_SIGNALS; k++) {
            /* > one timer tick between kills: each kill must find the
             * pending bit CLEAR (drained by the handler on the child's
             * CPU) or the delivery legitimately coalesces. */
            usleep(12000);
            kill(c, SIGUSR1);
        }
        int st = 0;
        pid_t w = waitpid(c, &st, 0);
        if (w != c || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("SMPSTRESS FAIL: signal child %d status %d\n", i, st);
            fails++;
        }
    }
    if (fails == 0) printf("SMPSTRESS: %d x %d signal deliveries counted\n",
                           8, N_SIGNALS);

    if (fails == 0) {
        printf("SMPSTRESS PASS\n");
        return 0;
    }
    printf("SMPSTRESS %d FAILURE(S)\n", fails);
    return 1;
}
