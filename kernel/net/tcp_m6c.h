#ifndef AURALITE_NET_TCP_M6C_H
#define AURALITE_NET_TCP_M6C_H

#include <stdint.h>

/*
 * MATURITY_PLAN.md M6c — the two prerequisites SACK needs.
 *
 * Scoping note, because this is the interesting part.  SACK looks like one
 * more bullet in M6's task list, but it cannot be built on this stack as it
 * stands, and building it anyway would produce exactly the kind of
 * fabricated green this audit exists to remove:
 *
 *   1. THE STACK HAS NEVER EMITTED A TCP OPTION.  tcp_send_segment_at()
 *      hardcodes data_offset = 5<<4, i.e. a bare 20-byte header.  SACK
 *      needs SACK-permitted in the SYN and a variable-length option on
 *      every ACK that reports blocks.
 *
 *   2. THERE IS NOTHING TO SELECTIVELY RETRANSMIT FROM.  tcp_record_retx()
 *      keeps ONE MSS-sized segment.  SACK's whole payoff is resending only
 *      the missing blocks; with a single buffer the only thing that can be
 *      resent is the most recent segment, whatever the receiver reports.
 *      The option would appear on the wire, the logs would look right, and
 *      the retransmit behaviour would be unchanged.
 *
 * So this header supplies the missing floor: option encode/decode, and a
 * bounded retransmit queue.  Pure functions over plain buffers, testable on
 * the host — the pattern tcp_x5.h and tcp_m6.h established.  SACK itself
 * (M6d) then becomes a small patch on top.
 */

/* ------------------------------------------------- TCP options ------- */

#define TCPOPT_END          0u
#define TCPOPT_NOP          1u
#define TCPOPT_MSS          2u
#define TCPOPT_WSCALE       3u
#define TCPOPT_SACK_PERM    4u
#define TCPOPT_SACK         5u
#define TCPOPT_TIMESTAMP    8u

/* What we learned from a peer's option block. */
typedef struct {
    uint16_t mss;           /* 0 = not offered */
    uint8_t  sack_permitted;
    uint8_t  wscale;        /* shift count; 0xFF = not offered */
} tcpm6c_opts_t;

static inline void tcpm6c_opts_init(tcpm6c_opts_t *o) {
    o->mss = 0;
    o->sack_permitted = 0;
    o->wscale = 0xFF;
}

/*
 * Parse a TCP option block.  `len` is (data_offset*4 - 20) and may be 0.
 *
 * Hostile input is the norm here: this runs on bytes from the network
 * before anything has been validated.  A zero or absurd option length is
 * the classic way to spin a parser forever, so every branch must consume
 * at least one byte.  Returns 0 on a well-formed block, -1 if it was
 * malformed (the caller should still use whatever was parsed).
 */
static inline int tcpm6c_parse_opts(const uint8_t *p, uint32_t len,
                                    tcpm6c_opts_t *out) {
    tcpm6c_opts_init(out);
    uint32_t i = 0;
    while (i < len) {
        uint8_t kind = p[i];
        if (kind == TCPOPT_END) return 0;
        if (kind == TCPOPT_NOP) { i++; continue; }
        /* Every other option carries a length byte. */
        if (i + 1 >= len) return -1;
        uint8_t olen = p[i + 1];
        /* olen counts kind and length themselves: < 2 is malformed, and
         * treating it as progress-free would hang the loop. */
        if (olen < 2 || i + olen > len) return -1;

        switch (kind) {
        case TCPOPT_MSS:
            if (olen == 4) out->mss = (uint16_t)((p[i + 2] << 8) | p[i + 3]);
            break;
        case TCPOPT_SACK_PERM:
            if (olen == 2) out->sack_permitted = 1;
            break;
        case TCPOPT_WSCALE:
            if (olen == 3) out->wscale = p[i + 2];
            break;
        default:
            break;      /* unknown options are skipped, not fatal */
        }
        i += olen;
    }
    return 0;
}

/*
 * Build the SYN option block: MSS, and SACK-permitted when `want_sack`.
 * Returns the number of bytes written, always a multiple of 4 (the TCP
 * header length is measured in 32-bit words, so the block must be padded).
 * `buf` must have room for 8 bytes.
 */
static inline uint32_t tcpm6c_build_syn_opts(uint8_t *buf, uint16_t mss,
                                             int want_sack) {
    uint32_t n = 0;
    buf[n++] = TCPOPT_MSS;
    buf[n++] = 4;
    buf[n++] = (uint8_t)(mss >> 8);
    buf[n++] = (uint8_t)(mss & 0xFF);
    if (want_sack) {
        /* NOP padding keeps the 2-byte option 4-byte aligned. */
        buf[n++] = TCPOPT_NOP;
        buf[n++] = TCPOPT_NOP;
        buf[n++] = TCPOPT_SACK_PERM;
        buf[n++] = 2;
    }
    /* Pad to a 4-byte boundary.  With today's two options (4 + 4 bytes)
     * the block is already aligned and this loop does not run -- it is
     * here for M6d, whose SACK blocks are 10/18/26/34 bytes and always
     * need padding.  Deliberately not deleted as dead code: removing it
     * would be invisible now and a wire-format bug the moment SACK lands. */
    while (n % 4u) buf[n++] = TCPOPT_END;
    return n;
}

/* data_offset nibble for a header carrying `optlen` bytes of options. */
static inline uint8_t tcpm6c_data_offset(uint32_t optlen) {
    return (uint8_t)(((20u + optlen) / 4u) << 4);
}

/* ------------------------------------------- retransmit queue -------- */

/*
 * A bounded queue of unacknowledged segments.
 *
 * Depth is a memory trade, not a protocol one: 8 MSS segments is ~11.7 KiB
 * per connection, so 16 connections cost ~187 KiB.  A full 64 KiB window
 * per connection would be 1 MiB and is not worth it here — when the queue
 * fills, the sender simply stops until an ACK frees a slot, which is what
 * the congestion window would have done anyway.
 */
#define TCPM6C_RETXQ_DEPTH 8u

typedef struct {
    uint32_t seq;           /* first sequence number in this segment */
    uint32_t len;           /* payload bytes */
    uint8_t  flags;
    uint8_t  in_use;
    uint8_t  sacked;        /* M6d: peer reported this block as received */
    uint32_t first_tick;    /* for RTT sampling / Karn */
} tcpm6c_seg_t;

typedef struct {
    tcpm6c_seg_t seg[TCPM6C_RETXQ_DEPTH];
    uint32_t     head;      /* oldest unacknowledged */
    uint32_t     count;
} tcpm6c_retxq_t;

static inline void tcpm6c_retxq_init(tcpm6c_retxq_t *q) {
    for (uint32_t i = 0; i < TCPM6C_RETXQ_DEPTH; i++) {
        q->seg[i].in_use = 0;
        q->seg[i].sacked = 0;
    }
    q->head = 0;
    q->count = 0;
}

static inline int tcpm6c_retxq_full(const tcpm6c_retxq_t *q) {
    return q->count >= TCPM6C_RETXQ_DEPTH;
}

/* Returns the slot index, or -1 when full. */
static inline int tcpm6c_retxq_push(tcpm6c_retxq_t *q, uint32_t seq,
                                    uint32_t len, uint8_t flags,
                                    uint32_t tick) {
    if (tcpm6c_retxq_full(q)) return -1;
    uint32_t idx = (q->head + q->count) % TCPM6C_RETXQ_DEPTH;
    q->seg[idx].seq = seq;
    q->seg[idx].len = len;
    q->seg[idx].flags = flags;
    q->seg[idx].in_use = 1;
    q->seg[idx].sacked = 0;
    q->seg[idx].first_tick = tick;
    q->count++;
    return (int)idx;
}

/*
 * Retire every segment fully covered by a cumulative ACK.  Returns how
 * many were retired.
 *
 * Sequence numbers wrap, so the comparison must be the signed difference,
 * never `<=` on the raw values: a connection that has been up long enough
 * to pass 2^31 bytes would otherwise stop retiring anything at all.
 */
static inline uint32_t tcpm6c_retxq_ack(tcpm6c_retxq_t *q, uint32_t ack) {
    uint32_t retired = 0;
    while (q->count > 0) {
        tcpm6c_seg_t *s = &q->seg[q->head];
        uint32_t end = s->seq + s->len;
        if ((int32_t)(ack - end) < 0) break;      /* not fully acked */
        s->in_use = 0;
        s->sacked = 0;
        q->head = (q->head + 1) % TCPM6C_RETXQ_DEPTH;
        q->count--;
        retired++;
    }
    return retired;
}

/* The oldest unacknowledged segment: what a timeout retransmits. */
static inline tcpm6c_seg_t *tcpm6c_retxq_oldest(tcpm6c_retxq_t *q) {
    if (q->count == 0) return 0;
    return &q->seg[q->head];
}

/* Walk the queue in sequence order: i in [0, count). */
static inline tcpm6c_seg_t *tcpm6c_retxq_at(tcpm6c_retxq_t *q, uint32_t i) {
    if (i >= q->count) return 0;
    return &q->seg[(q->head + i) % TCPM6C_RETXQ_DEPTH];
}

/* Bytes still in flight. */
static inline uint32_t tcpm6c_retxq_bytes(const tcpm6c_retxq_t *q) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < q->count; i++)
        n += q->seg[(q->head + i) % TCPM6C_RETXQ_DEPTH].len;
    return n;
}

#endif /* AURALITE_NET_TCP_M6C_H */
