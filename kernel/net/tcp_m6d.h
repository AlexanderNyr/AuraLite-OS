#ifndef AURALITE_NET_TCP_M6D_H
#define AURALITE_NET_TCP_M6D_H

#include <stdint.h>
#include "kernel/net/tcp_m6c.h"

/*
 * MATURITY_PLAN.md M6d — SACK (RFC 2018), on the floor M6c laid.
 *
 * M6c established the two things this needs: the stack can now emit and
 * parse TCP options at all, and it keeps more than one unacknowledged
 * segment.  With those in place SACK is what it should be -- bookkeeping.
 *
 * Receiver side: report the out-of-order data we are holding, so the peer
 * learns WHICH bytes are missing instead of only that something is.
 * Sender side: mark the reported blocks on the retransmit queue and resend
 * only the holes.
 */

/* RFC 2018 s3: the option is 8*n+2 bytes; the 40-byte option space caps it
 * at 4 blocks, and 3 when timestamps are also present. */
#define TCPM6D_MAX_BLOCKS 4u

typedef struct {
    uint32_t start;     /* first sequence number of the block */
    uint32_t end;       /* one past the last */
} tcpm6d_block_t;

/* ------------------------------------------------ receiver side ------ */

/*
 * Encode `n` blocks into a TCP option.  Returns bytes written (a multiple
 * of 4 after padding), or 0 when there is nothing to report.
 *
 * `buf` must have room for 2 + 8*TCPM6D_MAX_BLOCKS + 2 padding = 36 bytes.
 * Two leading NOPs are conventional: they align the block list on a 4-byte
 * boundary so the option does not straddle words.
 */
static inline uint32_t tcpm6d_encode(uint8_t *buf, const tcpm6d_block_t *blk,
                                     uint32_t n) {
    if (n == 0) return 0;
    if (n > TCPM6D_MAX_BLOCKS) n = TCPM6D_MAX_BLOCKS;

    uint32_t i = 0;
    buf[i++] = TCPOPT_NOP;
    buf[i++] = TCPOPT_NOP;
    buf[i++] = TCPOPT_SACK;
    buf[i++] = (uint8_t)(2u + 8u * n);      /* kind + len + the blocks */

    for (uint32_t b = 0; b < n; b++) {
        uint32_t s = blk[b].start, e = blk[b].end;
        buf[i++] = (uint8_t)(s >> 24); buf[i++] = (uint8_t)(s >> 16);
        buf[i++] = (uint8_t)(s >> 8);  buf[i++] = (uint8_t)s;
        buf[i++] = (uint8_t)(e >> 24); buf[i++] = (uint8_t)(e >> 16);
        buf[i++] = (uint8_t)(e >> 8);  buf[i++] = (uint8_t)e;
    }
    while (i % 4u) buf[i++] = TCPOPT_END;
    return i;
}

/*
 * Decode the SACK blocks out of an option area.  Returns how many were
 * found (capped at TCPM6D_MAX_BLOCKS).
 *
 * Same hostile-input rule as tcpm6c_parse_opts(): these are network bytes,
 * every branch must consume at least one, and a length that is not
 * 2 + 8*n is malformed rather than something to guess at.
 */
static inline uint32_t tcpm6d_decode(const uint8_t *p, uint32_t len,
                                     tcpm6d_block_t *out) {
    uint32_t found = 0, i = 0;
    while (i < len) {
        uint8_t kind = p[i];
        if (kind == TCPOPT_END) break;
        if (kind == TCPOPT_NOP) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t olen = p[i + 1];
        if (olen < 2 || i + olen > len) break;

        if (kind == TCPOPT_SACK && olen >= 10 && ((olen - 2u) % 8u) == 0) {
            uint32_t nb = (uint32_t)(olen - 2u) / 8u;
            const uint8_t *q = p + i + 2;
            for (uint32_t b = 0; b < nb && found < TCPM6D_MAX_BLOCKS; b++) {
                uint32_t s = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  |  (uint32_t)q[3];
                uint32_t e = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) |
                             ((uint32_t)q[6] << 8)  |  (uint32_t)q[7];
                q += 8;
                /* A block whose end does not follow its start is nonsense;
                 * accepting it would mark arbitrary queue entries acked. */
                if ((int32_t)(e - s) <= 0) continue;
                out[found].start = s;
                out[found].end = e;
                found++;
            }
        }
        i += olen;
    }
    return found;
}

/* ------------------------------------------------- sender side ------- */

/*
 * Mark queue segments fully covered by the reported blocks.
 *
 * Only FULLY covered segments count.  A partially covered segment must
 * stay unsacked: the uncovered bytes still have to be retransmitted, and
 * treating it as received would silently drop them.
 *
 * Returns how many segments were newly marked.
 */
static inline uint32_t tcpm6d_mark(tcpm6c_retxq_t *q,
                                   const tcpm6d_block_t *blk, uint32_t n) {
    uint32_t marked = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        tcpm6c_seg_t *s = tcpm6c_retxq_at(q, i);
        if (!s || s->sacked) continue;
        uint32_t s_end = s->seq + s->len;
        for (uint32_t b = 0; b < n; b++) {
            if ((int32_t)(s->seq - blk[b].start) >= 0 &&
                (int32_t)(blk[b].end - s_end) >= 0) {
                s->sacked = 1;
                marked++;
                break;
            }
        }
    }
    return marked;
}

/*
 * The next hole to retransmit: the oldest queued segment the peer has NOT
 * reported.  Returns 0 when every queued segment is sacked (in which case
 * the sender should wait for the cumulative ACK rather than resend).
 */
static inline tcpm6c_seg_t *tcpm6d_next_hole(tcpm6c_retxq_t *q) {
    for (uint32_t i = 0; i < q->count; i++) {
        tcpm6c_seg_t *s = tcpm6c_retxq_at(q, i);
        if (s && !s->sacked) return s;
    }
    return 0;
}

/*
 * RFC 6675: bytes the peer is known to hold, which must be discounted from
 * the in-flight estimate.  Without this the sender counts SACKed data as
 * still in the network, keeps the window artificially full, and stalls at
 * exactly the moment it should be recovering.
 */
static inline uint32_t tcpm6d_sacked_bytes(const tcpm6c_retxq_t *q) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < q->count; i++) {
        const tcpm6c_seg_t *s = &q->seg[(q->head + i) % TCPM6C_RETXQ_DEPTH];
        if (s->sacked) n += s->len;
    }
    return n;
}

/*
 * Build the receiver's block list from the single out-of-order region the
 * sequencer stashes (TCPX5_OOO_CAP).  RFC 2018 requires the most recently
 * received block first; with one stash that is simply the one block.
 *
 * Returns the number of blocks (0 or 1).  Kept as a function so M6e can
 * grow it to several stashes without touching tcp.c.
 */
static inline uint32_t tcpm6d_build_blocks(tcpm6d_block_t *out,
                                           int ooo_valid, uint32_t ooo_seq,
                                           uint32_t ooo_len) {
    if (!ooo_valid || ooo_len == 0) return 0;
    out[0].start = ooo_seq;
    out[0].end = ooo_seq + ooo_len;
    return 1;
}

#endif /* AURALITE_NET_TCP_M6D_H */
