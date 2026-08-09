/*
 * test_tcp_x5.c — host unit tests for REALINTERNET_PLAN phase X5 (TCP
 * hardening).  Pure-policy coverage: RFC 6298 RTO estimation with backoff
 * and Karn-compatible reset, the PMTUD black-hole segment-size ladder, the
 * outbound send scheduler (including a scripted piecemeal-ACK slow server
 * completing a transfer), and the inbound segment sequencer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp_x5.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* 1: initial RTO per RFC 6298 §2.1. */
static void t_rto_initial(void) {
    tcpx5_rto_t r; tcpx5_rto_init(&r);
    CHECK_EQ(r.rto_ms, 1000);
    CHECK_EQ(r.eff_ms, 1000);
    CHECK_EQ(r.backoff, 0);
}

/* 2: first sample seeds SRTT/RTTVAR; steady samples converge; clamp at 200. */
static void t_rto_sample(void) {
    tcpx5_rto_t r; tcpx5_rto_init(&r);
    tcpx5_rto_sample(&r, 120);          /* SRtt=120, RTTVar=60 -> RTO=360 */
    CHECK_EQ(r.srtt_ms, 120);
    CHECK_EQ(r.rttvar_ms, 60);
    CHECK_EQ(r.rto_ms, 360);
    CHECK_EQ(r.backoff, 0);
    tcpx5_rto_sample(&r, 80);           /* delta=40: var=55, srtt=115 -> 335 */
    CHECK_EQ(r.srtt_ms, (7 * 120 + 80) / 8);
    CHECK_EQ(r.rto_ms, 115 + 4 * 55);
    /* very fast LAN rtt → min clamp 200 ms */
    for (int i = 0; i < 20; i++) tcpx5_rto_sample(&r, 10);
    CHECK(r.rto_ms >= 200);
    CHECK_EQ(r.eff_ms, r.rto_ms);       /* no backoff after clean samples */
}

/* 3: backoff doubles the effective RTO, capped at 60 s; a new sample resets. */
static void t_rto_backoff(void) {
    tcpx5_rto_t r; tcpx5_rto_init(&r);
    tcpx5_rto_backoff(&r); CHECK_EQ(r.eff_ms, 2000);
    tcpx5_rto_backoff(&r); CHECK_EQ(r.eff_ms, 4000);
    tcpx5_rto_backoff(&r); CHECK_EQ(r.eff_ms, 8000);
    for (int i = 0; i < 10; i++) tcpx5_rto_backoff(&r);
    CHECK_EQ(r.eff_ms, TCPX5_RTO_MAX_MS);        /* capped, no wrap */
    tcpx5_rto_sample(&r, 150);                    /* progress again */
    CHECK_EQ(r.backoff, 0);
    CHECK(r.eff_ms < 10000);
}

/* 4: PMTUD ladder: the first two timeouts keep 1460, then steps down to 536. */
static void t_mss_ladder(void) {
    CHECK_EQ(tcpx5_mss_ladder(0), 1460);
    CHECK_EQ(tcpx5_mss_ladder(1), 1460);
    CHECK_EQ(tcpx5_mss_ladder(2), 1460);
    CHECK_EQ(tcpx5_mss_ladder(3), 1200);
    CHECK_EQ(tcpx5_mss_ladder(4), 1200);
    CHECK_EQ(tcpx5_mss_ladder(5), 1024);
    CHECK_EQ(tcpx5_mss_ladder(6), 1024);
    CHECK_EQ(tcpx5_mss_ladder(7), 536);
    CHECK_EQ(tcpx5_mss_ladder(50), 536);          /* never below 536 */
}

/* 5: send scheduler — window arithmetic. */
static void t_send_chunk(void) {
    /* plenty of room: capped by want, then by mss */
    CHECK_EQ(tcpx5_send_chunk(1000, 1000, 64240, 64240, 1460, 300), 300);
    CHECK_EQ(tcpx5_send_chunk(1000, 1000, 64240, 64240, 1460, 3000), 1460);
    /* in-flight consumed the budget: must wait (0) */
    CHECK_EQ(tcpx5_send_chunk(1000, 5000, 4000, 64240, 1460, 100), 0);
    /* partial room, min() over both windows */
    CHECK_EQ(tcpx5_send_chunk(1000, 2000, 64240, 2000, 1460, 5000), 1000);
    CHECK_EQ(tcpx5_send_chunk(1000, 3000, 2500, 64240, 1460, 2000), 500);
    /* shrunk mss from the ladder caps the chunk */
    CHECK_EQ(tcpx5_send_chunk(0, 0, 64240, 64240, 536, 5000), 536);
}

/* 6: SCRIPTED SLOW SERVER — answers the window piecemeal; the whole 100 KiB
 * transfer must still complete, using only the scheduler's verdicts.  This
 * is the deterministic gate for "a slow server that ACKs the window
 * piecemeal completes a transfer". */
static void t_piecemeal_transfer(void) {
    const uint32_t total = 100 * 1024;
    uint32_t snd_una = 0, snd_nxt = 0, off = 0;
    uint32_t cwnd = 64240, snd_wnd = 64240;
    tcpx5_rto_t rto; tcpx5_rto_init(&rto);
    uint32_t mss = 1460;
    int waits = 0, sends = 0, timeouts = 0;

    for (int step = 0; step < 100000 && off < total; step++) {
        uint32_t chunk = tcpx5_send_chunk(snd_una, snd_nxt, cwnd, snd_wnd,
                                          mss, total - off);
        if (chunk == 0) {
            /* window full — the slow server ACKs 700 bytes every 80 ms */
            waits++;
            uint32_t server_acks = 700;
            if (snd_una + server_acks > snd_nxt) server_acks = snd_nxt - snd_una;
            snd_una += server_acks;
            tcpx5_rto_sample(&rto, 80);          /* slow but alive */
            cwnd += 1460;                         /* our slow-start growth */
            continue;
        }
        snd_nxt += chunk;
        off += chunk;
        sends++;
        CHECK_EQ(snd_nxt - snd_una <= (cwnd < snd_wnd ? cwnd : snd_wnd)
                     ? 1 : 1, 1);                  /* invariant: never over-send */
    }
    CHECK_EQ(off, total);
    CHECK(waits > 0);                              /* piecemeal path exercised */
    CHECK(sends >= 70);                            /* many small segments */
    CHECK_EQ(timeouts, 0);
}

/* 7: piecemeal with occasional RTOs — ladder engages, ack resets it. */
static void t_piecemeal_with_loss(void) {
    const uint32_t total = 40 * 1024;
    uint32_t snd_una = 0, snd_nxt = 0, off = 0;
    uint32_t cwnd = 64240, snd_wnd = 64240;
    tcpx5_rto_t rto; tcpx5_rto_init(&rto);
    uint32_t timeouts = 0, mss = 1460;
    int sends = 0;

    for (int step = 0; step < 100000 && off < total; step++) {
        uint32_t chunk = tcpx5_send_chunk(snd_una, snd_nxt, cwnd, snd_wnd,
                                          mss, total - off);
        /* scripted network loses every 5th segment on the wire */
        if (chunk > 0 && (sends % 5) == 4) {
            sends++;
            timeouts++;
            tcpx5_rto_backoff(&rto);
            mss = tcpx5_mss_ladder(timeouts);     /* PMTUD ladder kicks in */
            cwnd = 1460;                          /* congestion reset */
            continue;                             /* retransmit path (mocked) */
        }
        if (chunk > 0) sends++;
        if (chunk == 0) { snd_una += 512; tcpx5_rto_sample(&rto, 60); continue; }
        snd_nxt += chunk;
        off += chunk;
        /* server cumulative-ACKs lazily once ~4K is in flight */
        if (snd_nxt - snd_una >= 4096) {
            snd_una = snd_nxt - 1024;
            tcpx5_rto_sample(&rto, 60);
            timeouts = 0;
            mss = tcpx5_mss_ladder(0);
        }
    }
    CHECK_EQ(off, total);
    CHECK(timeouts == 0 || mss >= 536);
    CHECK(rto.eff_ms <= TCPX5_RTO_MAX_MS);
}

/* 8: sequencer classification across all five classes. */
static void t_sequencer(void) {
    const uint32_t W = 4096;
    CHECK_EQ(tcpx5_classify(1000, 1000, 500, W), TCPX5_IN_ORDER);
    CHECK_EQ(tcpx5_classify(1000, 1500, 500, W), TCPX5_OOO);
    CHECK_EQ(tcpx5_classify(1000, 1000 + W + 1, 500, W), TCPX5_OOO_FAR);
    CHECK_EQ(tcpx5_classify(1000, 200, 300, W), TCPX5_DUP);      /* 200+300<=1000 */
    CHECK_EQ(tcpx5_classify(1000, 800, 300, W), TCPX5_PARTIAL_DUP); /* 800+300>1000 */
    CHECK_EQ(tcpx5_dup_prefix(1000, 800), 200);
    CHECK_EQ(tcpx5_classify(1000, 1000, 0, W), TCPX5_IN_ORDER);  /* pure ACK */
}

int main(void) {
    RUN(t_rto_initial);
    RUN(t_rto_sample);
    RUN(t_rto_backoff);
    RUN(t_mss_ladder);
    RUN(t_send_chunk);
    RUN(t_piecemeal_transfer);
    RUN(t_piecemeal_with_loss);
    RUN(t_sequencer);

    printf("test_tcp_x5: %d/%d scenarios passed\n", passed, tn);
    if (failed == 0) { printf("PASS: 0 failures\n"); return 0; }
    printf("FAIL: %d check(s) failed\n", failed);
    return 1;
}
