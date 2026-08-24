#ifndef AURALITE_NET_TCP_CC_H
#define AURALITE_NET_TCP_CC_H

#include <stdint.h>

/*
 * tcp_cc.h — congestion-control arithmetic (REALINTERNET2 Y1).
 *
 * The Y0 rig measured the exact gap this file fills: the M6 layer
 * already owns loss DETECTION (dup-ACK classification, fast
 * retransmit/recovery with the RFC 5681 §3.2 ssthresh arithmetic) and
 * tcp.c already collapses on RTO — but cwnd GROWTH was an
 * unconditional `+= 1460` per progress ACK from a WIDE-OPEN initial
 * window (cwnd = ssthresh = TCP_WINDOW), so slow start never ran and
 * the "congestion window" congestion-controlled nothing until the
 * first loss.  Following tcp_x5.h/tcp_m6.h: pure inline functions
 * over plain integers, host-tested by tests/unit/test_tcp_cc.c;
 * tcp.c keeps the I/O.
 *
 * What deliberately stays out (named): no pacing, no HyStart, no
 * CUBIC — RFC 5681's Reno shape is the honest match for a stack
 * whose receive path is single-gap OOO.  ABC (RFC 3465) is applied
 * in its L=1 form: slow start grows by min(acked, SMSS) per ACK.
 */

/* RFC 6928 initial window: min(10*MSS, max(2*MSS, 14600)).  For the
 * MSS range this stack drives (536..1460 via the PMTUD ladder) that
 * simplifies to min(10*MSS, 14600) — both bounds are kept anyway so
 * the formula reads like the RFC. */
static inline uint32_t tcpcc_iw(uint32_t mss) {
    uint32_t ten = 10u * mss;
    uint32_t floor2 = 2u * mss > 14600u ? 2u * mss : 14600u;
    return ten < floor2 ? ten : floor2;
}

/* One cumulative ACK made progress: grow cwnd.
 *
 *   slow start (cwnd < ssthresh):  cwnd += min(acked_bytes, SMSS)
 *                                  (RFC 5681 §3.1 with ABC L=1)
 *   congestion avoidance:          cwnd += max(1, SMSS*SMSS / cwnd)
 *                                  (§3.1's per-ACK approximation; the
 *                                   max(1,·) keeps giant-cwnd growth
 *                                   from rounding to a full stall)
 *
 * `cap` bounds the result (the peer's window / our buffer budget —
 * growing past what we may ever send is bookkeeping noise). */
static inline uint32_t tcpcc_ack_grow(uint32_t cwnd, uint32_t ssthresh,
                                      uint32_t mss, uint32_t acked_bytes,
                                      uint32_t cap) {
    uint32_t grown;
    if (cwnd < ssthresh) {
        uint32_t inc = acked_bytes < mss ? acked_bytes : mss;
        grown = cwnd + inc;
    } else {
        uint32_t inc = (mss * mss) / (cwnd ? cwnd : 1u);
        if (inc == 0) inc = 1;
        grown = cwnd + inc;
    }
    if (grown < cwnd) grown = cap;           /* wrap guard */
    return grown > cap ? cap : grown;
}

/* RTO fired: RFC 5681 §3.1 — cwnd collapses to the loss window
 * (1 SMSS); ssthresh is the caller's job via tcpm6_recovery_ssthresh
 * (max(FlightSize/2, 2*SMSS) — ONE formula for both loss signals,
 * which Y1 also unifies in tcp.c: the old RTO path halved cwnd
 * instead of flight and floored at 1 SMSS instead of 2). */
static inline uint32_t tcpcc_rto_cwnd(uint32_t mss) {
    return mss;
}

#endif /* AURALITE_NET_TCP_CC_H */
