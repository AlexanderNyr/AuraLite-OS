#ifndef AURALITE_NET_TCP_X5_H
#define AURALITE_NET_TCP_X5_H

/*
 * tcp_x5.h — pure TCP hardening policy (REALINTERNET_PLAN phase X5).
 *
 * Header-only, no kernel dependencies: the retransmit-timer estimator
 * (RFC 6298 as far as this stack can carry it), the PMTUD black-hole
 * segment-size ladder, the outbound send scheduler used by tcp_send's
 * window wait, and the inbound segment sequencer.  tcp.c glues them into
 * the connection table; tests/unit/test_tcp_x5.c drives them directly.
 */

#include <stdint.h>

/* ---------------------------------------------------------------- RTO -- */

/* RFC 6298 §2.1 initial RTO; our min is 200 ms (100 Hz clock granularity),
 * max 60 s.  The upper bound keeps a wedged connection diagnosable instead
 * of eternal. */
#define TCPX5_RTO_INIT_MS 1000u
#define TCPX5_RTO_MIN_MS   200u
#define TCPX5_RTO_MAX_MS 60000u

typedef struct {
    uint32_t srtt_ms;    /* smoothed RTT (0 until the first sample) */
    uint32_t rttvar_ms;  /* RTT variance */
    uint32_t rto_ms;     /* current RTO (already min/max clamped) */
    uint32_t eff_ms;     /* effective RTO including exponential backoff */
    uint8_t  backoff;    /* consecutive RTO expirations without progress */
} tcpx5_rto_t;

static inline void tcpx5_rto_init(tcpx5_rto_t *r) {
    r->srtt_ms = 0;
    r->rttvar_ms = 0;
    r->rto_ms = TCPX5_RTO_INIT_MS;
    r->eff_ms = TCPX5_RTO_INIT_MS;
    r->backoff = 0;
}

/* One RTT sample in milliseconds (only from segments that were never
 * retransmitted — Karn's rule is enforced by the caller). */
static inline void tcpx5_rto_sample(tcpx5_rto_t *r, uint32_t rtt_ms) {
    if (rtt_ms == 0) rtt_ms = 10;              /* one 100 Hz tick minimum */
    if (r->srtt_ms == 0) {
        r->srtt_ms = rtt_ms;
        r->rttvar_ms = rtt_ms / 2;
    } else {
        uint32_t delta = r->srtt_ms > rtt_ms ? r->srtt_ms - rtt_ms
                                             : rtt_ms - r->srtt_ms;
        /* RTTVAR = (3/4)RTTVAR + (1/4)|SRTT - R|; SRTT += (1/8)(R - SRTT) */
        r->rttvar_ms = (3 * r->rttvar_ms + delta) / 4;
        r->srtt_ms = (7 * r->srtt_ms + rtt_ms) / 8;
    }
    uint32_t rto = r->srtt_ms + 4 * r->rttvar_ms;
    if (rto < TCPX5_RTO_MIN_MS) rto = TCPX5_RTO_MIN_MS;
    if (rto > TCPX5_RTO_MAX_MS) rto = TCPX5_RTO_MAX_MS;
    r->rto_ms = rto;
    r->eff_ms = rto;
    r->backoff = 0;
}

/* An RTO has expired: exponential backoff (doubling), capped. */
static inline void tcpx5_rto_backoff(tcpx5_rto_t *r) {
    r->backoff++;
    uint32_t eff = r->eff_ms * 2;
    if (eff < r->eff_ms || eff > TCPX5_RTO_MAX_MS) eff = TCPX5_RTO_MAX_MS;
    r->eff_ms = eff;
}

/* ----------------------------------------------------- PMTUD ladder -- */

/* Consecutive retransmit timeouts -> effective segment size.  A DF-set
 * segment that always times out is classic Path-MTU black-hole evidence;
 * stepping the size down (before resending the SAME byte range) is the
 * classic cure (RFC 4821 / PLPMTUD in spirit).  Conservative plateaus. */
#define TCPX5_LADDER_LEN 4
static inline uint32_t tcpx5_mss_ladder(uint32_t timeouts) {
    static const uint32_t step[TCPX5_LADDER_LEN] = { 1460, 1200, 1024, 536 };
    uint32_t idx = timeouts < 3 ? 0 : (timeouts - 1) / 2;
    if (idx >= TCPX5_LADDER_LEN) idx = TCPX5_LADDER_LEN - 1;
    return step[idx];
}

/* ---------------------------------------------- send scheduler -- */

/* How much can we put on the wire right now?  min(cwnd, snd_wnd) of
 * in-flight budget minus what's already in flight, capped by the segment
 * size ladder.  Returns 0 => the sender must wait for an ACK. */
static inline uint32_t tcpx5_send_chunk(uint32_t snd_una, uint32_t snd_nxt,
                                        uint32_t cwnd, uint32_t snd_wnd,
                                        uint32_t eff_mss, uint32_t want) {
    uint32_t budget = cwnd < snd_wnd ? cwnd : snd_wnd;
    uint32_t in_flight = snd_nxt - snd_una;
    if (in_flight >= budget) return 0;
    uint32_t room = budget - in_flight;
    uint32_t chunk = want < room ? want : room;
    return chunk < eff_mss ? chunk : eff_mss;
}

/* ----------------------------------------------- segment sequencer -- */

/* Capacity of the per-connection single-gap out-of-order stash.  Covers
 * the whole 64240-byte receive window in the paths the stack actually
 * drives?  No — one 8 KiB gap at a time is the pragmatic X5 budget
 * (bigger gaps are dropped and re-fetched via dup-ACK pressure). */
#define TCPX5_OOO_CAP 8192u

typedef enum {
    TCPX5_IN_ORDER,     /* seg_seq == expected: accept */
    TCPX5_DUP,          /* fully below expected: drop, re-ACK */
    TCPX5_PARTIAL_DUP,  /* overlaps expected: trim prefix, accept tail */
    TCPX5_OOO,          /* ahead of expected, inside rcv window: stash */
    TCPX5_OOO_FAR       /* ahead beyond what we may hold: drop, re-ACK */
} tcpx5_seq_class_t;

static inline tcpx5_seq_class_t tcpx5_classify(uint32_t expected,
                                               uint32_t seg_seq,
                                               uint32_t seg_len,
                                               uint32_t rcv_wnd) {
    if (seg_len == 0) return TCPX5_IN_ORDER;        /* pure ACK/window probe */
    if (seg_seq == expected) return TCPX5_IN_ORDER;
    int32_t diff = (int32_t)(seg_seq - expected);   /* seq arithmetic */
    if (diff >= 0)
        return ((uint32_t)diff <= rcv_wnd) ? TCPX5_OOO : TCPX5_OOO_FAR;
    if ((uint32_t)(-diff) >= seg_len) return TCPX5_DUP;
    return TCPX5_PARTIAL_DUP;
}

/* For TCPX5_PARTIAL_DUP: how many bytes of the segment are old news. */
static inline uint32_t tcpx5_dup_prefix(uint32_t expected, uint32_t seg_seq) {
    return expected - seg_seq;
}

#endif /* AURALITE_NET_TCP_X5_H */
