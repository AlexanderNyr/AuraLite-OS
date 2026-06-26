/* selftest.c — focused userspace regression checks for syscall hardening. */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
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
    /* Usercopy validation: bad user pointers must fail instead of killing the
     * thread or faulting the kernel. */
    ssize_t w = write(1, (const void *)0xFFFF800000000000ULL, 4);
    check("write rejects kernel pointer", w < 0);

    w = write(1, (const void *)0x0ULL, 1);
    check("write rejects null pointer", w < 0);

    int fd = open((const char *)0xFFFF800000000000ULL);
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

    fd = open("/hello");
    check("open valid file returns process fd", fd >= 3);
    if (fd >= 3) {
        unsigned char hdr[4] = {0};
        ssize_t n = read(fd, hdr, sizeof(hdr));
        check("read valid file into user buffer", n == 4 && hdr[0] == 0x7F && hdr[1] == 'E');
        close(fd);
    }

    char tmp_path[] = "/tmp/selftest.txt";
    fd = open(tmp_path);
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
        int fd2 = open("/hello");
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
        int fd3 = open("/hello");
        check("open /hello for dup2", fd3 >= 3);
        if (fd3 >= 3) {
            int target = fd3 + 5;
            int r2 = dup2(fd3, target);
            check("dup2 to higher fd", r2 == target);
            close(target);
            close(fd3);
        }

        /* fcntl FD_CLOEXEC round-trip. */
        int fd4 = open("/hello");
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
