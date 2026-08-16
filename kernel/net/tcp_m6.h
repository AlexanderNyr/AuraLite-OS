#ifndef AURALITE_NET_TCP_M6_H
#define AURALITE_NET_TCP_M6_H

#include <stdint.h>

/*
 * MATURITY_PLAN.md M6 — production TCP, policy layer.
 *
 * AUDIT_A3 measured this phase against the tree and found it understated:
 * cwnd, ssthresh, rto_ms, SRTT, a retransmit queue, FIN_WAIT_1/2 and
 * tcp_listen() were already implemented (REALINTERNET_PLAN X5), and
 * TCP_MAX_CONNS was 16 rather than the 8 the task list assumed.  What is
 * genuinely missing is this file's subject matter:
 *
 *   - duplicate-ACK counting and fast retransmit / fast recovery (RFC 5681)
 *   - Nagle's algorithm (RFC 896) with the TCP_NODELAY escape
 *   - delayed ACK (RFC 1122 §4.2.3.2)
 *   - the TIME_WAIT / CLOSE_WAIT / LAST_ACK states
 *
 * Following the pattern tcp_x5.h established: the *policy* lives here as
 * pure inline functions over plain integers, so it can be unit-tested on
 * the host without a NIC, a guest or a timer.  tcp.c keeps the I/O.
 */

/* ------------------------------------------------ fast retransmit ---- */

/* RFC 5681 §3.2: three DUPLICATE ACKs (i.e. the fourth copy of the same
 * ACK number) trigger retransmission without waiting for the RTO. */
#define TCPM6_DUPACK_THRESH 3u

typedef struct {
    uint32_t last_ack;      /* the ACK number we have been seeing */
    uint32_t dup_count;     /* how many duplicates in a row */
    uint8_t  in_recovery;   /* inside fast recovery? */
    uint32_t recover;       /* snd_nxt when recovery began (RFC 6582) */
} tcpm6_dupack_t;

static inline void tcpm6_dupack_init(tcpm6_dupack_t *d) {
    d->last_ack = 0;
    d->dup_count = 0;
    d->in_recovery = 0;
    d->recover = 0;
}

typedef enum {
    TCPM6_ACK_PROGRESS,     /* new data acknowledged */
    TCPM6_ACK_DUP,          /* duplicate, below the retransmit threshold */
    TCPM6_ACK_FAST_RETX,    /* threshold reached: retransmit now */
    TCPM6_ACK_IN_RECOVERY,  /* further duplicate while already recovering */
    TCPM6_ACK_OLD           /* stale ACK, before snd_una: ignore */
} tcpm6_ack_kind_t;

/*
 * Classify an incoming ACK.
 *
 * A duplicate ACK is not merely "ack == last_ack": RFC 5681 §2 requires
 * that the segment carry no data and not move the window, otherwise a
 * stream of pure window updates during a stall would be miscounted as
 * loss.  `has_data` and `win_changed` let the caller enforce that.
 */
static inline tcpm6_ack_kind_t tcpm6_on_ack(tcpm6_dupack_t *d,
                                            uint32_t snd_una,
                                            uint32_t ack,
                                            uint32_t snd_nxt,
                                            int has_data,
                                            int win_changed) {
    if ((int32_t)(ack - snd_una) > 0) {
        /* Real progress.  Leaving recovery is the caller's cue to deflate
         * cwnd back to ssthresh. */
        d->last_ack = ack;
        d->dup_count = 0;
        if (d->in_recovery && (int32_t)(ack - d->recover) >= 0)
            d->in_recovery = 0;
        return TCPM6_ACK_PROGRESS;
    }
    if ((int32_t)(ack - snd_una) < 0) return TCPM6_ACK_OLD;

    /* ack == snd_una: a duplicate only if it tells us nothing new. */
    if (has_data || win_changed) return TCPM6_ACK_OLD;
    if (snd_nxt == snd_una) return TCPM6_ACK_OLD;   /* nothing in flight */

    if (d->in_recovery) {
        d->dup_count++;
        return TCPM6_ACK_IN_RECOVERY;
    }

    d->last_ack = ack;
    d->dup_count++;
    if (d->dup_count == TCPM6_DUPACK_THRESH) {
        d->in_recovery = 1;
        d->recover = snd_nxt;
        return TCPM6_ACK_FAST_RETX;
    }
    return TCPM6_ACK_DUP;
}

/* RFC 5681 §3.2 step 2: ssthresh = max(FlightSize/2, 2*SMSS) on entering
 * fast recovery. */
static inline uint32_t tcpm6_recovery_ssthresh(uint32_t flight, uint32_t mss) {
    uint32_t half = flight / 2u;
    uint32_t floor_ = 2u * mss;
    return half > floor_ ? half : floor_;
}

/* Step 3: cwnd = ssthresh + 3*SMSS, inflated by the segments that have
 * left the network (each duplicate ACK is evidence of one). */
static inline uint32_t tcpm6_recovery_cwnd(uint32_t ssthresh, uint32_t mss,
                                           uint32_t dupacks) {
    return ssthresh + (dupacks < 3u ? 3u : dupacks) * mss;
}

/* ------------------------------------------------------- Nagle ------- */

/*
 * RFC 896 / RFC 1122 §4.2.3.4.  A small segment may go out only when
 * there is nothing unacknowledged in flight; otherwise it waits so that
 * the coalesced data rides one segment.  A full-sized segment always
 * goes.  TCP_NODELAY disables the rule outright.
 *
 * Returns 1 = send now, 0 = hold.
 */
static inline int tcpm6_nagle_may_send(uint32_t len, uint32_t mss,
                                       uint32_t snd_una, uint32_t snd_nxt,
                                       int nodelay, int push) {
    if (len == 0) return 0;
    if (nodelay) return 1;
    if (len >= mss) return 1;              /* full segment: always */
    if (snd_nxt == snd_una) return 1;      /* nothing in flight */
    if (push) return 1;                    /* caller is flushing (close/PSH) */
    return 0;
}

/* -------------------------------------------------- delayed ACK ------ */

/*
 * RFC 1122 §4.2.3.2: an ACK may be withheld briefly, but must be sent for
 * every SECOND full-sized segment, and the delay must not exceed 500 ms
 * (200 ms in practice).  Withholding an ACK for a segment that fills a
 * window, or one carrying PSH, defeats the purpose, so those go at once.
 */
#define TCPM6_DELACK_MS      200u
#define TCPM6_DELACK_SEGS    2u

typedef struct {
    uint32_t pending_segs;  /* unacknowledged full segments received */
    uint32_t first_tick;    /* tick at which the first was withheld */
} tcpm6_delack_t;

static inline void tcpm6_delack_init(tcpm6_delack_t *a) {
    a->pending_segs = 0;
    a->first_tick = 0;
}

/* Returns 1 if an ACK must be emitted now. */
static inline int tcpm6_delack_on_segment(tcpm6_delack_t *a, uint32_t now_tick,
                                          uint32_t seg_len, uint32_t mss,
                                          int push, int out_of_order) {
    if (seg_len == 0) return 0;                  /* pure ACK: nothing owed */
    if (push || out_of_order) {                  /* ack immediately */
        a->pending_segs = 0;
        return 1;
    }
    if (a->pending_segs == 0) a->first_tick = now_tick;
    a->pending_segs++;
    if (a->pending_segs >= TCPM6_DELACK_SEGS || seg_len >= mss) {
        a->pending_segs = 0;
        return 1;
    }
    return 0;
}

/* Called from the poll loop: has the withheld ACK aged out? */
static inline int tcpm6_delack_expired(const tcpm6_delack_t *a,
                                       uint32_t now_tick, uint32_t ms_per_tick) {
    if (a->pending_segs == 0) return 0;
    uint32_t elapsed = (now_tick - a->first_tick) * ms_per_tick;
    return elapsed >= TCPM6_DELACK_MS;
}

/* ---------------------------------------------------- TIME_WAIT ------ */

/*
 * 2*MSL before a closed connection's 4-tuple may be reused, so that a
 * straggling duplicate cannot be accepted into a new incarnation.  RFC 793
 * says MSL = 2 minutes; every real stack shortens this, and 30 s of
 * TIME_WAIT is the common choice (Linux uses 60 s).
 */
#define TCPM6_MSL_MS        15000u
#define TCPM6_TIME_WAIT_MS  (2u * TCPM6_MSL_MS)

static inline int tcpm6_time_wait_expired(uint32_t entered_tick,
                                          uint32_t now_tick,
                                          uint32_t ms_per_tick) {
    return (now_tick - entered_tick) * ms_per_tick >= TCPM6_TIME_WAIT_MS;
}

#endif /* AURALITE_NET_TCP_M6_H */
