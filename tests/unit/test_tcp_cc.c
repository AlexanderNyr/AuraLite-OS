/* test_tcp_cc.c — host gate for the Y1 congestion-control arithmetic
 * (REALINTERNET2 Y1; kernel/net/tcp_cc.h).
 *
 * The D2 rationale: QEMU/SLIRP cannot manufacture loss and reordering
 * on demand, so the GROWTH arithmetic is proven here, deterministically
 * — the guest lanes then assert receipts (the tcp_cwnd_limited_sends
 * counter moving on the 1 MiB x5 upload), not timing.
 *
 * What is pinned:
 *   - the RFC 6928 initial window across the PMTUD ladder's MSS range;
 *   - slow start doubles per RTT and is ACK-byte-clamped (ABC L=1);
 *   - the SS→CA crossover at ssthresh, and CA's ~one-MSS-per-RTT rate;
 *   - the RTO collapse (loss window) and its unified ssthresh partner
 *     (max(flight/2, 2*SMSS) — the m6 helper both signals share);
 *   - the cap and the wrap guard.
 */
#include <stdint.h>
#include <stdio.h>

#include "../../kernel/net/tcp_cc.h"
#include "../../kernel/net/tcp_m6.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

#define MSS   1460u
#define CAP   64240u   /* TCP_WINDOW */

static void test_iw(void) {
    CHECK(tcpcc_iw(1460) == 14600, "IW(1460) = 10*MSS = 14600");
    CHECK(tcpcc_iw(1200) == 12000, "IW(1200) = 12000");
    CHECK(tcpcc_iw(536) == 5360,  "IW(536) = 5360 (ladder floor)");
    CHECK(tcpcc_iw(2000) == 14600, "IW(2000) capped at 14600");
    CHECK(tcpcc_iw(1460) < CAP, "IW starts below the cap: slow start RUNS");
}

/* Feed one "RTT" of full-MSS ACKs: cwnd/MSS acks, each acking MSS. */
static uint32_t one_rtt_ss(uint32_t cwnd, uint32_t ssthresh) {
    uint32_t acks = cwnd / MSS;
    for (uint32_t i = 0; i < acks; i++) {
        cwnd = tcpcc_ack_grow(cwnd, ssthresh, MSS, MSS, CAP);
    }
    return cwnd;
}

static void test_slow_start(void) {
    uint32_t cwnd = tcpcc_iw(MSS);           /* 14600 */
    uint32_t ssthresh = CAP;                 /* "arbitrarily high" */

    uint32_t after = one_rtt_ss(cwnd, ssthresh);
    CHECK(after == 2 * cwnd,
          "slow start doubles per RTT (14600 -> %u, want 29200)", after);

    /* ABC L=1: a 1-byte ACK grows cwnd by 1 byte, not an MSS. */
    CHECK(tcpcc_ack_grow(14600, ssthresh, MSS, 1, CAP) == 14601,
          "ABC: tiny ACK grows by its byte count, not a full MSS");
    /* And a jumbo cumulative ACK is clamped to one MSS of growth. */
    CHECK(tcpcc_ack_grow(14600, ssthresh, MSS, 8 * MSS, CAP)
          == 14600 + MSS,
          "ABC L=1: giant cumulative ACK still grows by one MSS");
}

static void test_crossover_and_ca(void) {
    uint32_t ssthresh = 29200;               /* 20*MSS */
    /* Below ssthresh: exponential.  At/above: linear. */
    CHECK(tcpcc_ack_grow(29199, ssthresh, MSS, MSS, CAP) == 29199 + MSS,
          "one byte below ssthresh still slow-starts");
    uint32_t ca = tcpcc_ack_grow(29200, ssthresh, MSS, MSS, CAP);
    CHECK(ca == 29200 + (MSS * MSS) / 29200,
          "at ssthresh the growth is MSS^2/cwnd (got +%u)", ca - 29200);

    /* One RTT of CA grows ~one MSS total. */
    uint32_t cwnd = 29200, grown = cwnd;
    uint32_t acks = cwnd / MSS;
    for (uint32_t i = 0; i < acks; i++)
        grown = tcpcc_ack_grow(grown, ssthresh, MSS, MSS, CAP);
    uint32_t rtt_gain = grown - cwnd;
    CHECK(rtt_gain >= MSS - 80 && rtt_gain <= MSS + 80,
          "CA gains ~1 MSS per RTT (got %u)", rtt_gain);

    /* Giant cwnd: the max(1,·) keeps growth from stalling at zero. */
    CHECK(tcpcc_ack_grow(4000000000u, 1000u, MSS, MSS, 4294967295u)
          > 4000000000u,
          "CA never rounds to a full stall at giant cwnd");
}

static void test_loss(void) {
    /* RTO: cwnd collapses to one (effective) MSS. */
    CHECK(tcpcc_rto_cwnd(1460) == 1460, "RTO loss window = 1 SMSS");
    CHECK(tcpcc_rto_cwnd(536) == 536,
          "loss window follows the PMTUD ladder's effective MSS");

    /* The unified ssthresh (the m6 helper both signals now share). */
    CHECK(tcpm6_recovery_ssthresh(40000, MSS) == 20000,
          "ssthresh = flight/2 when flight is large");
    CHECK(tcpm6_recovery_ssthresh(1000, MSS) == 2 * MSS,
          "ssthresh floors at 2*SMSS (RFC 5681 s3.2 step 2)");

    /* Recovery restart: from the loss window, slow start climbs back
     * to the new ssthresh, then goes linear — the whole story. */
    uint32_t cwnd = tcpcc_rto_cwnd(MSS);
    uint32_t ssthresh = tcpm6_recovery_ssthresh(20 * MSS, MSS);
    int rtts = 0;
    while (cwnd < ssthresh && rtts < 32) {
        cwnd = one_rtt_ss(cwnd ? cwnd : 1, ssthresh);
        rtts++;
    }
    CHECK(rtts >= 3 && rtts <= 5,
          "1 MSS -> ssthresh(10 MSS) takes ~log2(10) RTTs (got %d)", rtts);
}

static void test_cap(void) {
    CHECK(tcpcc_ack_grow(CAP - 10, CAP + 1, MSS, MSS, CAP) == CAP,
          "growth clamps at the cap");
    CHECK(tcpcc_ack_grow(CAP, CAP + 1, MSS, MSS, CAP) == CAP,
          "at the cap it stays at the cap");
    /* Wrap guard: near-UINT32_MAX cwnd with growth lands on the cap,
     * never wraps past zero. */
    CHECK(tcpcc_ack_grow(4294967290u, 4294967295u, MSS, MSS, 4294967295u)
          >= 4294967290u,
          "wrap guard holds at the uint32 edge");
}

int main(void) {
    test_iw();
    test_slow_start();
    test_crossover_and_ca();
    test_loss();
    test_cap();

    printf("test_tcp_cc: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
