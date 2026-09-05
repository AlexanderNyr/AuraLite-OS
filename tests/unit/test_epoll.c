/*
 * test_epoll.c — host-side unit tests for the select()-backed epoll triple
 * (RESIDUE2 T4, lib/libc/src/epoll.c).
 *
 * The shipping source is compiled in directly.  The test is written against
 * the GUEST headers (compiled with -I lib/libc/include) and linked against
 * the HOST libc's select()/usleep()/pipe(), which the implementation calls:
 * if the level-triggered contract holds against glibc's select, it holds
 * against the kernel's — both are polled the same way per wait.
 *
 * Pipes give the full readiness story without a network: an empty read end
 * is not ready, a written byte makes it ready (and KEEPS it ready until
 * drained — the level-triggered rule), and a write end is always writable.
 */

#include "sys/epoll.h"
#include "sys/select.h"
#include "stdio.h"
#include "errno.h"
#include "string.h"
#include "unistd.h"    /* pipe/read/write via the host libc at link time */
#include "time.h"      /* gettimeofday: wall clock for the sleep bounds */

/* Wall clock for the sleep-bounded cases.  Uses the same guest header
 * shape the implementation sees; glibc provides the symbol at link time. */
static long long time_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

static int t_create_and_ctl(void) {
    int ep = epoll_create1(0);
    CHECK(ep >= 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = {.fd = 7} };
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    /* Double add: EEXIST. */
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == -1);
    CHECK(errno == EEXIST);
    /* MOD on a registered fd, DEL then MOD: ENOENT. */
    ev.events = EPOLLIN | EPOLLOUT;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == -1);
    CHECK(errno == ENOENT);
    /* Unknown op: EINVAL. */
    CHECK(epoll_ctl(ep, 99, p[0], &ev) == -1);
    CHECK(errno == EINVAL);
    /* Bad instance: EBADF. */
    CHECK(epoll_ctl(12345, EPOLL_CTL_ADD, p[0], &ev) == -1);
    CHECK(errno == EBADF);
    close(p[0]); close(p[1]);
    return 1;
}

static int t_level_triggered_pipe(void) {
    int ep = epoll_create(1);          /* size ignored */
    CHECK(ep >= 0);
    int p[2];
    CHECK(pipe(p) == 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = {.u32 = 42} };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    struct epoll_event out[4];

    /* Empty pipe: timeout 0 polls, nothing ready. */
    CHECK(epoll_wait(ep, out, 4, 0) == 0);

    /* One byte in: ready, with the cookie. */
    char b = 'x';
    CHECK(write(p[1], &b, 1) == 1);
    int n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    CHECK(out[0].events & EPOLLIN);
    CHECK(out[0].data.u32 == 42);

    /* STILL ready without a new write — level-triggered. */
    CHECK(epoll_wait(ep, out, 4, 0) == 1);

    /* Drained: not ready again. */
    CHECK(read(p[0], &b, 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);

    close(p[0]); close(p[1]);
    return 1;
}

static int t_writable_and_maxevents(void) {
    int ep = epoll_create1(0);
    int p[2];
    CHECK(pipe(p) == 0);
    struct epoll_event ev = { .events = EPOLLOUT, .data = {.fd = p[1]} };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev) == 0);
    struct epoll_event out[4];

    /* A fresh pipe write end is writable. */
    int n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1);
    CHECK(out[0].events & EPOLLOUT);

    /* maxevents caps the report. */
    CHECK(epoll_wait(ep, out, 1, 0) == 1);

    /* maxevents <= 0 is EINVAL. */
    CHECK(epoll_wait(ep, out, 0, 0) == -1);
    CHECK(errno == EINVAL);

    close(p[0]); close(p[1]);
    return 1;
}

static int t_oneshot(void) {
    int ep = epoll_create1(0);
    int p[2];
    CHECK(pipe(p) == 0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT, .data = {.fd = p[0]} };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    struct epoll_event out[4];

    char b = 'y';
    CHECK(write(p[1], &b, 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 1);          /* reported once... */
    CHECK(epoll_wait(ep, out, 4, 0) == 0);          /* ...then disarmed */
    CHECK(read(p[0], &b, 1) == 1);

    /* Re-armed by MOD: fires again on the next byte. */
    ev.events = EPOLLIN | EPOLLONESHOT;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
    CHECK(write(p[1], &b, 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 1);

    close(p[0]); close(p[1]);
    return 1;
}

static int t_timeout_sleeps(void) {
    /* A positive timeout on an idle interest actually waits (bounded
     * check: >= 90 ms of wall clock for a 100 ms ask). */
    int ep = epoll_create1(0);
    int p[2];
    CHECK(pipe(p) == 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = {.fd = p[0]} };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    struct epoll_event out[2];

    unsigned long long t0 = (unsigned long long)time_now_ms();
    CHECK(epoll_wait(ep, out, 2, 100) == 0);
    unsigned long long dt = (unsigned long long)time_now_ms() - t0;
    CHECK(dt >= 90);

    close(p[0]); close(p[1]);
    return 1;
}

static int t_empty_set_and_flags(void) {
    /* Unknown create1 flags: EINVAL. */
    CHECK(epoll_create1(0x4242) == -1);
    CHECK(errno == EINVAL);
    /* EPOLL_CLOEXEC is accepted (and recorded — no-op at this layer). */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event out[2];
    /* Empty interest + 30 ms timeout: sleeps, returns 0. */
    unsigned long long t0 = (unsigned long long)time_now_ms();
    CHECK(epoll_wait(ep, out, 2, 30) == 0);
    unsigned long long dt = (unsigned long long)time_now_ms() - t0;
    CHECK(dt >= 25);
    /* Wait on a bad instance: EBADF. */
    CHECK(epoll_wait(999, out, 2, 0) == -1);
    CHECK(errno == EBADF);
    return 1;
}

int main(void) {
    printf("test_epoll: select()-backed epoll triple (T4)\n");

    RUN(t_create_and_ctl);
    RUN(t_level_triggered_pipe);
    RUN(t_writable_and_maxevents);
    RUN(t_oneshot);
    RUN(t_timeout_sleeps);
    RUN(t_empty_set_and_flags);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
