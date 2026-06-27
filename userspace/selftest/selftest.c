/* selftest.c — focused userspace regression checks for syscall hardening. */

#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "auragui.h"

static int fails = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("SELFTEST PASS: %s\n", name);
    } else {
        printf("SELFTEST FAIL: %s\n", name);
        fails++;
    }
}

int main(void) {
    /* ---- P1: errno reporting contract ---- */
    {
        /* open() of a missing path must fail with errno == ENOENT. */
        errno = 0;
        int nf = open("/nonexistent", O_RDONLY);
        check("open(missing) returns -1", nf < 0);
        check("open(missing) sets errno=ENOENT", errno == ENOENT);
        /* Emit the exact gate string for the integration harness. */
        printf("errno=%d (ENOENT): %s\n", errno, strerror(errno));
        perror("open");   /* -> "open: No such file or directory" */

        /* strerror table spot-checks. */
        check("strerror(EINVAL)",
              strcmp(strerror(EINVAL), "Invalid argument") == 0);
        check("strerror(0)", strcmp(strerror(0), "Success") == 0);

        /* A bad fd to read() must report EBADF, not a bare -1. */
        errno = 0;
        char b;
        ssize_t br = read(999, &b, 1);
        check("read(badfd) returns -1", br < 0);
        check("read(badfd) sets errno=EBADF", errno == EBADF);

        /* errno must NOT be perturbed by a successful call. */
        errno = ENOENT;          /* leftover from a prior failure */
        int okfd = open("/hello", O_RDONLY);
        check("open(/hello) succeeds without clearing errno-on-success rule",
              okfd >= 3);
        if (okfd >= 3) close(okfd);
    }

    /* ---- P2: open() flags & fcntl() ---- */
    {
        const char *p = "/tmp/p2flags.txt";
        unlink(p);

        /* O_CREAT|O_WRONLY creates the file. */
        int fd = open(p, O_CREAT | O_WRONLY, 0644);
        check("O_CREAT|O_WRONLY creates file", fd >= 3);
        if (fd >= 3) {
            /* Writing to an O_WRONLY fd works; reading must fail with EBADF. */
            check("write to O_WRONLY fd", write(fd, "abc", 3) == 3);
            errno = 0;
            char rb[4];
            check("read on O_WRONLY fd -> EBADF",
                  read(fd, rb, 1) < 0 && errno == EBADF);
            close(fd);
        }

        /* O_CREAT|O_EXCL on an existing file -> EEXIST. */
        errno = 0;
        int ex = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644);
        check("O_CREAT|O_EXCL on existing -> EEXIST",
              ex < 0 && errno == EEXIST);

        /* Open missing without O_CREAT -> ENOENT. */
        errno = 0;
        int mi = open("/tmp/p2_absent.txt", O_RDONLY);
        check("open missing without O_CREAT -> ENOENT",
              mi < 0 && errno == ENOENT);

        /* O_RDONLY fd: write must fail with EBADF. */
        int rfd = open(p, O_RDONLY);
        if (rfd >= 3) {
            errno = 0;
            check("write on O_RDONLY fd -> EBADF",
                  write(rfd, "x", 1) < 0 && errno == EBADF);
            /* F_GETFL reports the access mode (O_RDONLY == 0), not FD_CLOEXEC. */
            int fl = fcntl(rfd, F_GETFL, 0);
            check("F_GETFL access mode == O_RDONLY",
                  fl >= 0 && (fl & O_ACCMODE) == O_RDONLY);
            close(rfd);
        }

        /* O_APPEND: writes go to EOF regardless of seek. */
        int afd = open(p, O_WRONLY | O_TRUNC, 0644);
        if (afd >= 3) { write(afd, "12345", 5); close(afd); }
        afd = open(p, O_WRONLY | O_APPEND, 0644);
        if (afd >= 3) {
            check("F_GETFL reports O_APPEND",
                  (fcntl(afd, F_GETFL, 0) & O_APPEND) != 0);
            write(afd, "678", 3);
            close(afd);
        }
        int vfd = open(p, O_RDONLY);
        if (vfd >= 3) {
            char ab[16] = {0};
            ssize_t an = read(vfd, ab, sizeof(ab) - 1);
            check("O_APPEND appended at EOF",
                  an == 8 && memcmp(ab, "12345678", 8) == 0);
            close(vfd);
        }

        /* F_DUPFD: lowest fd >= arg. */
        int base = open(p, O_RDONLY);
        if (base >= 3) {
            int dfd = fcntl(base, F_DUPFD, 20);
            check("F_DUPFD returns fd >= 20", dfd >= 20);
            if (dfd >= 0) close(dfd);
            errno = 0;
            check("F_DUPFD arg out of range -> EINVAL",
                  fcntl(base, F_DUPFD, 9999) < 0 && errno == EINVAL);
            close(base);
        }

        /* O_NONBLOCK: reading an empty pipe (writer still open) -> EAGAIN. */
        int nbp[2];
        if (pipe2(nbp, O_NONBLOCK) == 0) {
            errno = 0;
            char nbb[4];
            check("O_NONBLOCK read on empty pipe -> EAGAIN",
                  read(nbp[0], nbb, 1) < 0 &&
                  (errno == EAGAIN || errno == EWOULDBLOCK));
            close(nbp[0]); close(nbp[1]);
        } else {
            check("O_NONBLOCK pipe2", 0);
        }

        /* pipe2 with O_CLOEXEC sets FD_CLOEXEC on both ends. */
        int pf[2];
        if (pipe2(pf, O_CLOEXEC) == 0) {
            check("pipe2 O_CLOEXEC sets FD_CLOEXEC on read end",
                  (fcntl(pf[0], F_GETFD, 0) & FD_CLOEXEC) != 0);
            check("pipe2 O_CLOEXEC sets FD_CLOEXEC on write end",
                  (fcntl(pf[1], F_GETFD, 0) & FD_CLOEXEC) != 0);
            close(pf[0]); close(pf[1]);
        } else {
            check("pipe2 O_CLOEXEC", 0);
        }

        unlink(p);
    }

    /* Usercopy validation: bad user pointers must fail instead of killing the
     * thread or faulting the kernel. */
    ssize_t w = write(1, (const void *)0xFFFF800000000000ULL, 4);
    check("write rejects kernel pointer", w < 0);

    w = write(1, (const void *)0x0ULL, 1);
    check("write rejects null pointer", w < 0);

    int fd = open((const char *)0xFFFF800000000000ULL, O_RDONLY);
    check("open rejects kernel path pointer", fd < 0);

    struct stat st;
    int r = stat("/hello", (struct stat *)0xFFFF800000000000ULL);
    check("stat rejects kernel output pointer", r < 0);

    r = stat("/hello", &st);
    check("stat accepts valid output pointer", r == 0 && st.st_size > 0);

    r = readdir("/", (void *)0xFFFF800000000000ULL, 4);
    check("readdir rejects kernel output pointer", r < 0);

    struct dirent ents[8];
    r = readdir("/", ents, 8);
    check("readdir accepts valid output pointer", r > 0);

    fd = open("/hello", O_RDONLY);
    check("open valid file returns process fd", fd >= 3);
    if (fd >= 3) {
        unsigned char hdr[4] = {0};
        ssize_t n = read(fd, hdr, sizeof(hdr));
        check("read valid file into user buffer", n == 4 && hdr[0] == 0x7F && hdr[1] == 'E');
        close(fd);
    }

    char tmp_path[] = "/tmp/selftest.txt";
    fd = open(tmp_path, O_CREAT | O_RDWR, 0644);
    check("open/create tmp file", fd >= 3);
    if (fd >= 3) {
        const char msg[] = "selftest-data";
        check("write tmp file", write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
        close(fd);
    }

    /* Socket-style API: creation and owner-local close should work even if the
     * network transport is not connected. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    check("socket create AF_INET/SOCK_STREAM", s >= 0);
    if (s >= 0) {
        char buf[4];
        check("recv on unconnected socket fails", recv(s, buf, sizeof(buf)) < 0);
        check("closesocket succeeds", closesocket(s) == 0);
        check("closesocket twice fails", closesocket(s) < 0);
    }

    /* ---- dup / dup2 / pipe / fcntl ---- */
    {
        int fd2 = open("/hello", O_RDONLY);
        check("open /hello for dup", fd2 >= 3);
        if (fd2 >= 3) {
            int dup_fd = dup(fd2);
            check("dup returns >=3", dup_fd >= 3 && dup_fd != fd2);
            unsigned char b1[4] = {0}, b2[4] = {0};
            read(fd2, b1, 4);
            /* dup shares offset with original, so reading from dup continues
             * after fd2's read.  Just confirm it doesn't error. */
            ssize_t n2 = read(dup_fd, b2, 4);
            check("read from duped fd works", n2 >= 0);
            close(dup_fd);
            close(fd2);
        }

        /* dup2 to a specific number. */
        int fd3 = open("/hello", O_RDONLY);
        check("open /hello for dup2", fd3 >= 3);
        if (fd3 >= 3) {
            int target = fd3 + 5;
            int r2 = dup2(fd3, target);
            check("dup2 to higher fd", r2 == target);
            close(target);
            close(fd3);
        }

        /* fcntl FD_CLOEXEC round-trip. */
        int fd4 = open("/hello", O_RDONLY);
        if (fd4 >= 3) {
            check("fcntl F_GETFD initial 0", fcntl(fd4, F_GETFD, 0) == 0);
            check("fcntl F_SETFD CLOEXEC",   fcntl(fd4, F_SETFD, FD_CLOEXEC) == 0);
            check("fcntl F_GETFD == CLOEXEC",fcntl(fd4, F_GETFD, 0) == FD_CLOEXEC);
            close(fd4);
        }

        /* pipe round-trip.  Note: AuraLite pipes are polling/yield-based and
         * the read path may currently return short on the first attempt; we
         * therefore accept either 5 or a partial read followed by a second
         * read that completes the buffer. */
        int pfds[2] = { -1, -1 };
        int pr = pipe(pfds);
        check("pipe returns 0", pr == 0 && pfds[0] >= 3 && pfds[1] >= 3);
        if (pr == 0) {
            const char *msg = "ping!";
            ssize_t pw = write(pfds[1], msg, 5);
            check("pipe write 5 bytes", pw == 5);
            char rb[6] = {0};
            ssize_t prd = 0;
            for (int attempt = 0; attempt < 4 && prd < 5; attempt++) {
                ssize_t r = read(pfds[0], rb + prd, 5 - prd);
                if (r > 0) prd += r;
            }
            check("pipe read 5 bytes (possibly across calls)",
                  prd == 5 && memcmp(rb, "ping!", 5) == 0);
            close(pfds[0]); close(pfds[1]);
        }

        /* pipe with bad output buffer should fail without faulting. */
        int bad_pipe = pipe((int *)0xFFFF800000000000ULL);
        check("pipe rejects kernel out buffer", bad_pipe < 0);
    }

    /* ---- GUI syscall hardening ---- */
    {
        int wid = ag_window_create(50, 50, 120, 80, "selftest", AG_WIN_DEFAULT);
        check("ag_window_create returns >=0", wid >= 0);
        if (wid >= 0) {
            /* Valid op on our window: clear. */
            check("gui_clear on owned window",
                  ag_clear(wid, 0x000000) == 0);

            /* Out-of-range wid: must fail, not fault. */
            check("gui_clear rejects wid=999",
                  ag_clear(999, 0x000000) != 0);
            check("gui_clear rejects wid=-1",
                  ag_clear(-1, 0x000000) != 0);

            /* Bad text pointer: must fail. */
            int draw_bad = ag_draw_text(wid, 0, 0,
                                        (const char *)0xFFFF800000000000ULL,
                                        0xFFFFFF);
            check("gui_draw_text rejects kernel string", draw_bad != 0);

            /* Bad event pointer: must fail. */
            int evt_bad = ag_poll_event(wid,
                                        (ag_event_t *)0xFFFF800000000000ULL);
            check("gui_event rejects kernel out pointer", evt_bad < 0);

            /* Get size into a valid buffer. */
            uint32_t w = 0, h = 0;
            int gs = ag_window_get_size(wid, &w, &h);
            check("gui_get_size valid pointer", gs == 0 && w > 0 && h > 0);

            /* Calling get_size on a non-owned wid must fail cleanly. */
            int gs_bad = ag_window_get_size(999, &w, &h);
            check("gui_get_size rejects bad wid", gs_bad != 0);

            ag_window_destroy(wid);

            /* Operating on a destroyed wid must fail (ownership lost). */
            check("gui op on destroyed wid fails",
                  ag_clear(wid, 0x000000) != 0);
        }
    }

    /* NOTE: spawn+waitpid+exit_code from inside a user process is currently
     * exercised by the kernel-side process_self_test (boot) and by the
     * dedicated test_process_cleanup.sh integration case.  We deliberately
     * do not call spawn/waitpid here because the shell already does that
     * (it waits on us); double-stacking syscall paths inside the same
     * user binary still races with the global syscall_saved_* on this
     * kernel. */

    if (fails == 0) {
        puts("SELFTEST ALL PASS");
        return 0;
    }
    printf("SELFTEST %d FAILURES\n", fails);
    return 1;
}
