/*
 * test_tcp_m6e.c — host unit tests for M6's last three items:
 * listen backlog, SO_REUSEADDR, keepalive.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp_m6e.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* -------------------------------------------------------- backlog ---- */

/* 1: FIFO order -- the peer that waited longest is served first. */
static void t_backlog_fifo(void) {
    tcpm6e_backlog_t b;
    tcpm6e_backlog_init(&b, 4);

    CHECK_EQ(tcpm6e_backlog_push(&b, 0x0A000001, 1111, 100), 0);
    CHECK_EQ(tcpm6e_backlog_push(&b, 0x0A000002, 2222, 200), 0);
    CHECK_EQ(b.count, 2);

    tcpm6e_pending_t *p = tcpm6e_backlog_pop(&b);
    CHECK(p != 0);
    CHECK_EQ(p ? p->peer_port : 0, 1111);
    p = tcpm6e_backlog_pop(&b);
    CHECK_EQ(p ? p->peer_port : 0, 2222);
    CHECK(tcpm6e_backlog_pop(&b) == 0);      /* empty */
}

/* 2: the queue honours the requested limit and counts what it drops. */
static void t_backlog_limit(void) {
    tcpm6e_backlog_t b;
    tcpm6e_backlog_init(&b, 2);

    CHECK_EQ(tcpm6e_backlog_push(&b, 1, 1000, 1), 0);
    CHECK_EQ(tcpm6e_backlog_push(&b, 2, 1001, 2), 0);
    CHECK(tcpm6e_backlog_full(&b));
    CHECK_EQ(tcpm6e_backlog_push(&b, 3, 1002, 3), -1);
    CHECK_EQ(b.dropped, 1);
    CHECK_EQ(b.count, 2);

    /* Accepting one frees exactly one slot. */
    tcpm6e_backlog_pop(&b);
    CHECK_EQ(tcpm6e_backlog_push(&b, 3, 1002, 3), 0);
}

/* 3: listen(fd, 0) means one, and the ceiling is enforced. */
static void t_backlog_limit_clamp(void) {
    tcpm6e_backlog_t b;
    tcpm6e_backlog_init(&b, 0);
    CHECK_EQ(b.limit, 1);

    tcpm6e_backlog_init(&b, 9999);
    CHECK_EQ(b.limit, TCPM6E_BACKLOG_MAX);
}

/*
 * 4: a retransmitted SYN must not take a second slot.  A peer whose first
 * SYN was dropped retransmits -- without this one client fills the queue.
 */
static void t_backlog_duplicate_syn(void) {
    tcpm6e_backlog_t b;
    tcpm6e_backlog_init(&b, 4);
    tcpm6e_backlog_push(&b, 0x0A000001, 1111, 100);

    CHECK(tcpm6e_backlog_has(&b, 0x0A000001, 1111));
    CHECK(!tcpm6e_backlog_has(&b, 0x0A000001, 2222));   /* other port */
    CHECK(!tcpm6e_backlog_has(&b, 0x0A000002, 1111));   /* other host */

    /* After accept() the entry is gone and a fresh SYN is new again. */
    tcpm6e_backlog_pop(&b);
    CHECK(!tcpm6e_backlog_has(&b, 0x0A000001, 1111));
}

/* 5: the ring wraps without losing order. */
static void t_backlog_wrap(void) {
    tcpm6e_backlog_t b;
    tcpm6e_backlog_init(&b, TCPM6E_BACKLOG_MAX);

    for (uint32_t lap = 0; lap < 3 * TCPM6E_BACKLOG_MAX; lap++) {
        CHECK_EQ(tcpm6e_backlog_push(&b, 0x0A000000 + lap,
                                     (uint16_t)(1000 + lap), lap), 0);
        tcpm6e_pending_t *p = tcpm6e_backlog_pop(&b);
        CHECK_EQ(p ? p->peer_port : 0, (uint16_t)(1000 + lap));
    }
    CHECK_EQ(b.count, 0);
}

/* ---------------------------------------------------- SO_REUSEADDR --- */

/*
 * 6: the bug this phase found.  A port held by a LIVE listener must be
 * refused -- before M6e tcp_listen() checked nothing, so two listens on
 * one port both "succeeded" and whichever polled first stole the SYN.
 */
static void t_bind_live_listener_refused(void) {
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_LISTENING, 0), TCPM6E_BIND_IN_USE);
    /* SO_REUSEADDR does NOT permit two live listeners (that is
     * SO_REUSEPORT, a different option, not implemented). */
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_LISTENING, 1), TCPM6E_BIND_IN_USE);
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_ACTIVE, 1), TCPM6E_BIND_IN_USE);
}

/* 7: TIME_WAIT is exactly the case SO_REUSEADDR exists for. */
static void t_bind_time_wait(void) {
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_TIME_WAIT, 0), TCPM6E_BIND_IN_USE);
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_TIME_WAIT, 1), TCPM6E_BIND_OK);
}

/* 8: a free port is always available. */
static void t_bind_free(void) {
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_FREE, 0), TCPM6E_BIND_OK);
    CHECK_EQ(tcpm6e_can_bind(TCPM6E_ST_FREE, 1), TCPM6E_BIND_OK);
}

/* ------------------------------------------------------- keepalive --- */

/* 9: disabled keepalive never fires, however long the idle. */
static void t_ka_disabled(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 0);
    CHECK_EQ(k.enabled, 0);
    CHECK_EQ(tcpm6e_ka_poll(&k, 100000000u, 10), TCPM6E_KA_IDLE_STILL);
}

/* 10: the RFC 1122 default idle is two hours, not something convenient. */
static void t_ka_defaults(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 0);
    CHECK_EQ(k.idle_ms, 7200000u);      /* 2 hours */
    CHECK_EQ(k.intvl_ms, 75000u);
    CHECK_EQ(k.count, 9u);
}

/* 11: a probe goes out only after the idle time, and not before. */
static void t_ka_idle_threshold(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 1000);
    k.enabled = 1;
    k.idle_ms = 1000;                    /* 1 s, 10 ms per tick = 100 ticks */

    CHECK_EQ(tcpm6e_ka_poll(&k, 1099, 10), TCPM6E_KA_IDLE_STILL);  /* 990ms */
    CHECK_EQ(tcpm6e_ka_poll(&k, 1100, 10), TCPM6E_KA_SEND_PROBE);  /* 1000ms */
}

/* 12: traffic resets the idle timer. */
static void t_ka_activity_resets(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 1000);
    k.enabled = 1;
    k.idle_ms = 1000;

    CHECK_EQ(tcpm6e_ka_poll(&k, 1100, 10), TCPM6E_KA_SEND_PROBE);
    tcpm6e_ka_activity(&k, 1100);
    CHECK_EQ(tcpm6e_ka_poll(&k, 1100, 10), TCPM6E_KA_IDLE_STILL);
    CHECK_EQ(tcpm6e_ka_poll(&k, 1200, 10), TCPM6E_KA_SEND_PROBE);
}

/* 13: probes space out by intvl, and the run ends in DEAD. */
static void t_ka_probe_run(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 0);
    k.enabled = 1;
    k.idle_ms = 100;        /* 10 ticks */
    k.intvl_ms = 100;
    k.count = 3;

    uint32_t now = 10;
    CHECK_EQ(tcpm6e_ka_poll(&k, now, 10), TCPM6E_KA_SEND_PROBE);
    tcpm6e_ka_probe_sent(&k, now);

    /* Too soon for the next one. */
    CHECK_EQ(tcpm6e_ka_poll(&k, now + 5, 10), TCPM6E_KA_IDLE_STILL);
    /* Interval elapsed. */
    now += 10;
    CHECK_EQ(tcpm6e_ka_poll(&k, now, 10), TCPM6E_KA_SEND_PROBE);
    tcpm6e_ka_probe_sent(&k, now);
    now += 10;
    CHECK_EQ(tcpm6e_ka_poll(&k, now, 10), TCPM6E_KA_SEND_PROBE);
    tcpm6e_ka_probe_sent(&k, now);

    /* Three sent, none answered: dead. */
    CHECK_EQ(k.probes_sent, 3);
    CHECK_EQ(tcpm6e_ka_poll(&k, now + 100, 10), TCPM6E_KA_DEAD);
}

/* 14: an answer mid-run aborts it -- the connection is alive after all. */
static void t_ka_answer_aborts_run(void) {
    tcpm6e_ka_t k;
    tcpm6e_ka_init(&k, 0);
    k.enabled = 1;
    k.idle_ms = 100;
    k.intvl_ms = 100;
    k.count = 3;

    tcpm6e_ka_poll(&k, 10, 10);
    tcpm6e_ka_probe_sent(&k, 10);
    CHECK_EQ(k.probes_sent, 1);

    tcpm6e_ka_activity(&k, 12);          /* the peer replied */
    CHECK_EQ(k.probes_sent, 0);
    CHECK_EQ(tcpm6e_ka_poll(&k, 12, 10), TCPM6E_KA_IDLE_STILL);
}

int main(void) {
    printf("test_tcp_m6e (backlog, SO_REUSEADDR, keepalive)\n");

    RUN(t_backlog_fifo);
    RUN(t_backlog_limit);
    RUN(t_backlog_limit_clamp);
    RUN(t_backlog_duplicate_syn);
    RUN(t_backlog_wrap);
    RUN(t_bind_live_listener_refused);
    RUN(t_bind_time_wait);
    RUN(t_bind_free);
    RUN(t_ka_disabled);
    RUN(t_ka_defaults);
    RUN(t_ka_idle_threshold);
    RUN(t_ka_activity_resets);
    RUN(t_ka_probe_run);
    RUN(t_ka_answer_aborts_run);

    printf("%s: %d/%d test(s) passed\n",
           failed ? "FAILURES" : "ALL PASS", passed, tn);
    return failed ? 1 : 0;
}
