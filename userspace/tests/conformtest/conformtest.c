/* conformtest.c — POSIX.1-2024 conformance suite, guest layer.
 *
 * POSIX2024_PLAN.md phase Q12.  This program is installed in the initrd as
 * /tests/conformtest and driven by the QEMU integration case
 * (tests/integration/cases/test_posix2024_conf.sh).  It asserts syscall-
 * backed behaviour END TO END on the real kernel, closing the Q5 gate hole
 * (the AT-family was host-compiled but never exercised at runtime) and
 * pinning the matrix rows that must not regress:
 *
 *   1. AT-family on the writable /tmp volume: openat/mkdirat/fstatat
 *      (with and without AT_SYMLINK_NOFOLLOW)/faccessat/renameat/unlinkat
 *      (file + AT_REMOVEDIR)/readlinkat, plus a cwd-relative openat via
 *      AT_FDCWD and symlink+readlink round-trips;
 *   2. AT-family on the FAT32 volume when it is mounted (skipped otherwise);
 *   3. posix_spawn with an explicit argv/envp (fork+exec with live
 *      callee-saved registers, see kernel/proc/fork_return.asm);
 *   4. message-queue send/receive round-trip on the same descriptor;
 *   5. named semaphores: documented partial (sem_open fails with ENOSYS:
 *      needs MAP_SHARED backing, see tests/posix2024/known_partials.txt),
 *      plus process-private unnamed semaphore sanity (init/wait/post/
 *      trywait-EAGAIN/destroy);
 *   6. clock_nanosleep(TIMER_ABSTIME) actually sleeps until the deadline;
 *   7. getentropy bounds: success, non-determinism, length>256 -> EIO;
 *   8. scandir ordering: alphasort and versionsort.
 *
 * Every check prints a stable "CONFORMTEST PASS <name>" / "CONFORMTEST FAIL
 * <name> (errno=N)" marker; the summary line "CONFORMTEST ALL PASS" plus
 * exit code 0 is what the integration case asserts.
 */

#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "fcntl.h"
#include "dirent.h"
#include "time.h"
#include "sys/wait.h"
#include "sys/stat.h"
#include "spawn.h"
#include "mqueue.h"
#include "semaphore.h"
#include "signal.h"      /* Q15: mq_notify SIGEV_SIGNAL/SIGEV_THREAD; Q16: sig2str */
#include "sys/select.h"  /* Q16: pselect */
#include "poll.h"        /* Q16: ppoll */
#include "sys/random.h"  /* Q16: getrandom */
#include "sys/ipc.h"     /* Q14: System V IPC */
#include "sys/sem.h"
#include "sys/shm.h"
#include "sys/msg.h"

static int failures = 0;

#define CHECK(name, cond) do {                                              \
    if (cond) {                                                             \
        printf("CONFORMTEST PASS %s\n", name);                              \
    } else {                                                                \
        failures++;                                                         \
        printf("CONFORMTEST FAIL %s (errno=%d)\n", name, errno);            \
    }                                                                       \
} while (0)

#define CHECK_SKIP(name, why) printf("CONFORMTEST SKIP %s (%s)\n", name, why)

/* ------------------------------------------------------------------ */
/* 1. AT-family on /tmp (tmpfs with real directory semantics, Q12)     */
/* ------------------------------------------------------------------ */
static void test_at_tmpfs(void) {
    errno = 0;

    /* Cleanup any leftovers from a previous run. */
    unlinkat(AT_FDCWD, "/tmp/q12c/sub", AT_REMOVEDIR);
    unlinkat(AT_FDCWD, "/tmp/q12c/link", 0);
    unlinkat(AT_FDCWD, "/tmp/q12c/renamed.txt", 0);
    unlinkat(AT_FDCWD, "/tmp/q12c/w.txt", 0);
    unlinkat(AT_FDCWD, "/tmp/q12c", AT_REMOVEDIR);

    errno = 0;
    CHECK("at-tmpfs: mkdir /tmp/q12c",
          mkdir("/tmp/q12c", 0755) == 0 || errno == EEXIST);

    /* openat create + write + read-back (plain path, AT_FDCWD). */
    int fd = openat(AT_FDCWD, "/tmp/q12c/w.txt",
                    O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("at-tmpfs: openat(O_CREAT) creates", fd >= 0);
    if (fd >= 0) {
        errno = 0;
        CHECK("at-tmpfs: write to openat fd",
              write(fd, "q12 payload", 11) == 11);
        close(fd);
    }
    fd = openat(AT_FDCWD, "/tmp/q12c/w.txt", O_RDONLY);
    CHECK("at-tmpfs: openat(O_RDONLY) reopens", fd >= 0);
    if (fd >= 0) {
        char buf[32];
        memset(buf, 0, sizeof(buf));
        errno = 0;
        CHECK("at-tmpfs: read back payload",
              read(fd, buf, sizeof(buf) - 1) == 11 &&
              strcmp(buf, "q12 payload") == 0);
        close(fd);
    }

    /* Cwd-relative openat via AT_FDCWD (kernel joins the cwd). */
    fd = openat(AT_FDCWD, "tmp/q12c/w.txt", O_RDONLY);
    CHECK("at-tmpfs: cwd-relative openat via AT_FDCWD", fd >= 0);
    if (fd >= 0) close(fd);

    /* fstatat: type bits must be visible (S_ISREG) and size must match. */
    struct stat st;
    memset(&st, 0, sizeof(st));
    errno = 0;
    CHECK("at-tmpfs: fstatat returns 0",
          fstatat(AT_FDCWD, "/tmp/q12c/w.txt", &st, 0) == 0);
    CHECK("at-tmpfs: fstatat st_mode has S_IFREG (S_ISREG)",
          S_ISREG(st.st_mode));
    CHECK("at-tmpfs: fstatat st_size matches",
          st.st_size == 11);

    /* mkdirat + faccessat. */
    errno = 0;
    CHECK("at-tmpfs: mkdirat creates subdir",
          mkdirat(AT_FDCWD, "/tmp/q12c/sub", 0755) == 0 || errno == EEXIST);
    errno = 0;
    CHECK("at-tmpfs: faccessat(F_OK) on dir",
          faccessat(AT_FDCWD, "/tmp/q12c/sub", F_OK, 0) == 0);

    /* renameat file. */
    errno = 0;
    CHECK("at-tmpfs: renameat file",
          renameat(AT_FDCWD, "/tmp/q12c/w.txt",
                   AT_FDCWD, "/tmp/q12c/renamed.txt") == 0);
    errno = 0;
    CHECK("at-tmpfs: old name gone after rename",
          openat(AT_FDCWD, "/tmp/q12c/w.txt", O_RDONLY) < 0 && errno == ENOENT);

    /* symlink + readlinkat, and fstatat with AT_SYMLINK_NOFOLLOW. */
    unlink("/tmp/q12c/link");
    errno = 0;
    CHECK("at-tmpfs: symlink to renamed.txt",
          symlink("renamed.txt", "/tmp/q12c/link") == 0);
    char linkbuf[64];
    memset(linkbuf, 0, sizeof(linkbuf));
    errno = 0;
    CHECK("at-tmpfs: readlinkat returns target",
          readlinkat(AT_FDCWD, "/tmp/q12c/link", linkbuf, sizeof(linkbuf)) ==
          (ssize_t)strlen("renamed.txt") &&
          strcmp(linkbuf, "renamed.txt") == 0);
    struct stat lstat_st;
    memset(&lstat_st, 0, sizeof(lstat_st));
    errno = 0;
    CHECK("at-tmpfs: fstatat follows symlink (regular file)",
          fstatat(AT_FDCWD, "/tmp/q12c/link", &lstat_st, 0) == 0 &&
          S_ISREG(lstat_st.st_mode));
    memset(&lstat_st, 0, sizeof(lstat_st));
    errno = 0;
    CHECK("at-tmpfs: fstatat AT_SYMLINK_NOFOLLOW sees the link itself",
          fstatat(AT_FDCWD, "/tmp/q12c/link", &lstat_st,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
          (lstat_st.st_mode & S_IFMT) == S_IFLNK);

    /* unlinkat: file, then directory with AT_REMOVEDIR. */
    errno = 0;
    CHECK("at-tmpfs: unlinkat removes file",
          unlinkat(AT_FDCWD, "/tmp/q12c/renamed.txt", 0) == 0);
    errno = 0;
    CHECK("at-tmpfs: unlinkat AT_REMOVEDIR removes empty dir",
          unlinkat(AT_FDCWD, "/tmp/q12c/sub", AT_REMOVEDIR) == 0);
    errno = 0;
    CHECK("at-tmpfs: unlinkat AT_REMOVEDIR on non-dir is ENOTDIR",
          unlinkat(AT_FDCWD, "/tmp/q12c/link", AT_REMOVEDIR) < 0 &&
          errno == ENOTDIR);
    unlinkat(AT_FDCWD, "/tmp/q12c/link", 0);
    unlinkat(AT_FDCWD, "/tmp/q12c", AT_REMOVEDIR);
}

/* ------------------------------------------------------------------ */
/* 2. AT-family on the FAT32 volume (skipped when not mounted)         */
/* ------------------------------------------------------------------ */
static void test_at_fat(void) {
    struct stat st;
    if (stat("/fat", &st) != 0 || !S_ISDIR(st.st_mode)) {
        CHECK_SKIP("at-fat: /fat not mounted in this boot", "no FAT32 volume");
        return;
    }
    errno = 0;
    int fd = openat(AT_FDCWD, "/fat/q12fat.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("at-fat: openat(O_CREAT) on FAT32", fd >= 0);
    if (fd >= 0) {
        errno = 0;
        CHECK("at-fat: write on FAT32", write(fd, "fat12", 5) == 5);
        close(fd);
    }
    errno = 0;
    CHECK("at-fat: renameat on FAT32",
          renameat(AT_FDCWD, "/fat/q12fat.txt", AT_FDCWD, "/fat/q12fat2.txt") == 0);
    errno = 0;
    CHECK("at-fat: unlinkat on FAT32",
          unlinkat(AT_FDCWD, "/fat/q12fat2.txt", 0) == 0);
}

/* ------------------------------------------------------------------ */
/* 3. posix_spawn with explicit argv/envp                              */
/* ------------------------------------------------------------------ */
static void test_spawn(void) {
    char *const argv[] = { (char *)"argv_echo", (char *)"q12",
                           (char *)"sp ace", (char *)0 };
    char *const envp[] = { (char *)"A=Q12", (char *)0 };
    pid_t pid = -1;
    errno = 0;
    int r = posix_spawn(&pid, "/tests/argv_echo", NULL, NULL, argv, envp);
    CHECK("spawn: posix_spawn returns 0", r == 0);
    if (r == 0) {
        int status = -1;
        errno = 0;
        CHECK("spawn: waitpid reaps child", waitpid(pid, &status, 0) == pid);
        CHECK("spawn: child exit status is 0", status == 0);
    }
}

/* ------------------------------------------------------------------ */
/* 4. Message queue round-trip                                         */
/* ------------------------------------------------------------------ */
static void test_mqueue(void) {
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = 64;

    mq_unlink("/q12mq");
    errno = 0;
    mqd_t mq = mq_open("/q12mq", O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    CHECK("mqueue: mq_open(O_CREAT|O_EXCL)", mq != (mqd_t)-1);
    if (mq == (mqd_t)-1) return;

    errno = 0;
    CHECK("mqueue: mq_send 4-byte message",
          mq_send(mq, "ping", 4, 0) == 0);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    errno = 0;
    CHECK("mqueue: mq_receive round-trips the payload",
          mq_receive(mq, buf, sizeof(buf), NULL) == 4 &&
          strcmp(buf, "ping") == 0);
    errno = 0;
    CHECK("mqueue: mq_send + mq_receive again (cursor advanced)",
          mq_send(mq, "pong!", 5, 0) == 0 &&
          mq_receive(mq, buf, sizeof(buf), NULL) == 5 &&
          strcmp(buf, "pong!") == 0);

    mq_close(mq);
    mq_unlink("/q12mq");
}

/* ------------------------------------------------------------------ */
/* 4b. Q15: mq_notify + sigevent delivery                              */
/* ------------------------------------------------------------------ */
static volatile int mq_notify_sig_flag = 0;
static void mq_notify_sig_handler(int sig) {
    (void)sig;
    mq_notify_sig_flag = 1;
}

static volatile int mq_notify_thread_count = 0;
static void mq_notify_thread_fn(union sigval v) {
    (void)v;
    mq_notify_thread_count++;
}

/* Poll a volatile flag for up to `ms` (5 ms sleeps).  Returns 1 when the
 * condition becomes true, 0 on timeout. */
static int mq_wait_until(volatile int *cond, int ms) {
    int iters = ms / 5;
    while (iters-- > 0 && !*cond) usleep(5000);
    return *cond;
}

static void test_mq_notify(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mq_notify_sig_handler;
    sigemptyset(&sa.sa_mask);
    errno = 0;
    CHECK("mq_notify: sigaction(SIGUSR1) installs", sigaction(SIGUSR1, &sa, NULL) == 0);

    mq_unlink("/q15mq");
    errno = 0;
    mqd_t mq = mq_open("/q15mq", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
    CHECK("mq_notify: mq_open", mq != (mqd_t)-1);
    if (mq == (mqd_t)-1) return;

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo  = SIGUSR1;

    errno = 0;
    CHECK("mq_notify: register SIGEV_SIGNAL", mq_notify(mq, &sev) == 0);

    errno = 0;
    CHECK("mq_notify: second registration gives EBUSY",
          mq_notify(mq, &sev) == -1 && errno == EBUSY);

    /* The freshly created queue is empty; a forked sender produces the
     * empty -> non-empty edge POSIX keys on.  The parent waits for the
     * child FIRST (blocking waitpid guarantees the child gets CPU even
     * under slow TCG), then polls for the asynchronous delivery. */
    mq_notify_sig_flag = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int rc = mq_send(mq, "ping", 4, 0);
        _exit(rc ? 2 : 0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    CHECK("mq_notify: SIGEV_SIGNAL delivered on empty->non-empty",
          mq_wait_until(&mq_notify_sig_flag, 6000));

    /* Re-arm: drain the queue, send again, expect a second delivery. */
    {
        char buf[64];
        while (mq_receive(mq, buf, sizeof(buf), NULL) >= 0) { }
    }
    mq_notify_sig_flag = 0;
    mq_send(mq, "again", 5, 0);
    CHECK("mq_notify: re-armed after drain (second delivery)",
          mq_wait_until(&mq_notify_sig_flag, 6000));

    /* Deregistration stops delivery. */
    errno = 0;
    CHECK("mq_notify: deregister with NULL", mq_notify(mq, NULL) == 0);
    {
        char buf[64];
        while (mq_receive(mq, buf, sizeof(buf), NULL) >= 0) { }
    }
    mq_notify_sig_flag = 0;
    mq_send(mq, "ghost", 5, 0);
    usleep(300000);   /* 300 ms: several watcher polls would have fired */
    CHECK("mq_notify: no delivery after deregistration",
          mq_notify_sig_flag == 0);

    /* SIGEV_THREAD: run the notification function on a fresh pthread.
     * Drain first: the queue still holds "ghost", and the watcher must
     * start in the empty state or it never sees the 0 -> >0 edge. */
    {
        char buf[64];
        while (mq_receive(mq, buf, sizeof(buf), NULL) >= 0) { }
    }
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = mq_notify_thread_fn;
    sev.sigev_value.sival_int = 7;
    errno = 0;
    CHECK("mq_notify: register SIGEV_THREAD", mq_notify(mq, &sev) == 0);
    mq_notify_thread_count = 0;
    CHECK("mq_notify: send before THREAD wait", mq_send(mq, "thr", 3, 0) == 0);
    /* The notification function runs on the watcher thread (annotated
     * deviation, see libc/src/posix_extra.c mq_notify_deliver), so it is
     * delivered within one watcher poll. */
    CHECK("mq_notify: SIGEV_THREAD runs the notification function",
          mq_wait_until(&mq_notify_thread_count, 6000));
    CHECK("mq_notify: deregister SIGEV_THREAD", mq_notify(mq, NULL) == 0);

    mq_close(mq);
    mq_unlink("/q15mq");
}

/* ------------------------------------------------------------------ */
/* 5. Semaphores: named = documented partial; unnamed = in-process     */
/* ------------------------------------------------------------------ */
static void test_sem(void) {
    /* Named: POSIX wants them on shared memory (/dev/shm); the kernel has
     * no MAP_SHARED backing yet, so sem_open must fail with ENOSYS (see
     * tests/posix2024/known_partials.txt; planned for Q14/Q15). */
    sem_t *s = sem_open("/q12sem", O_CREAT | O_EXCL, 0600, 0);
    CHECK("sem: named sem_open is the documented partial (ENOSYS)",
          s == SEM_FAILED && errno == ENOSYS);

    /* Process-private unnamed semaphore sanity. */
    sem_t us;
    errno = 0;
    CHECK("sem: sem_init value 2", sem_init(&us, 0, 2) == 0);
    errno = 0;
    CHECK("sem: sem_wait #1", sem_wait(&us) == 0);
    errno = 0;
    CHECK("sem: sem_wait #2", sem_wait(&us) == 0);
    errno = 0;
    CHECK("sem: sem_trywait EAGAIN when empty",
          sem_trywait(&us) == -1 && errno == EAGAIN);
    errno = 0;
    CHECK("sem: sem_post bumps", sem_post(&us) == 0);
    errno = 0;
    CHECK("sem: sem_wait consumes post", sem_wait(&us) == 0);
    errno = 0;
    CHECK("sem: sem_destroy", sem_destroy(&us) == 0);
}

/* ------------------------------------------------------------------ */
/* 6. clock_nanosleep(TIMER_ABSTIME)                                   */
/* ------------------------------------------------------------------ */
static void test_clock_nanosleep(void) {
    struct timespec now, deadline;
    errno = 0;
    CHECK("clockns: clock_gettime(MONOTONIC)",
          clock_gettime(CLOCK_MONOTONIC, &now) == 0);

    deadline = now;
    deadline.tv_sec += 0;
    deadline.tv_nsec += 300 * 1000000L;   /* +300 ms */
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    errno = 0;
    int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    CHECK("clockns: clock_nanosleep(TIMER_ABSTIME) returns 0", r == 0);

    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &after);
    int64_t slept_ms = (after.tv_sec - now.tv_sec) * 1000L
                     + (after.tv_nsec - now.tv_nsec) / 1000000L;
    CHECK("clockns: slept >= ~200 ms (abstime honoured)", slept_ms >= 200);
    CHECK("clockns: slept < 30 s (no hang)", slept_ms < 30000);

    /* Relative mode still works (regression guard). */
    struct timespec rel = { 0, 10 * 1000000L };   /* 10 ms */
    errno = 0;
    CHECK("clockns: relative mode returns 0",
          clock_nanosleep(CLOCK_MONOTONIC, 0, &rel, NULL) == 0);
}

/* ------------------------------------------------------------------ */
/* 7. getentropy bounds                                                */
/* ------------------------------------------------------------------ */
static void test_getentropy(void) {
    unsigned char b1[16], b2[16];
    memset(b1, 0, sizeof(b1));
    memset(b2, 0xff, sizeof(b2));
    errno = 0;
    CHECK("entropy: getentropy(16) succeeds", getentropy(b1, sizeof(b1)) == 0);
    errno = 0;
    CHECK("entropy: second draw differs", getentropy(b2, sizeof(b2)) == 0 &&
          memcmp(b1, b2, sizeof(b1)) != 0);
    unsigned char big[300];
    errno = 0;
    CHECK("entropy: length > 256 fails with EIO",
          getentropy(big, sizeof(big)) == -1 && errno == EIO);
}

/* ------------------------------------------------------------------ */
/* 8. scandir ordering                                                 */
/* ------------------------------------------------------------------ */
static void test_scandir(void) {
    /* Sandbox with files whose names sort differently alphabetically vs
     * numerically: fB, fA, fc10, fc2.
     *   alphasort  -> fA, fB, fc10, fc2   ('1' < '2' in "fc10" vs "fc2")
     *   versionsort-> fA, fB, fc2, fc10   (numeric 2 < 10)
     */
    mkdir("/tmp/q12sd", 0755);
    const char *names[] = { "fB", "fA", "fc10", "fc2" };
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/tmp/q12sd/%s", names[i]);
        int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }

    struct dirent **list = NULL;
    errno = 0;
    int n = scandir("/tmp/q12sd", &list, NULL, alphasort);
    CHECK("scandir: scandir returns 4 entries", n == 4);
    if (n == 4) {
        CHECK("scandir: alphasort orders fA,fB,fc10,fc2",
              strcmp(list[0]->d_name, "fA") == 0 &&
              strcmp(list[1]->d_name, "fB") == 0 &&
              strcmp(list[2]->d_name, "fc10") == 0 &&
              strcmp(list[3]->d_name, "fc2") == 0);
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);

    list = NULL;
    errno = 0;
    n = scandir("/tmp/q12sd", &list, NULL, versionsort);
    CHECK("scandir: versionsort returns 4 entries", n == 4);
    if (n == 4) {
        CHECK("scandir: versionsort orders fA,fB,fc2,fc10",
              strcmp(list[0]->d_name, "fA") == 0 &&
              strcmp(list[1]->d_name, "fB") == 0 &&
              strcmp(list[2]->d_name, "fc2") == 0 &&
              strcmp(list[3]->d_name, "fc10") == 0);
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);

    /* Cleanup. */
    for (int i = 0; i < 4; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/tmp/q12sd/%s", names[i]);
        unlink(p);
    }
    rmdir("/tmp/q12sd");
}

/* ------------------------------------------------------------------ */
/* Q13 (POSIX2024_PLAN.md phase Q13): AT-family completion             */
/* ------------------------------------------------------------------ */

/* 9. link/linkat on tmpfs: content sharing, nlink, inode identity,
 * EEXIST/EPERM/EXDEV, FAT32-EPERM, and unlink-of-one-name semantics. */
static void test_link(void) {
    mkdir("/tmp/q13a", 0755);
    int fd = open("/tmp/q13a/a.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "linkme", 6);
        close(fd);
    }

    errno = 0;
    CHECK("link: link() creates a second name",
          link("/tmp/q13a/a.txt", "/tmp/q13a/b.txt") == 0);
    fd = open("/tmp/q13a/b.txt", O_RDONLY);
    CHECK("link: second name opens", fd >= 0);
    if (fd >= 0) {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        CHECK("link: content shared through both names",
              read(fd, buf, sizeof(buf) - 1) == 6 &&
              strcmp(buf, "linkme") == 0);
        close(fd);
    }
    /* write via b -> visible via a (same data block). */
    fd = open("/tmp/q13a/b.txt", O_WRONLY);
    if (fd >= 0) {
        lseek(fd, 6, SEEK_SET);
        write(fd, "-b", 2);
        close(fd);
    }
    fd = open("/tmp/q13a/a.txt", O_RDONLY);
    if (fd >= 0) {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        read(fd, buf, sizeof(buf) - 1);
        CHECK("link: write via one name visible via the other",
              strcmp(buf, "linkme-b") == 0);
        close(fd);
    }
    struct stat sa, sb;
    memset(&sa, 0, sizeof(sa));
    memset(&sb, 0, sizeof(sb));
    stat("/tmp/q13a/a.txt", &sa);
    stat("/tmp/q13a/b.txt", &sb);
    CHECK("link: st_nlink == 2", sa.st_nlink == 2);
    CHECK("link: both names share the inode", sa.st_inode == sb.st_inode);

    errno = 0;
    CHECK("link: existing target gives EEXIST",
          link("/tmp/q13a/a.txt", "/tmp/q13a/b.txt") == -1 &&
          errno == EEXIST);
    errno = 0;
    CHECK("link: directory gives EPERM",
          link("/tmp/q13a", "/tmp/q13a/dirlink") == -1 && errno == EPERM);

    errno = 0;
    CHECK("linkat: creates via AT_FDCWD",
          linkat(AT_FDCWD, "/tmp/q13a/a.txt", AT_FDCWD, "/tmp/q13a/c.txt",
                 0) == 0);

    errno = 0;
    CHECK("link: cross-device gives EXDEV",
          link("/tmp/q13a/a.txt", "/dev/shm/q13x") == -1 && errno == EXDEV);

    /* FAT32 has no link support: EPERM (POSIX wording). */
    struct stat fst;
    if (stat("/fat", &fst) == 0 && S_ISDIR(fst.st_mode)) {
        int ffd = open("/fat/q13fl.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (ffd >= 0) close(ffd);
        errno = 0;
        CHECK("link: FAT32 gives EPERM (links unsupported)",
              link("/fat/q13fl.txt", "/fat/q13fl2.txt") == -1 &&
              errno == EPERM);
        unlink("/fat/q13fl.txt");
    }

    /* Unlink one name: the other stays alive (refcounted data). */
    unlink("/tmp/q13a/b.txt");
    errno = 0;
    CHECK("link: unlink of one name keeps the other alive",
          open("/tmp/q13a/a.txt", O_RDONLY) >= 0);
    unlink("/tmp/q13a/a.txt");
    unlink("/tmp/q13a/c.txt");
    rmdir("/tmp/q13a");
}

/* 10. symlinkat. */
static void test_symlinkat(void) {
    mkdir("/tmp/q13b", 0755);
    int fd = open("/tmp/q13b/a.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "x", 1);
        close(fd);
    }
    errno = 0;
    CHECK("symlinkat: creates a link",
          symlinkat("a.txt", AT_FDCWD, "/tmp/q13b/sl") == 0);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    errno = 0;
    CHECK("symlinkat: readlinkat sees the target",
          readlinkat(AT_FDCWD, "/tmp/q13b/sl", buf, sizeof(buf)) == 5 &&
          strcmp(buf, "a.txt") == 0);
    unlink("/tmp/q13b/sl");
    unlink("/tmp/q13b/a.txt");
    rmdir("/tmp/q13b");
}

/* 11. mkfifoat / mknodat / mknod. */
static void test_mknod(void) {
    mkdir("/tmp/q13c", 0755);
    errno = 0;
    CHECK("mknod: mkfifoat creates a FIFO",
          mkfifoat(AT_FDCWD, "/tmp/q13c/fifo", 0644) == 0);
    struct stat st;
    memset(&st, 0, sizeof(st));
    stat("/tmp/q13c/fifo", &st);
    CHECK("mknod: FIFO type in st_mode", S_ISFIFO(st.st_mode));

    errno = 0;
    CHECK("mknod: mknodat creates a regular file",
          mknodat(AT_FDCWD, "/tmp/q13c/n.txt", S_IFREG | 0644, 0) == 0);
    memset(&st, 0, sizeof(st));
    stat("/tmp/q13c/n.txt", &st);
    CHECK("mknod: regular type in st_mode", S_ISREG(st.st_mode));

    errno = 0;
    CHECK("mknod: mknodat on existing gives EEXIST",
          mknodat(AT_FDCWD, "/tmp/q13c/n.txt", S_IFREG | 0644, 0) == -1 &&
          errno == EEXIST);
    errno = 0;
    CHECK("mknod: device node gives ENOSYS (no devfs backing)",
          mknodat(AT_FDCWD, "/tmp/q13c/dev", S_IFCHR | 0600, 1) == -1 &&
          errno == ENOSYS);
    errno = 0;
    CHECK("mknod: mknod() with a plain path works",
          mknod("/tmp/q13c/p.txt", S_IFREG | 0644, 0) == 0);

    unlink("/tmp/q13c/fifo");
    unlink("/tmp/q13c/n.txt");
    unlink("/tmp/q13c/p.txt");
    rmdir("/tmp/q13c");
}

/* 12. utimensat / futimens: explicit times, UTIME_NOW, UTIME_OMIT, and
 * read-back through stat/fstat (second-granularity storage). */
static void test_utimens(void) {
    mkdir("/tmp/q13d", 0755);
    int fd = open("/tmp/q13d/t.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "ut", 2);
        close(fd);
    }

    struct timespec ts[2];
    ts[0].tv_sec = 2000000000; ts[0].tv_nsec = 0;   /* atime */
    ts[1].tv_sec = 1500000000; ts[1].tv_nsec = 0;   /* mtime */
    errno = 0;
    CHECK("utimens: utimensat sets explicit times",
          utimensat(AT_FDCWD, "/tmp/q13d/t.txt", ts, 0) == 0);
    struct stat st;
    memset(&st, 0, sizeof(st));
    stat("/tmp/q13d/t.txt", &st);
    CHECK("utimens: atime read back (within 1s)",
          st.st_atime == 2000000000 || st.st_atime == 2000000001);
    CHECK("utimens: mtime read back (within 1s)",
          st.st_mtime == 1500000000 || st.st_mtime == 1500000001);

    ts[0].tv_nsec = UTIME_NOW;
    ts[1].tv_nsec = UTIME_OMIT;
    errno = 0;
    CHECK("utimens: UTIME_NOW/UTIME_OMIT accepted",
          utimensat(AT_FDCWD, "/tmp/q13d/t.txt", ts, 0) == 0);
    time_t now = time(NULL);
    memset(&st, 0, sizeof(st));
    stat("/tmp/q13d/t.txt", &st);
    CHECK("utimens: atime updated to now",
          (int64_t)st.st_atime >= now - 2 && (int64_t)st.st_atime <= now + 2);
    CHECK("utimens: mtime kept (UTIME_OMIT)",
          st.st_mtime == 1500000000 || st.st_mtime == 1500000001);

    fd = open("/tmp/q13d/t.txt", O_RDWR);
    ts[0].tv_sec = 1000000000; ts[0].tv_nsec = 0;
    ts[1].tv_sec = 900000000;  ts[1].tv_nsec = 0;
    errno = 0;
    CHECK("utimens: futimens on an fd",
          fd >= 0 && futimens(fd, ts) == 0);
    memset(&st, 0, sizeof(st));
    fstat(fd, &st);
    CHECK("utimens: futimens mtime read back via fstat",
          st.st_mtime == 900000000 || st.st_mtime == 900000001);
    if (fd >= 0) close(fd);

    errno = 0;
    CHECK("utimens: NULL times sets both to now",
          utimensat(AT_FDCWD, "/tmp/q13d/t.txt", NULL, 0) == 0);

    ts[0].tv_sec = 0; ts[0].tv_nsec = 2000000000;   /* out of range */
    ts[1].tv_sec = 0; ts[1].tv_nsec = 0;
    errno = 0;
    CHECK("utimens: out-of-range tv_nsec gives EINVAL",
          utimensat(AT_FDCWD, "/tmp/q13d/t.txt", ts, 0) == -1 &&
          errno == EINVAL);

    unlink("/tmp/q13d/t.txt");
    rmdir("/tmp/q13d");
}

/* 13. fdopendir + dirfd interop (via /proc/self/fd). */
static void test_fdopendir(void) {
    mkdir("/tmp/q13e", 0755);
    mkdir("/tmp/q13e/d", 0755);
    int fd = open("/tmp/q13e/d/x1", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    fd = open("/tmp/q13e/d/x2", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);

    int dfd = open("/tmp/q13e/d", O_RDONLY | O_DIRECTORY);
    CHECK("fdopendir: open a directory fd", dfd >= 0);
    if (dfd >= 0) {
        DIR *dp = fdopendir(dfd);
        CHECK("fdopendir: stream created", dp != NULL);
        if (dp) {
            CHECK("fdopendir: dirfd() returns the fd", dirfd(dp) == dfd);
            int found = 0;
            struct dirent *e;
            while ((e = readdir(dp)) != NULL) {
                if (strcmp(e->d_name, "x1") == 0 ||
                    strcmp(e->d_name, "x2") == 0)
                    found++;
            }
            CHECK("fdopendir: readdir lists the entries", found == 2);
            closedir(dp);
        }
    }

    DIR *dp2 = opendir("/tmp/q13e/d");
    CHECK("fdopendir: opendir yields a real dirfd",
          dp2 != NULL && dirfd(dp2) >= 0);
    if (dp2) closedir(dp2);

    /* fdopendir on a non-directory fd -> ENOTDIR, fd left open. */
    int ffd = open("/tmp/q13e/d/x1", O_RDONLY);
    if (ffd >= 0) {
        errno = 0;
        DIR *bad = fdopendir(ffd);
        CHECK("fdopendir: non-directory fd gives ENOTDIR",
              bad == NULL && errno == ENOTDIR);
        if (bad)
            closedir(bad);
        else
            close(ffd);   /* POSIX: fd stays open when fdopendir fails */
    }

    unlink("/tmp/q13e/d/x1");
    unlink("/tmp/q13e/d/x2");
    rmdir("/tmp/q13e/d");
    rmdir("/tmp/q13e");
}

/* 14. fexecve: exec a binary by fd (proves /proc/self/fd works; the child
 * prints ARGV_ECHO markers with the custom argv/envp). */
static void test_fexecve(void) {
    int fd = open("/tests/argv_echo", O_RDONLY);
    CHECK("fexecve: open the target binary", fd >= 0);
    if (fd < 0) return;

    char *const argv[] = { (char *)"argv_echo", (char *)"fex", (char *)"0",
                           (char *)0 };
    char *const envp[] = { (char *)"B=fex", (char *)0 };
    pid_t c = fork();
    if (c == 0) {
        fexecve(fd, argv, envp);
        _exit(127);
    }
    int status = -1;
    errno = 0;
    CHECK("fexecve: waitpid reaps child", waitpid(c, &status, 0) == c);
    CHECK("fexecve: child exit status is 0", status == 0);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Q16: Issue-8 tail — pselect/ppoll, getrandom, sig2str/str2sig       */
/* ------------------------------------------------------------------ */
static volatile int q16_sig_flag = 0;
static void q16_sig_handler(int sig) {
    (void)sig;
    q16_sig_flag = 1;
}

static void test_q16(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = q16_sig_handler;
    sigemptyset(&sa.sa_mask);
    CHECK("q16: sigaction(SIGUSR2) installs", sigaction(SIGUSR2, &sa, NULL) == 0);

    /* ---- pselect: the mask is applied atomically with the block, and a
     * pending (unmasked) signal wakes the wait with -EINTR.  A sender
     * child fires SIGUSR2 every 40 ms; the main thread blocks in pselect
     * with an EMPTY mask (SIGUSR2 is NOT blocked, so it must interrupt).
     * If signals could not wake a blocked select, every iteration would
     * hit the 5 s timeout and the count would be 0. */
    {
        sigset_t mask;
        sigemptyset(&mask);        /* empty: nothing blocked */
        struct timespec five_sec = { 5, 0 };
        int interrupted = 0;
        pid_t me = getpid();
        pid_t sender = fork();
        if (sender == 0) {
            for (;;) {
                usleep(40000);
                kill(me, SIGUSR2);
            }
        }
        for (int i = 0; i < 20; i++) {
            q16_sig_flag = 0;
            errno = 0;
            int r = pselect(0, NULL, NULL, NULL, &five_sec, &mask);
            if (r == -1 && errno == EINTR) interrupted++;
            if (r == 0) break;     /* timeout = lost wakeup */
        }
        CHECK("q16: pselect wakes on signal with EINTR (20/20)",
              interrupted == 20);
        if (sender > 0) kill(sender, SIGKILL);
        if (sender > 0) waitpid(sender, NULL, 0);

        /* The mask is honoured DURING the block: a mask that blocks
         * SIGUSR2 must prevent the interrupt (pselect returns 0 on the
         * timeout instead of EINTR). */
        sigaddset(&mask, SIGUSR2);
        q16_sig_flag = 0;
        struct timespec short_ts = { 0, 300000000 };   /* 300 ms */
        errno = 0;
        int r2 = pselect(0, NULL, NULL, NULL, &short_ts, &mask);
        CHECK("q16: pselect mask blocks the signal (no EINTR)",
              r2 == 0);
    }

    /* ---- ppoll on a pipe: readiness reported. */
    {
        int pfd[2];
        CHECK("q16: pipe() for ppoll", pipe(pfd) == 0);
        struct pollfd fds[1];
        fds[0].fd = pfd[0];
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        struct timespec zero = { 0, 0 };
        int r = ppoll(fds, 1, &zero, NULL);
        CHECK("q16: ppoll returns 0 with no data", r == 0);
        CHECK("q16: ppoll revents empty", fds[0].revents == 0);
        CHECK("q16: ppoll write end", write(pfd[1], "x", 1) == 1);
        fds[0].revents = 0;
        r = ppoll(fds, 1, &zero, NULL);
        CHECK("q16: ppoll reports POLLIN", r == 1 && (fds[0].revents & POLLIN));
        /* Drain the single byte so the ppoll below actually blocks (a
         * loop-until-0 would block on the empty pipe with the writer open). */
        {
            char drain[4];
            (void)read(pfd[0], drain, sizeof(drain));
        }
        /* ppoll with an empty signal mask wakes on signal like pselect. */
        fds[0].revents = 0;
        sigset_t mask;
        sigemptyset(&mask);        /* empty: SIGUSR2 not blocked */
        struct timespec five_sec = { 5, 0 };
        q16_sig_flag = 0;
        pid_t me = getpid();
        pid_t sender = fork();
        if (sender == 0) {
            usleep(100000);
            kill(me, SIGUSR2);
            _exit(0);
        }
        errno = 0;
        r = ppoll(fds, 1, &five_sec, &mask);
        CHECK("q16: ppoll interrupted by signal (EINTR)",
              r == -1 && errno == EINTR);
        if (sender > 0) waitpid(sender, NULL, 0);
        close(pfd[0]);
        close(pfd[1]);
    }

    /* ---- getrandom. */
    {
        uint8_t a[32], b[32];
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        errno = 0;
        CHECK("q16: getrandom(32) returns 32",
              getrandom(a, sizeof(a), 0) == (ssize_t)sizeof(a));
        CHECK("q16: getrandom(32) again",
              getrandom(b, sizeof(b), GRND_NONBLOCK) == (ssize_t)sizeof(b));
        CHECK("q16: two getrandom streams differ", memcmp(a, b, sizeof(a)) != 0);
        errno = 0;
        CHECK("q16: getrandom(0) returns 0", getrandom(NULL, 0, 0) == 0);
        errno = 0;
        CHECK("q16: getrandom unknown flags -> EINVAL",
              getrandom(a, sizeof(a), 0x8000) == -1 && errno == EINVAL);
    }

    /* ---- sig2str / str2sig round trip. */
    {
        char buf[SIG2STR_MAX];
        int ok = 1, n = 0;
        for (int s = 1; s < NSIG; s++) {
            if (sig2str(s, buf) != 0) continue;
            n++;
            int back = -1;
            if (str2sig(buf, &back) != 0 || back != s) ok = 0;
            char prefixed[SIG2STR_MAX + 4];
            snprintf(prefixed, sizeof(prefixed), "SIG%s", buf);
            if (str2sig(prefixed, &back) != 0 || back != s) ok = 0;
        }
        CHECK("q16: sig2str/str2sig round-trips every named signal", ok);
        CHECK("q16: >= 20 named signals", n >= 20);
        errno = 0;
        int s = 0;
        CHECK("q16: str2sig(\"NOSUCH\") -> EINVAL",
              str2sig("NOSUCH", &s) == -1 && errno == EINVAL);
    }
}

/* ------------------------------------------------------------------ */
/* Q14: System V IPC — semaphores, shared memory, message queues       */
/* ------------------------------------------------------------------ */
static void test_sysvipc(void) {
    /* ---- semaphores: semget/semctl(SETVAL|GETVAL)/semop ---- */
    {
        int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
        CHECK("sysv: semget(IPC_PRIVATE, 1) creates", semid >= 0);
        if (semid >= 0) {
            union semun { int val; struct semid_ds *buf; unsigned short *array; };
            union semun su;
            su.val = 1;
            errno = 0;
            CHECK("sysv: semctl(SETVAL, 1)", semctl(semid, 0, SETVAL, su) == 0);
            errno = 0;
            CHECK("sysv: semctl(GETVAL) == 1",
                  semctl(semid, 0, GETVAL, su) == 1);
            struct sembuf op;
            op.sem_num = 0; op.sem_op = -1; op.sem_flg = 0;   /* P() */
            errno = 0;
            CHECK("sysv: semop(P) decrements", semop(semid, &op, 1) == 0);
            errno = 0;
            CHECK("sysv: semctl(GETVAL) == 0 after P",
                  semctl(semid, 0, GETVAL, su) == 0);
            op.sem_op = 1;                                     /* V() */
            CHECK("sysv: semop(V) increments", semop(semid, &op, 1) == 0);
            CHECK("sysv: semctl(GETVAL) == 1 after V",
                  semctl(semid, 0, GETVAL, su) == 1);
            /* Blocking P() on an empty semaphore returns EINTR on signal,
             * but with IPC_NOWAIT it fails with EAGAIN. */
            op.sem_op = -2;                                    /* needs 2, have 1 */
            op.sem_flg = IPC_NOWAIT;
            errno = 0;
            CHECK("sysv: semop(IPC_NOWAIT) on insufficient -> EAGAIN",
                  semop(semid, &op, 1) == -1 && errno == EAGAIN);
            CHECK("sysv: semctl(IPC_RMID)", semctl(semid, 0, IPC_RMID, su) == 0);
        }
    }

    /* ---- shared memory: shmget/shmat/shmdt/read-write/rmid ---- */
    {
        int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        CHECK("sysv: shmget(4096) creates", shmid >= 0);
        if (shmid >= 0) {
            void *p = shmat(shmid, NULL, 0);
            CHECK("sysv: shmat returns non-NULL", p != (void *)-1 && p != NULL);
            if (p != (void *)-1 && p != NULL) {
                volatile unsigned *v = (volatile unsigned *)p;
                *v = 0xCAFEBABE;
                CHECK("sysv: shm write/read round-trip", *v == 0xCAFEBABE);
                struct shmid_ds ds;
                errno = 0;
                CHECK("sysv: shmctl(IPC_STAT) reads metadata",
                      shmctl(shmid, IPC_STAT, &ds) == 0);
                if (shmctl(shmid, IPC_STAT, &ds) == 0)
                    CHECK("sysv: shm_segsz == 4096", ds.shm_segsz == 4096);
                errno = 0;
                CHECK("sysv: shmdt detaches", shmdt(p) == 0);
            }
            errno = 0;
            CHECK("sysv: shmctl(IPC_RMID)", shmctl(shmid, IPC_RMID, NULL) == 0);
        }
    }

    /* ---- fork pair: shared counter guarded by a SysV semaphore ---- */
    {
        int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        int semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
        CHECK("sysv: fork-pair shmget", shmid >= 0);
        CHECK("sysv: fork-pair semget", semid >= 0);
        if (shmid >= 0 && semid >= 0) {
            union semun { int val; struct semid_ds *buf; unsigned short *array; };
            union semun su;
            su.val = 1;
            semctl(semid, 0, SETVAL, su);
            unsigned *counter = (unsigned *)shmat(shmid, NULL, 0);
            CHECK("sysv: fork-pair shmat", counter != (void *)-1 && counter != NULL);
            if (counter != (void *)-1 && counter != NULL) {
                *counter = 0;
                pid_t pid = fork();
                if (pid == 0) {
                    /* Child: 200 protected increments. */
                    struct sembuf op = { 0, -1, 0 };
                    for (int i = 0; i < 200; i++) {
                        semop(semid, &op, 1);
                        (*counter)++;
                        op.sem_op = 1;
                        semop(semid, &op, 1);
                        op.sem_op = -1;
                    }
                    _exit(0);
                }
                /* Parent: 200 protected increments. */
                struct sembuf op = { 0, -1, 0 };
                for (int i = 0; i < 200; i++) {
                    semop(semid, &op, 1);
                    (*counter)++;
                    op.sem_op = 1;
                    semop(semid, &op, 1);
                    op.sem_op = -1;
                }
                if (pid > 0) waitpid(pid, NULL, 0);
                CHECK("sysv: shared counter == 400 (no lost increments)",
                      *counter == 400);
                shmdt(counter);
            }
            shmctl(shmid, IPC_RMID, NULL);
            semctl(semid, 0, IPC_RMID, su);
        }
    }

    /* ---- message queues: msgsnd/msgrcv round-trip + mtype rules ---- */
    {
        int qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        CHECK("sysv: msgget(IPC_PRIVATE) creates", qid >= 0);
        if (qid >= 0) {
            struct msg1 { long mtype; char mtext[32]; } m;
            m.mtype = 1; strcpy(m.mtext, "one");
            CHECK("sysv: msgsnd(mtype=1)", msgsnd(qid, &m, 4, 0) == 0);
            m.mtype = 5; strcpy(m.mtext, "five-a");
            CHECK("sysv: msgsnd(mtype=5) a", msgsnd(qid, &m, 7, 0) == 0);
            m.mtype = 5; strcpy(m.mtext, "five-b");
            CHECK("sysv: msgsnd(mtype=5) b", msgsnd(qid, &m, 7, 0) == 0);
            m.mtype = 9; strcpy(m.mtext, "nine");
            CHECK("sysv: msgsnd(mtype=9)", msgsnd(qid, &m, 5, 0) == 0);

            /* msgtyp == 0: FIFO -> first is mtype 1. */
            memset(&m, 0, sizeof(m));
            errno = 0;
            ssize_t n = msgrcv(qid, &m, sizeof(m.mtext), 0, 0);
            CHECK("sysv: msgrcv(typ=0) FIFO returns mtype 1",
                  n == 4 && m.mtype == 1 && strcmp(m.mtext, "one") == 0);
            /* msgtyp > 0: exact match -> first mtype 5. */
            memset(&m, 0, sizeof(m));
            n = msgrcv(qid, &m, sizeof(m.mtext), 5, 0);
            CHECK("sysv: msgrcv(typ=5) returns mtype 5 'five-a'",
                  n == 7 && m.mtype == 5 && strcmp(m.mtext, "five-a") == 0);
            /* msgtyp < 0: first with mtype <= -typ -> 5 ('five-b'). */
            memset(&m, 0, sizeof(m));
            n = msgrcv(qid, &m, sizeof(m.mtext), -5, 0);
            CHECK("sysv: msgrcv(typ=-5) returns mtype 5 'five-b'",
                  n == 7 && m.mtype == 5 && strcmp(m.mtext, "five-b") == 0);
            /* Now only mtype 9 remains. */
            memset(&m, 0, sizeof(m));
            n = msgrcv(qid, &m, sizeof(m.mtext), 0, 0);
            CHECK("sysv: msgrcv(typ=0) returns mtype 9",
                  n == 5 && m.mtype == 9 && strcmp(m.mtext, "nine") == 0);
            /* Empty queue + IPC_NOWAIT -> ENOMSG. */
            errno = 0;
            CHECK("sysv: msgrcv on empty queue -> ENOMSG",
                  msgrcv(qid, &m, sizeof(m.mtext), 0, IPC_NOWAIT) == -1 &&
                  errno == ENOMSG);
            struct msqid_ds qds;
            errno = 0;
            CHECK("sysv: msgctl(IPC_STAT)", msgctl(qid, IPC_STAT, &qds) == 0);
            CHECK("sysv: msgctl(IPC_RMID)", msgctl(qid, IPC_RMID, NULL) == 0);
        }
    }
}

int main(void) {
    printf("CONFORMTEST start\n");
    test_at_tmpfs();
    test_at_fat();
    test_spawn();
    test_mqueue();
    test_mq_notify();
    test_sem();
    test_clock_nanosleep();
    test_getentropy();
    test_scandir();
    test_link();
    test_symlinkat();
    test_mknod();
    test_utimens();
    test_fdopendir();
    test_fexecve();
    test_q16();
    test_sysvipc();

    if (failures == 0) {
        printf("CONFORMTEST ALL PASS\n");
        return 0;
    }
    printf("CONFORMTEST FAILURES %d\n", failures);
    return 1;
}
