/*
 * test_tcp_m6.c — host unit tests for MATURITY_PLAN.md M6 policy.
 *
 * Covers the four things AUDIT_A3 established were genuinely missing:
 * duplicate-ACK counting with fast retransmit/recovery, Nagle, delayed
 * ACK, and the TIME_WAIT timer.  Pure integer policy, no NIC required.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp_m6.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---------------------------------------------- fast retransmit ------ */

/* 1: three duplicate ACKs trigger fast retransmit -- and not two. */
static void t_dupack_threshold(void) {
    tcpm6_dupack_t d;
    tcpm6_dupack_init(&d);
    uint32_t una = 1000, nxt = 5000;

    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, nxt, 0, 0), TCPM6_ACK_DUP);
    CHECK_EQ(d.dup_count, 1);
    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, nxt, 0, 0), TCPM6_ACK_DUP);
    CHECK_EQ(d.dup_count, 2);
    /* The third is the trigger. */
    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, nxt, 0, 0), TCPM6_ACK_FAST_RETX);
    CHECK(d.in_recovery);
    CHECK_EQ(d.recover, nxt);
}

/* 2: an ACK that advances the window is progress and clears the count. */
static void t_dupack_progress_resets(void) {
    tcpm6_dupack_t d;
    tcpm6_dupack_init(&d);
    uint32_t una = 1000, nxt = 5000;

    tcpm6_on_ack(&d, una, 1000, nxt, 0, 0);
    tcpm6_on_ack(&d, una, 1000, nxt, 0, 0);
    CHECK_EQ(d.dup_count, 2);
    CHECK_EQ(tcpm6_on_ack(&d, una, 2000, nxt, 0, 0), TCPM6_ACK_PROGRESS);
    CHECK_EQ(d.dup_count, 0);
}

/*
 * 3: the discriminator that stops a stalled-but-chatty peer from being
 * mistaken for loss.  A repeated ACK number that carries data, or that
 * moves the window, is NOT a duplicate ACK (RFC 5681 s2).  Getting this
 * wrong makes a stack retransmit into a receiver that is merely slow.
 */
static void t_dupack_requires_no_data_no_winupdate(void) {
    tcpm6_dupack_t d;
    tcpm6_dupack_init(&d);
    uint32_t una = 1000, nxt = 5000;

    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, nxt, 1, 0), TCPM6_ACK_OLD);
    CHECK_EQ(d.dup_count, 0);
    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, nxt, 0, 1), TCPM6_ACK_OLD);
    CHECK_EQ(d.dup_count, 0);

    /* Nothing in flight => a repeated ACK means nothing either. */
    CHECK_EQ(tcpm6_on_ack(&d, una, 1000, una, 0, 0), TCPM6_ACK_OLD);
    CHECK_EQ(d.dup_count, 0);
}

/* 4: RFC 5681 3.2 window arithmetic on entering recovery. */
static void t_recovery_windows(void) {
    /* FlightSize 10000, MSS 1460 => ssthresh = 5000. */
    CHECK_EQ(tcpm6_recovery_ssthresh(10000, 1460), 5000);
    /* Floor at 2*MSS when FlightSize is tiny. */
    CHECK_EQ(tcpm6_recovery_ssthresh(1000, 1460), 2920);
    /* cwnd = ssthresh + 3*MSS at the moment of entry. */
    CHECK_EQ(tcpm6_recovery_cwnd(5000, 1460, 3), 5000 + 3 * 1460);
    /* Each further duplicate inflates by one more segment. */
    CHECK_EQ(tcpm6_recovery_cwnd(5000, 1460, 5), 5000 + 5 * 1460);
}

/* 5: recovery ends only once the recovery point is acknowledged. */
static void t_recovery_exit(void) {
    tcpm6_dupack_t d;
    tcpm6_dupack_init(&d);
    uint32_t una = 1000, nxt = 5000;

    tcpm6_on_ack(&d, una, 1000, nxt, 0, 0);
    tcpm6_on_ack(&d, una, 1000, nxt, 0, 0);
    tcpm6_on_ack(&d, una, 1000, nxt, 0, 0);
    CHECK(d.in_recovery);

    /* A partial ACK (below `recover`) does not end recovery. */
    CHECK_EQ(tcpm6_on_ack(&d, una, 3000, nxt, 0, 0), TCPM6_ACK_PROGRESS);
    CHECK(d.in_recovery);

    /* Reaching the recovery point does. */
    CHECK_EQ(tcpm6_on_ack(&d, 3000, 5000, nxt, 0, 0), TCPM6_ACK_PROGRESS);
    CHECK(!d.in_recovery);
}

/* ------------------------------------------------------- Nagle ------- */

/* 6: a small segment waits while data is unacknowledged. */
static void t_nagle_holds_small(void) {
    /* 100 bytes, 2000 in flight => hold. */
    CHECK_EQ(tcpm6_nagle_may_send(100, 1460, 1000, 3000, 0, 0), 0);
    /* Nothing in flight => send even though it is small. */
    CHECK_EQ(tcpm6_nagle_may_send(100, 1460, 1000, 1000, 0, 0), 1);
    /* Full segment => always. */
    CHECK_EQ(tcpm6_nagle_may_send(1460, 1460, 1000, 3000, 0, 0), 1);
    /* TCP_NODELAY overrides. */
    CHECK_EQ(tcpm6_nagle_may_send(100, 1460, 1000, 3000, 1, 0), 1);
    /* An explicit flush (close, PSH) overrides. */
    CHECK_EQ(tcpm6_nagle_may_send(100, 1460, 1000, 3000, 0, 1), 1);
    /* Nothing to send is never a send. */
    CHECK_EQ(tcpm6_nagle_may_send(0, 1460, 1000, 1000, 1, 1), 0);
}

/* -------------------------------------------------- delayed ACK ------ */

/* 7: every second segment is acknowledged (RFC 1122 4.2.3.2). */
static void t_delack_every_second_segment(void) {
    tcpm6_delack_t a;
    tcpm6_delack_init(&a);

    CHECK_EQ(tcpm6_delack_on_segment(&a, 100, 500, 1460, 0, 0), 0);
    CHECK_EQ(a.pending_segs, 1);
    CHECK_EQ(tcpm6_delack_on_segment(&a, 101, 500, 1460, 0, 0), 1);
    CHECK_EQ(a.pending_segs, 0);
}

/* 8: PSH, out-of-order and full-sized segments are acknowledged at once. */
static void t_delack_immediate_cases(void) {
    tcpm6_delack_t a;

    tcpm6_delack_init(&a);
    CHECK_EQ(tcpm6_delack_on_segment(&a, 100, 500, 1460, 1, 0), 1);  /* PSH */

    tcpm6_delack_init(&a);
    CHECK_EQ(tcpm6_delack_on_segment(&a, 100, 500, 1460, 0, 1), 1);  /* OOO */

    tcpm6_delack_init(&a);
    CHECK_EQ(tcpm6_delack_on_segment(&a, 100, 1460, 1460, 0, 0), 1); /* full */

    /* A pure ACK owes nothing. */
    tcpm6_delack_init(&a);
    CHECK_EQ(tcpm6_delack_on_segment(&a, 100, 0, 1460, 0, 0), 0);
}

/* 9: the withheld ACK ages out at 200 ms and not before. */
static void t_delack_timer(void) {
    tcpm6_delack_t a;
    tcpm6_delack_init(&a);

    /* Nothing pending: never expired. */
    CHECK_EQ(tcpm6_delack_expired(&a, 1000, 10), 0);

    tcpm6_delack_on_segment(&a, 100, 500, 1460, 0, 0);
    CHECK_EQ(tcpm6_delack_expired(&a, 105, 10), 0);   /* 50 ms */
    CHECK_EQ(tcpm6_delack_expired(&a, 119, 10), 0);   /* 190 ms */
    CHECK_EQ(tcpm6_delack_expired(&a, 120, 10), 1);   /* 200 ms */
}

/* ---------------------------------------------------- TIME_WAIT ------ */

/* 10: 2*MSL must elapse before the tuple is reusable. */
static void t_time_wait(void) {
    /* 10 ms per tick => 30 s = 3000 ticks. */
    CHECK_EQ(tcpm6_time_wait_expired(1000, 1000 + 2999, 10), 0);
    CHECK_EQ(tcpm6_time_wait_expired(1000, 1000 + 3000, 10), 1);
    CHECK_EQ(TCPM6_TIME_WAIT_MS, 30000);
}

int main(void) {
    printf("test_tcp_m6 (MATURITY_PLAN M6 policy)\n");

    RUN(t_dupack_threshold);
    RUN(t_dupack_progress_resets);
    RUN(t_dupack_requires_no_data_no_winupdate);
    RUN(t_recovery_windows);
    RUN(t_recovery_exit);
    RUN(t_nagle_holds_small);
    RUN(t_delack_every_second_segment);
    RUN(t_delack_immediate_cases);
    RUN(t_delack_timer);
    RUN(t_time_wait);

    printf("%s: %d/%d test(s) passed\n",
           failed ? "FAILURES" : "ALL PASS", passed, tn);
    return failed ? 1 : 0;
}
