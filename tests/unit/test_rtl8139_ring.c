/*
 * test_rtl8139_ring.c -- host unit tests for the RTL8139 RX ring core.
 *
 * This drives the REAL header (drivers/rtl8139/rtl8139_ring.h), not a
 * re-declaration: the whole reason that arithmetic lives in a header
 * with no hardware in it is so this test and the kernel driver share
 * one implementation (the D2 pattern tcp_x5.h/tcp_cc.h established).
 *
 * The cases below are the three bugs the 8139 is famous for, plus the
 * hostile inputs a real wire produces:
 *
 *   - the length field INCLUDES the 4-byte CRC (strip it, or every
 *     frame is 4 bytes too long);
 *   - a frame may WRAP past the end of the ring;
 *   - CAPR reads back 16 bytes behind the true offset.
 *
 * QEMU cannot be asked to deliver a wrapped frame or a CRC-error frame
 * on demand, which is exactly why these live on the host.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/rtl8139/rtl8139_ring.h"

static int failures;
static int checks;

static void ok(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void eq_u32(uint32_t got, uint32_t want, const char *what) {
    checks++;
    if (got != want) {
        printf("  FAIL: %s (got %u, want %u)\n", what, got, want);
        failures++;
    }
}

/* Build a 32-bit packet header the way the chip writes it. */
static uint32_t mk_hdr(uint16_t status, uint16_t len) {
    return ((uint32_t)len << 16) | status;
}

/* The modulus is the RING PROPER (RCR.RBLEN), never the allocation.
 * See the comment in rtl8139_ring.h: getting this wrong kills the
 * receiver permanently after ~128 packets, which is the bug the
 * test_wrap_modulus_is_the_ring_not_the_allocation case below pins. */
#define RING_LEN RTL8139_RX_WRAP_LEN

static void test_crc_stripping(void) {
    printf("-- CRC stripping (the classic 8139 bug) --\n");
    uint32_t out = 0;
    /* A 64-byte frame on the wire: 60 bytes of payload + 4 bytes FCS. */
    enum rtl8139_rx_verdict v =
        rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 64), 2048,
                            RING_LEN, &out);
    ok(v == RTL8139_RX_OK, "a good frame is accepted");
    eq_u32(out, 60, "the 4-byte FCS is removed from the length");

    /* 1518 is the maximum Ethernet frame including FCS -> 1514 payload. */
    v = rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 1518), 2048,
                            RING_LEN, &out);
    ok(v == RTL8139_RX_OK, "a full-MTU frame is accepted");
    eq_u32(out, 1514, "full-MTU payload is 1514 after stripping");
}

static void test_error_frames_dropped(void) {
    printf("-- hardware error bits --\n");
    uint32_t out = 0;
    struct { uint16_t bit; const char *name; } errs[] = {
        { RTL8139_RX_STATUS_FAE,  "frame alignment error" },
        { RTL8139_RX_STATUS_CRC,  "CRC error" },
        { RTL8139_RX_STATUS_LONG, "too-long packet" },
        { RTL8139_RX_STATUS_RUNT, "runt packet" },
        { RTL8139_RX_STATUS_ISE,  "invalid symbol" },
    };
    for (unsigned i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        uint16_t st = (uint16_t)(RTL8139_RX_STATUS_ROK | errs[i].bit);
        enum rtl8139_rx_verdict v =
            rtl8139_rx_classify(mk_hdr(st, 64), 2048, RING_LEN, &out);
        ok(v == RTL8139_RX_DROP, errs[i].name);
        eq_u32(out, 0, "a dropped frame reports zero length");
    }

    /* ROK clear means the chip did not vouch for the frame. */
    enum rtl8139_rx_verdict v =
        rtl8139_rx_classify(mk_hdr(0, 64), 2048, RING_LEN, &out);
    ok(v == RTL8139_RX_DROP, "ROK clear is a drop, not a delivery");
}

static void test_empty_and_inflight(void) {
    printf("-- empty slots and in-flight DMA --\n");
    uint32_t out = 0;
    ok(rtl8139_rx_classify(0, 2048, RING_LEN, &out) == RTL8139_RX_EMPTY,
       "an all-zero header means nothing has been written yet");
    ok(rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 0), 2048,
                           RING_LEN, &out) == RTL8139_RX_EMPTY,
       "a zero length means nothing has been committed");
    ok(rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 0xFFF0), 2048,
                           RING_LEN, &out) == RTL8139_RX_EMPTY,
       "0xFFF0 is the documented DMA-in-progress marker");
}

static void test_impossible_lengths_reset(void) {
    printf("-- lengths that cannot be real --\n");
    uint32_t out = 0;
    /* A frame must at least carry its own CRC. */
    ok(rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 4), 2048,
                           RING_LEN, &out) == RTL8139_RX_RESET,
       "length == CRC length alone is a desync, not a frame");
    ok(rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 2), 2048,
                           RING_LEN, &out) == RTL8139_RX_RESET,
       "length below the CRC length is a desync");
    /* Longer than the ring itself cannot have been stored in it. */
    ok(rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 0xFFFE), 2048,
                           RING_LEN, &out) == RTL8139_RX_RESET,
       "a length larger than the ring is a desync");
}

static void test_clamping(void) {
    printf("-- clamping to the caller's buffer --\n");
    uint32_t out = 0;
    /* A 1600-byte frame into a 128-byte buffer must clamp, never overrun. */
    enum rtl8139_rx_verdict v =
        rtl8139_rx_classify(mk_hdr(RTL8139_RX_STATUS_ROK, 1600), 128,
                            RING_LEN, &out);
    ok(v == RTL8139_RX_OK, "an oversized frame is still delivered");
    eq_u32(out, 128, "the payload is clamped to the buffer size");
}

static void test_advance_and_alignment(void) {
    printf("-- advancing the read cursor --\n");
    /* 64-byte frame at offset 0: 4 header + 64 = 68, already aligned. */
    eq_u32(rtl8139_rx_advance(0, 64, RING_LEN), 68, "aligned advance");
    /* 62 bytes: 4 + 62 = 66 -> rounds up to 68. */
    eq_u32(rtl8139_rx_advance(0, 62, RING_LEN), 68,
           "advance rounds up to the 4-byte boundary");
    eq_u32(rtl8139_rx_advance(0, 61, RING_LEN), 68, "61 -> 68");
    eq_u32(rtl8139_rx_advance(0, 65, RING_LEN), 72, "65 -> 72");

    /* The wrap: a frame that ends past the ring end comes back to the front. */
    uint32_t near_end = RING_LEN - 8;
    uint32_t next = rtl8139_rx_advance(near_end, 64, RING_LEN);
    ok(next < RING_LEN, "the advanced offset stays inside the ring");
    eq_u32(next, (near_end + 68) % RING_LEN, "the wrap is a modulo");
}

static void test_capr_bias(void) {
    printf("-- the CAPR -16 bias --\n");
    eq_u32(rtl8139_capr_from_offset(16), 0, "offset 16 programs CAPR 0");
    eq_u32(rtl8139_capr_from_offset(68), 52, "offset 68 programs CAPR 52");
    /* The interesting case: offset 0 must wrap to 0xFFF0, not underflow. */
    eq_u32(rtl8139_capr_from_offset(0), 0xFFF0,
           "offset 0 wraps to 0xFFF0 rather than going negative");

    /* Round-trip: converting back must recover the offset. */
    uint32_t offs[] = { 0, 4, 16, 68, 1024, 8191 };
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        uint16_t capr = rtl8139_capr_from_offset(offs[i]);
        uint32_t back = rtl8139_offset_from_capr(capr, RING_LEN);
        eq_u32(back, offs[i] % RING_LEN, "CAPR round-trips to the offset");
    }
}

static void test_header_read_wraps(void) {
    printf("-- reading a header that straddles the ring end --\n");
    static uint8_t ring[RING_LEN];
    memset(ring, 0, sizeof(ring));

    /* Put a header's 4 bytes across the boundary: last 2 + first 2. */
    uint32_t off = RING_LEN - 2;
    uint32_t want = mk_hdr(RTL8139_RX_STATUS_ROK, 64);
    for (uint32_t i = 0; i < 4; i++) {
        ring[(off + i) % RING_LEN] = (uint8_t)((want >> (8 * i)) & 0xFF);
    }
    eq_u32(rtl8139_rx_header(ring, RING_LEN, off), want,
           "a split header is reassembled correctly");

    /* And the ordinary, non-split case. */
    memset(ring, 0, sizeof(ring));
    for (uint32_t i = 0; i < 4; i++) {
        ring[100 + i] = (uint8_t)((want >> (8 * i)) & 0xFF);
    }
    eq_u32(rtl8139_rx_header(ring, RING_LEN, 100), want,
           "a contiguous header reads back little-endian");
}

static void test_full_ring_walk(void) {
    printf("-- walking a ring full of frames without desync --\n");
    static uint8_t ring[RING_LEN];
    memset(ring, 0, sizeof(ring));

    /* Lay down 20 frames of varying size, then walk them back. */
    const uint32_t sizes[] = { 64, 128, 65, 1518, 60, 301, 64, 802,
                               64, 64, 129, 1000, 77, 64, 512, 64,
                               333, 64, 1518, 64 };
    const unsigned n = sizeof(sizes) / sizeof(sizes[0]);
    uint32_t off = 0;
    uint32_t placed[32];
    for (unsigned i = 0; i < n; i++) {
        placed[i] = off;
        uint32_t hdr = mk_hdr(RTL8139_RX_STATUS_ROK, (uint16_t)sizes[i]);
        for (uint32_t b = 0; b < 4; b++) {
            ring[(off + b) % RING_LEN] = (uint8_t)((hdr >> (8 * b)) & 0xFF);
        }
        off = rtl8139_rx_advance(off, sizes[i], RING_LEN);
    }

    off = 0;
    for (unsigned i = 0; i < n; i++) {
        eq_u32(off, placed[i], "the walk lands on each frame in turn");
        uint32_t hdr = rtl8139_rx_header(ring, RING_LEN, off);
        uint32_t out = 0;
        enum rtl8139_rx_verdict v =
            rtl8139_rx_classify(hdr, 2048, RING_LEN, &out);
        ok(v == RTL8139_RX_OK, "each laid-down frame classifies OK");
        eq_u32(out, sizes[i] - 4, "each payload is the size minus the FCS");
        off = rtl8139_rx_advance(off, sizes[i], RING_LEN);
    }
}

/*
 * The regression this file exists for.
 *
 * MEASURED, not theorised: the first version of the driver used the
 * ALLOCATION size (8192+16+1500 = 9708) as the wrap modulus.  Under
 * QEMU that booted fine -- DHCP got a lease, ARP resolved, ICMP and
 * sixteen TCP connections all passed -- and then, after roughly 128
 * packets, the receiver stopped dead and every subsequent ARP timed
 * out.  No error bit is set when this happens: writing a CAPR beyond
 * the ring proper simply convinces the chip its buffer is full, so
 * CMD.BUFE never clears again.
 *
 * The arithmetic below is what separates the two numbers, so a future
 * edit that collapses them back into one fails here in milliseconds
 * instead of after two minutes of QEMU and a puzzling silence.
 */
static void test_wrap_modulus_is_the_ring_not_the_allocation(void) {
    printf("-- the wrap modulus is the ring, not the allocation --\n");

    ok(RTL8139_RX_WRAP_LEN == 8192,
       "the ring proper is the 8 KiB RCR.RBLEN=00 selects");
    ok(RTL8139_RX_BUF_LEN > RTL8139_RX_WRAP_LEN,
       "the allocation is larger than the ring (header slack + WRAP pad)");
    ok(RTL8139_RX_BUF_LEN - RTL8139_RX_WRAP_LEN >= 1500,
       "the pad can absorb a full-MTU overrun past the ring end");

    /* THE LOAD-BEARING ASSERTION.
     *
     * RING_LEN is whatever modulus the rest of this file -- and, by
     * the same constant, the driver -- actually uses.  If a future
     * edit points it at the allocation instead of the ring proper,
     * every offset handed to CAPR can exceed what the chip accepts,
     * and the receiver dies permanently.  Assert the relationship
     * itself rather than restating the constant, so the check cannot
     * be satisfied by a copy of the wrong number. */
    ok(RING_LEN <= RTL8139_RX_WRAP_LEN,
       "the modulus in use never exceeds the ring the chip wraps at");
    ok(RING_LEN == RTL8139_RX_WRAP_LEN,
       "the modulus in use IS the ring proper (not the allocation)");

    /* Every advance must land inside the RING, never in the pad: the
     * pad is somewhere the CHIP may write, not somewhere our cursor
     * may point.  Driven through RING_LEN for the same reason. */
    for (uint32_t off = RING_LEN - 200; off < RING_LEN; off += 4) {
        uint32_t next = rtl8139_rx_advance(off, 1518, RING_LEN);
        ok(next < RTL8139_RX_WRAP_LEN,
           "an advance near the end wraps into the ring, not the pad");
    }

    /* And a CAPR derived from any in-ring offset must come back inside
     * the ring the chip knows about. */
    for (uint32_t off = 0; off < RING_LEN; off += 256) {
        uint32_t back = rtl8139_offset_from_capr(
            rtl8139_capr_from_offset(off), RING_LEN);
        ok(back < RTL8139_RX_WRAP_LEN,
           "a programmed CAPR always maps back inside the ring");
    }

    /* And the CAPR that offset produces must be inside the ring too. */
    for (uint32_t off = 0; off < RTL8139_RX_WRAP_LEN; off += 512) {
        uint16_t capr = rtl8139_capr_from_offset(off);
        uint32_t back = rtl8139_offset_from_capr(capr, RTL8139_RX_WRAP_LEN);
        eq_u32(back, off, "CAPR round-trips within the ring proper");
    }
}

int main(void) {
    printf("test_rtl8139_ring: RTL8139 RX ring decision core\n");
    test_wrap_modulus_is_the_ring_not_the_allocation();
    test_crc_stripping();
    test_error_frames_dropped();
    test_empty_and_inflight();
    test_impossible_lengths_reset();
    test_clamping();
    test_advance_and_alignment();
    test_capr_bias();
    test_header_read_wraps();
    test_full_ring_walk();

    printf("test_rtl8139_ring: %d passed, %d failed (%d total)\n",
           checks - failures, failures, checks);
    return failures ? 1 : 0;
}
