/*
 * test_tcp_m6c.c — host unit tests for the SACK prerequisites.
 *
 * Two things SACK cannot be built without, and which this stack lacked:
 * TCP option encode/decode (data_offset was hardcoded to 5, so no option
 * had ever been emitted), and a retransmit queue deeper than one segment
 * (there was nothing to selectively retransmit *from*).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp_m6c.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ------------------------------------------------ option parsing ----- */

/* 1: a real Linux SYN-ACK option block. */
static void t_parse_typical(void) {
    /* MSS 1460, SACK-permitted, NOP, window scale 7. */
    uint8_t opts[] = { 2,4,0x05,0xB4, 4,2, 1, 3,3,7 };
    tcpm6c_opts_t o;
    CHECK_EQ(tcpm6c_parse_opts(opts, sizeof(opts), &o), 0);
    CHECK_EQ(o.mss, 1460);
    CHECK_EQ(o.sack_permitted, 1);
    CHECK_EQ(o.wscale, 7);
}

/* 2: no options at all is valid, not an error. */
static void t_parse_empty(void) {
    tcpm6c_opts_t o;
    CHECK_EQ(tcpm6c_parse_opts((const uint8_t *)"", 0, &o), 0);
    CHECK_EQ(o.mss, 0);
    CHECK_EQ(o.sack_permitted, 0);
}

/*
 * 3: the hostile cases.  These bytes come off the network before anything
 * has validated them, and a zero-length option is the classic way to spin
 * a parser forever.  If this test hangs, that is the bug.
 */
static void t_parse_hostile(void) {
    tcpm6c_opts_t o;

    /* Length byte of 0 -- must be rejected, not looped on. */
    uint8_t zero_len[] = { 2, 0, 0, 0 };
    CHECK_EQ(tcpm6c_parse_opts(zero_len, sizeof(zero_len), &o), -1);

    /* Length byte of 1 -- likewise below the 2-byte minimum. */
    uint8_t one_len[] = { 2, 1, 0, 0 };
    CHECK_EQ(tcpm6c_parse_opts(one_len, sizeof(one_len), &o), -1);

    /* Length running past the end of the block. */
    uint8_t overrun[] = { 2, 40, 0, 0 };
    CHECK_EQ(tcpm6c_parse_opts(overrun, sizeof(overrun), &o), -1);

    /* Truncated: a kind byte with no length byte. */
    uint8_t truncated[] = { 2 };
    CHECK_EQ(tcpm6c_parse_opts(truncated, sizeof(truncated), &o), -1);

    /* All-NOP padding is well formed and yields nothing. */
    uint8_t nops[] = { 1,1,1,1 };
    CHECK_EQ(tcpm6c_parse_opts(nops, sizeof(nops), &o), 0);
    CHECK_EQ(o.mss, 0);
}

/* 4: an unknown option is skipped, and later options still parse. */
static void t_parse_unknown_option(void) {
    /* Timestamps (kind 8, len 10) then MSS. */
    uint8_t opts[] = { 8,10,0,0,0,0,0,0,0,0, 2,4,0x05,0xB4 };
    tcpm6c_opts_t o;
    CHECK_EQ(tcpm6c_parse_opts(opts, sizeof(opts), &o), 0);
    CHECK_EQ(o.mss, 1460);
}

/* 5: what we emit must be parseable, 4-byte aligned, and match. */
static void t_build_roundtrip(void) {
    uint8_t buf[8];
    uint32_t n = tcpm6c_build_syn_opts(buf, 1460, 1);
    CHECK_EQ(n % 4, 0);
    CHECK_EQ(n, 8);

    tcpm6c_opts_t o;
    CHECK_EQ(tcpm6c_parse_opts(buf, n, &o), 0);
    CHECK_EQ(o.mss, 1460);
    CHECK_EQ(o.sack_permitted, 1);

    /* Every byte of padding must be a real END option, not whatever was
     * on the stack: a block that is the right LENGTH but full of garbage
     * still parses (the parser stops at the first END) and would have
     * passed a length-only check.  Assert the bytes. */
    n = tcpm6c_build_syn_opts(buf, 1460, 1);
    CHECK_EQ(buf[0], TCPOPT_MSS);
    CHECK_EQ(buf[1], 4);
    CHECK_EQ(buf[4], TCPOPT_NOP);
    CHECK_EQ(buf[5], TCPOPT_NOP);
    CHECK_EQ(buf[6], TCPOPT_SACK_PERM);
    CHECK_EQ(buf[7], 2);

    /* An odd-length block must be padded up, and the padding must be END. */
    memset(buf, 0xAA, sizeof(buf));
    n = tcpm6c_build_syn_opts(buf, 536, 0);
    CHECK_EQ(n % 4, 0);
    for (uint32_t i = 4; i < n; i++) CHECK_EQ(buf[i], TCPOPT_END);

    /* Without SACK it is MSS alone, still aligned. */
    n = tcpm6c_build_syn_opts(buf, 1460, 0);
    CHECK_EQ(n, 4);
    CHECK_EQ(tcpm6c_parse_opts(buf, n, &o), 0);
    CHECK_EQ(o.mss, 1460);
    CHECK_EQ(o.sack_permitted, 0);
}

/* 6: data_offset must count the options. */
static void t_data_offset(void) {
    CHECK_EQ(tcpm6c_data_offset(0) >> 4, 5);    /* bare header */
    CHECK_EQ(tcpm6c_data_offset(8) >> 4, 7);    /* +8 bytes = 7 words */
    CHECK_EQ(tcpm6c_data_offset(4) >> 4, 6);
}

/* -------------------------------------------- retransmit queue ------- */

/* 7: push and retire in order. */
static void t_retxq_basic(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    CHECK_EQ(q.count, 0);
    CHECK(tcpm6c_retxq_oldest(&q) == 0);

    tcpm6c_retxq_push(&q, 1000, 100, 0x10, 1);
    tcpm6c_retxq_push(&q, 1100, 100, 0x10, 2);
    CHECK_EQ(q.count, 2);
    CHECK_EQ(tcpm6c_retxq_bytes(&q), 200);
    CHECK_EQ(tcpm6c_retxq_oldest(&q)->seq, 1000);

    /* An ACK covering the first segment retires exactly one. */
    CHECK_EQ(tcpm6c_retxq_ack(&q, 1100), 1);
    CHECK_EQ(q.count, 1);
    CHECK_EQ(tcpm6c_retxq_oldest(&q)->seq, 1100);
}

/* 8: a partial ACK retires nothing -- the segment is still owed. */
static void t_retxq_partial_ack(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    tcpm6c_retxq_push(&q, 1000, 100, 0x10, 1);

    CHECK_EQ(tcpm6c_retxq_ack(&q, 1050), 0);   /* half the segment */
    CHECK_EQ(q.count, 1);
    CHECK_EQ(tcpm6c_retxq_ack(&q, 1100), 1);   /* all of it */
    CHECK_EQ(q.count, 0);
}

/* 9: the queue fills and refuses, then recovers as ACKs arrive. */
static void t_retxq_full(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    uint32_t seq = 1000;
    for (uint32_t i = 0; i < TCPM6C_RETXQ_DEPTH; i++) {
        CHECK(tcpm6c_retxq_push(&q, seq, 100, 0x10, i) >= 0);
        seq += 100;
    }
    CHECK(tcpm6c_retxq_full(&q));
    CHECK_EQ(tcpm6c_retxq_push(&q, seq, 100, 0x10, 99), -1);

    /* Retiring one frees exactly one slot. */
    CHECK_EQ(tcpm6c_retxq_ack(&q, 1100), 1);
    CHECK(!tcpm6c_retxq_full(&q));
    CHECK(tcpm6c_retxq_push(&q, seq, 100, 0x10, 99) >= 0);
    CHECK(tcpm6c_retxq_full(&q));
}

/* 10: the ring wraps without losing ordering. */
static void t_retxq_wrap(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    uint32_t seq = 1000;

    /* Three full laps of push-then-retire. */
    for (uint32_t lap = 0; lap < 3 * TCPM6C_RETXQ_DEPTH; lap++) {
        CHECK(tcpm6c_retxq_push(&q, seq, 100, 0x10, lap) >= 0);
        seq += 100;
        CHECK_EQ(tcpm6c_retxq_ack(&q, seq), 1);
        CHECK_EQ(q.count, 0);
    }

    /* And ordering still holds afterwards. */
    tcpm6c_retxq_push(&q, seq, 100, 0x10, 1);
    tcpm6c_retxq_push(&q, seq + 100, 100, 0x10, 2);
    CHECK_EQ(tcpm6c_retxq_at(&q, 0)->seq, seq);
    CHECK_EQ(tcpm6c_retxq_at(&q, 1)->seq, seq + 100);
    CHECK(tcpm6c_retxq_at(&q, 2) == 0);
}

/*
 * 11: sequence numbers wrap at 2^32.  Comparing raw values with <= would
 * stop retiring anything once a connection passes 2^31 bytes; the signed
 * difference is the only correct test.
 */
static void t_retxq_seq_wrap(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);

    /* A segment straddling the wrap point.
     *
     * The ACK must be strictly BEYOND the segment end, not equal to it:
     * with ack == end both a signed-difference test and a naive `ack < end`
     * happen to agree, so an equal ACK cannot tell the two apart.  The
     * first version of this test used exactly that value and passed while
     * the raw-comparison bug was present. */
    uint32_t seq = 0xFFFFFF00u;
    tcpm6c_retxq_push(&q, seq, 0x200, 0x10, 1);   /* ends at 0x00000100 */

    /* ack = 0x00000180: past the end, but numerically far BELOW seq. */
    CHECK_EQ(tcpm6c_retxq_ack(&q, 0x00000180u), 1);
    CHECK_EQ(q.count, 0);

    /* And an ACK before the end must not retire it. */
    tcpm6c_retxq_init(&q);
    tcpm6c_retxq_push(&q, seq, 0x200, 0x10, 1);
    CHECK_EQ(tcpm6c_retxq_ack(&q, 0x000000FFu), 0);
    CHECK_EQ(q.count, 1);

    /* The mirror case, well away from the wrap: a normal segment must not
     * be retired by an ACK that is numerically huge but sequence-wise
     * BEHIND it (the stale-ACK-after-wrap direction). */
    tcpm6c_retxq_init(&q);
    tcpm6c_retxq_push(&q, 1000, 100, 0x10, 1);
    CHECK_EQ(tcpm6c_retxq_ack(&q, 0xFFFFFF00u), 0);
    CHECK_EQ(q.count, 1);
}

/* 12: a cumulative ACK can retire several segments at once. */
static void t_retxq_bulk_ack(void) {
    tcpm6c_retxq_t q;
    tcpm6c_retxq_init(&q);
    for (uint32_t i = 0; i < 4; i++)
        tcpm6c_retxq_push(&q, 1000 + i * 100, 100, 0x10, i);

    CHECK_EQ(tcpm6c_retxq_ack(&q, 1400), 4);
    CHECK_EQ(q.count, 0);
    CHECK_EQ(tcpm6c_retxq_bytes(&q), 0);
}

int main(void) {
    printf("test_tcp_m6c (SACK prerequisites: options + retransmit queue)\n");

    RUN(t_parse_typical);
    RUN(t_parse_empty);
    RUN(t_parse_hostile);
    RUN(t_parse_unknown_option);
    RUN(t_build_roundtrip);
    RUN(t_data_offset);
    RUN(t_retxq_basic);
    RUN(t_retxq_partial_ack);
    RUN(t_retxq_full);
    RUN(t_retxq_wrap);
    RUN(t_retxq_seq_wrap);
    RUN(t_retxq_bulk_ack);

    printf("%s: %d/%d test(s) passed\n",
           failed ? "FAILURES" : "ALL PASS", passed, tn);
    return failed ? 1 : 0;
}
