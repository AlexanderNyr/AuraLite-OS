/* userspace/tests/errnotest/errnotest.c — FIX_R3 gate: per-thread errno.
 *
 * One errno per thread (FIXES_PLAN.md R3).  Two threads each provoke a
 * DIFFERENT errno in a loop and, after deliberately giving the other thread
 * time to run, re-read errno WITHOUT making any new syscall.  POSIX allows
 * errno to change only due to a call made by THIS thread, so the re-read
 * must yield the same value it had right after the provoking call.  With
 * the pre-R3 single global cell, the other thread's syscalls overwrite it
 * within a few iterations — the test FAILS pre-fix, which is the point
 * (defect D2), and passes once errno is a TLS cell.
 *
 * Also verified:
 *   - the per-thread errno CELLS are distinct addresses (the core TLS
 *     property: __errno_location() must not return one shared cell);
 *   - the main thread's own cell works after the new TLS install path
 *     (single-threaded programs must not regress, R3 task 3);
 *   - plain sequential single-threaded errno semantics (provoke, read
 *     back) both for ENOENT and EBADF.
 *
 * Verdict line for the integration gate:
 *   "ERRNOTEST PASS per-thread isolation ..."  or  "ERRNOTEST FAIL ..."
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define ITERS 500

static volatile int a_done = 0, b_done = 0;
static long a_foreign = 0, b_foreign = 0;   /* observed the OTHER thread's errno */
static long a_miss = 0, b_miss = 0;         /* errno was neither mine nor foreign */
static int *addr_a = 0, *addr_b = 0, *addr_main = 0;

static void tiny_sleep(void) {
    struct timespec ts;
    ts.tv_sec = 0; ts.tv_nsec = 1000000L;   /* 1 ms: a few full time-slices */
    nanosleep(&ts, 0);
}

static void *thread_a(void *arg) {
    (void)arg;
    addr_a = __errno_location();
    for (int i = 0; i < ITERS; i++) {
        /* Provoke MY errno: opening a nonexistent path must give ENOENT. */
        errno = 0;
        int fd = open("/definitely/not/there", O_RDONLY);
        if (fd != -1) { a_miss++; if (fd >= 0) close(fd); continue; }
        if (errno != ENOENT) { a_miss++; continue; }
        /* Let thread B run.  It will execute failing syscalls of its own. */
        tiny_sleep();
        /* Re-read with NO intervening syscall on this thread. */
        int now = errno;
        if (now == EBADF)      a_foreign++;     /* thread B's errno leaked in */
        else if (now != ENOENT) a_miss++;
    }
    a_done = 1;
    return 0;
}

static void *thread_b(void *arg) {
    (void)arg;
    addr_b = __errno_location();
    char c;
    for (int i = 0; i < ITERS; i++) {
        /* Provoke MY errno: reading a bad descriptor must give EBADF. */
        errno = 0;
        long r = read(-12345, &c, 1);
        if (r != -1 || errno != EBADF) { b_miss++; continue; }
        tiny_sleep();
        int now = errno;
        if (now == ENOENT)      b_foreign++;    /* thread A's errno leaked in */
        else if (now != EBADF)  b_miss++;
    }
    b_done = 1;
    return 0;
}

int main(void) {
    int fails = 0;

    /* Single-threaded contract first (also exercises the main thread's
     * TLS-backed cell after the R3 install path). */
    errno = 0;
    int fd = open("/nonexistent", O_RDONLY);
    if (fd == -1 && errno == ENOENT) {
        printf("ERRNOTEST PASS single-thread open(missing)=ENOENT\n");
    } else {
        printf("ERRNOTEST FAIL single-thread open(missing): fd=%d errno=%d\n",
               fd, errno);
        fails++;
    }

    errno = 0;
    char c;
    long r = read(-1, &c, 1);
    if (r == -1 && errno == EBADF) {
        printf("ERRNOTEST PASS single-thread read(badfd)=EBADF\n");
    } else {
        printf("ERRNOTEST FAIL single-thread read(badfd): r=%ld errno=%d\n",
               r, errno);
        fails++;
    }

    addr_main = __errno_location();

    pthread_t ta, tb;
    if (pthread_create(&ta, 0, thread_a, 0) != 0) {
        printf("ERRNOTEST FAIL pthread_create A\n");
        return 1;
    }
    if (pthread_create(&tb, 0, thread_b, 0) != 0) {
        printf("ERRNOTEST FAIL pthread_create B\n");
        return 1;
    }
    while (!a_done || !b_done) tiny_sleep();

    int distinct = (addr_a && addr_b &&
                    addr_a != addr_b &&
                    addr_a != addr_main && addr_b != addr_main);
    printf("ERRNOTEST cells main=%p A=%p B=%p distinct=%d\n",
           (void *)addr_main, (void *)addr_a, (void *)addr_b, distinct);
    printf("ERRNOTEST A: foreign-observed=%ld other-mismatch=%ld of %d\n",
           a_foreign, a_miss, ITERS);
    printf("ERRNOTEST B: foreign-observed=%ld other-mismatch=%ld of %d\n",
           b_foreign, b_miss, ITERS);

    long cross = a_foreign + b_foreign;
    long miss  = a_miss + b_miss;
    if (cross == 0 && miss == 0 && distinct && fails == 0) {
        printf("ERRNOTEST PASS per-thread isolation (%d iterations x2)\n",
               ITERS);
        return 0;
    }
    printf("ERRNOTEST FAIL cross-contamination: foreign=%ld mismatch=%ld "
           "distinct=%d singlethread-fails=%d\n", cross, miss, distinct,
           fails);
    return 1;
}
