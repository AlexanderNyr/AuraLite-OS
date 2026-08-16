/*
 * test_tcp_m6d.c — host unit tests for SACK (RFC 2018).
 *
 * The prerequisites landed in M6c; this covers the SACK bookkeeping
 * itself: encoding blocks a peer can read, decoding hostile ones, marking
 * the retransmit queue, and choosing which hole to resend.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp_m6d.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ------------------------------------------------------ encoding ----- */

/* 1: one block encodes to the RFC 2018 shape and is 4-byte aligned. */
static void t_encode_one(void) {
    tcpm6d_block_t b = { 2000, 3000 };
    uint8_t buf[36];
    uint32_t n = tcpm6d_encode(buf, &b, 1);

    CHECK_EQ(n, 12);                /* 2 NOP + kind + len + 8 = 12 */
    CHECK_EQ(n % 4, 0);
    CHECK_EQ(buf[0], TCPOPT_NOP);
    CHECK_EQ(buf[1], TCPOPT_NOP);
    CHECK_EQ(buf[2], TCPOPT_SACK);
    CHECK_EQ(buf[3], 10);           /* 2 + 8*1 */
    /* Big-endian on the wire. */
    CHECK_EQ(buf[4], 0); CHECK_EQ(buf[5], 0);
    CHECK_EQ(buf[6], (2000 >> 8) & 0xFF);
    CHECK_EQ(buf[7], 2000 & 0xFF);
}

/* 2: nothing to report encodes to nothing at all. */
static void t_encode_empty(void) {
    uint8_t buf[36];
    memset(buf, 0xAA, sizeof(buf));
    CHECK_EQ(tcpm6d_encode(buf, 0, 0), 0);
    CHECK_EQ(buf[0], 0xAA);         /* untouched */
}

/* 3: the block count is capped at what the option space allows. */
static void t_encode_cap(void) {
    tcpm6d_block_t b[8];
    for (uint32_t i = 0; i < 8; i++) {
        b[i].start = 1000 + i * 200;
        b[i].end = b[i].start + 100;
    }
    uint8_t buf[36];
    uint32_t n = tcpm6d_encode(buf, b, 8);
    CHECK_EQ(buf[3], 2 + 8 * TCPM6D_MAX_BLOCKS);
    CHECK_EQ(n, 4 + 8 * TCPM6D_MAX_BLOCKS);
    CHECK(n <= 36);
}

/* 4: what we emit, we can read back. */
static void t_roundtrip(void) {
    tcpm6d_block_t in[3] = { {5000,6000}, {7000,7500}, {9000,9100} };
    uint8_t buf[36];
    uint32_t n = tcpm6d_encode(buf, in, 3);

    tcpm6d_block_t out[TCPM6D_MAX_BLOCKS];
    CHECK_EQ(tcpm6d_decode(buf, n, out), 3);
    for (uint32_t i = 0; i < 3; i++) {
        CHECK_EQ(out[i].start, in[i].start);
        CHECK_EQ(out[i].end, in[i].end);
    }
}

/* ------------------------------------------------------ decoding ----- */

/*
 * 5: hostile input.  These bytes come off the network.  A zero-length
 * option must not spin the loop; a length that is not 2+8n is malformed;
 * a block whose end precedes its start would otherwise mark arbitrary
 * queue entries as received.  If this test hangs, that is the bug.
 */
static void t_decode_hostile(void) {
    tcpm6d_block_t out[TCPM6D_MAX_BLOCKS];

    uint8_t zero_len[] = { TCPOPT_SACK, 0, 0, 0 };
    CHECK_EQ(tcpm6d_decode(zero_len, sizeof(zero_len), out), 0);

    /* Length 9: not 2 + 8n, so the block list is not well formed. */
    uint8_t odd_len[] = { TCPOPT_SACK, 9, 0,0,0,0, 0,0,0 };
    CHECK_EQ(tcpm6d_decode(odd_len, sizeof(odd_len), out), 0);

    /* Length running past the buffer. */
    uint8_t overrun[] = { TCPOPT_SACK, 40, 0, 0 };
    CHECK_EQ(tcpm6d_decode(overrun, sizeof(overrun), out), 0);

    /* A reversed block (end < start) must be dropped, not accepted. */
    tcpm6d_block_t bad = { 9000, 8000 };
    uint8_t buf[36];
    uint32_t n = tcpm6d_encode(buf, &bad, 1);
    CHECK_EQ(tcpm6d_decode(buf, n, out), 0);

    /* An empty block (end == start) reports no bytes and is dropped too. */
    tcpm6d_block_t empty = { 9000, 9000 };
    n = tcpm6d_encode(buf, &empty, 1);
    CHECK_EQ(tcpm6d_decode(buf, n, out), 0);
}

/* 6: a SACK option among others is found; unknown options are skipped. */
static void t_decode_mixed(void) {
    /* timestamps(8,10) then SACK with one block 100..200 */
    uint8_t opts[] = {
        8,10, 0,0,0,0, 0,0,0,0,
        TCPOPT_SACK,10, 0,0,0,100, 0,0,0,200
    };
    tcpm6d_block_t out[TCPM6D_MAX_BLOCKS];
    CHECK_EQ(tcpm6d_decode(opts, sizeof(opts), out), 1);
    CHECK_EQ(out[0].start, 100);
    CHECK_EQ(out[0].end, 200);
}

/* --------------------------------------------------- queue marking --- */

/* 7: a fully covered segment is marked; the rest are not. */
static void t_mark_full(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    for (uint32_t i = 0; i < 4; i++)
        tcpm6c_retxq_push(&q, 1000 + i * 100, 100, 0x10, i);

    /* Peer holds 1100..1300, i.e. segments 2 and 3. */
    tcpm6d_block_t b = { 1100, 1300 };
    CHECK_EQ(tcpm6d_mark(&q, &b, 1), 2);
    CHECK_EQ(tcpm6c_retxq_at(&q, 0)->sacked, 0);
    CHECK_EQ(tcpm6c_retxq_at(&q, 1)->sacked, 1);
    CHECK_EQ(tcpm6c_retxq_at(&q, 2)->sacked, 1);
    CHECK_EQ(tcpm6c_retxq_at(&q, 3)->sacked, 0);

    /* Re-applying the same block marks nothing new. */
    CHECK_EQ(tcpm6d_mark(&q, &b, 1), 0);
}

/*
 * 8: THE case that matters.  A partially covered segment must stay
 * unsacked -- its uncovered bytes still have to be retransmitted, and
 * marking it would silently drop them.
 */
static void t_mark_partial_is_not_sacked(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    tcpm6c_retxq_push(&q, 1000, 100, 0x10, 1);   /* 1000..1100 */

    tcpm6d_block_t half = { 1000, 1050 };        /* only the first half */
    CHECK_EQ(tcpm6d_mark(&q, &half, 1), 0);
    CHECK_EQ(tcpm6c_retxq_at(&q, 0)->sacked, 0);

    /* Overlapping the tail is equally insufficient. */
    tcpm6d_block_t tail = { 1050, 1100 };
    CHECK_EQ(tcpm6d_mark(&q, &tail, 1), 0);

    /* Exact coverage does mark it. */
    tcpm6d_block_t all = { 1000, 1100 };
    CHECK_EQ(tcpm6d_mark(&q, &all, 1), 1);
}

/* 9: the next hole is the oldest unsacked segment. */
static void t_next_hole(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    for (uint32_t i = 0; i < 4; i++)
        tcpm6c_retxq_push(&q, 1000 + i * 100, 100, 0x10, i);

    /* Peer got everything except 1100..1200. */
    tcpm6d_block_t b[2] = { {1000,1100}, {1200,1400} };
    tcpm6d_mark(&q, b, 2);

    /* CHECK records a failure but does not abort, so a null pointer here
     * must not be dereferenced anyway -- otherwise a broken implementation
     * crashes the whole suite instead of reporting which case failed.  The
     * first version of this test did exactly that and segfaulted under a
     * negative control, hiding the other twelve results. */
    tcpm6c_seg_t *hole = tcpm6d_next_hole(&q);
    CHECK(hole != 0);
    CHECK_EQ(hole ? hole->seq : 0, 1100);

    /* Once everything is sacked there is no hole to resend. */
    tcpm6d_block_t all = { 1000, 1400 };
    tcpm6d_mark(&q, &all, 1);
    CHECK(tcpm6d_next_hole(&q) == 0);
}

/* 10: SACKed bytes are discounted from the in-flight estimate. */
static void t_sacked_bytes(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    for (uint32_t i = 0; i < 4; i++)
        tcpm6c_retxq_push(&q, 1000 + i * 100, 100, 0x10, i);

    CHECK_EQ(tcpm6c_retxq_bytes(&q), 400);
    CHECK_EQ(tcpm6d_sacked_bytes(&q), 0);

    tcpm6d_block_t b = { 1200, 1400 };
    tcpm6d_mark(&q, &b, 1);
    CHECK_EQ(tcpm6d_sacked_bytes(&q), 200);
    /* in-flight = queued - sacked */
    CHECK_EQ(tcpm6c_retxq_bytes(&q) - tcpm6d_sacked_bytes(&q), 200);
}

/* 11: a cumulative ACK clears the sacked flags with the segments. */
static void t_ack_clears_sacked(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    for (uint32_t i = 0; i < 3; i++)
        tcpm6c_retxq_push(&q, 1000 + i * 100, 100, 0x10, i);

    tcpm6d_block_t b = { 1100, 1200 };
    tcpm6d_mark(&q, &b, 1);
    CHECK_EQ(tcpm6d_sacked_bytes(&q), 100);

    /* The hole is filled and everything up to 1300 is acknowledged. */
    CHECK_EQ(tcpm6c_retxq_ack(&q, 1300), 3);
    CHECK_EQ(q.count, 0);
    CHECK_EQ(tcpm6d_sacked_bytes(&q), 0);
}

/* 12: the receiver reports its out-of-order stash, and nothing when empty. */
static void t_build_blocks(void) {
    tcpm6d_block_t out[TCPM6D_MAX_BLOCKS];

    CHECK_EQ(tcpm6d_build_blocks(out, 0, 5000, 100), 0);   /* no stash */
    CHECK_EQ(tcpm6d_build_blocks(out, 1, 5000, 0), 0);     /* empty stash */

    CHECK_EQ(tcpm6d_build_blocks(out, 1, 5000, 250), 1);
    CHECK_EQ(out[0].start, 5000);
    CHECK_EQ(out[0].end, 5250);
}

/* 13: sequence numbers wrap; marking must use signed differences. */
static void t_mark_seq_wrap(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    /* Segment straddling the wrap: 0xFFFFFF00 .. 0x00000100 */
    tcpm6c_retxq_push(&q, 0xFFFFFF00u, 0x200, 0x10, 1);

    /* A block covering it, also straddling. */
    tcpm6d_block_t b = { 0xFFFFFF00u, 0x00000100u };
    CHECK_EQ(tcpm6d_mark(&q, &b, 1), 1);

    /* A block numerically "larger" but sequence-wise unrelated must not. */
    tcpm6c_retxq_init(&q);
    tcpm6c_retxq_push(&q, 1000, 100, 0x10, 1);
    tcpm6d_block_t far = { 0xFFFFFF00u, 0xFFFFFFF0u };
    CHECK_EQ(tcpm6d_mark(&q, &far, 1), 0);
}

int main(void) {
    printf("test_tcp_m6d (SACK, RFC 2018)\n");

    RUN(t_encode_one);
    RUN(t_encode_empty);
    RUN(t_encode_cap);
    RUN(t_roundtrip);
    RUN(t_decode_hostile);
    RUN(t_decode_mixed);
    RUN(t_mark_full);
    RUN(t_mark_partial_is_not_sacked);
    RUN(t_next_hole);
    RUN(t_sacked_bytes);
    RUN(t_ack_clears_sacked);
    RUN(t_build_blocks);
    RUN(t_mark_seq_wrap);

    printf("%s: %d/%d test(s) passed\n",
           failed ? "FAILURES" : "ALL PASS", passed, tn);
    return failed ? 1 : 0;
}
