/*
 * test_r8169_desc.c -- host unit tests for the RTL8169 descriptor core.
 *
 * This drives the REAL header (drivers/r8169/r8169_desc.h), not a
 * re-declaration: the whole reason the register map and descriptor
 * arithmetic live in a header with no hardware in it is so this test
 * and the kernel driver share one implementation (the D2 pattern
 * rtl8139_ring.h/tcp_x5.h established).
 *
 * The facts below are pinned from the RTL8169 datasheet (Rev. 1.21)
 * cross-checked against Linux's r8169 driver, and each is a bug the
 * 8169 is famous for when it is mis-pinned:
 *
 *   - the RX length field is 14 bits (0x3fff = 16383) and the armed
 *     buffer must never exceed what the field can express;
 *   - the length INCLUDES the 4-byte FCS (strip it, or every frame
 *     is 4 bytes too long);
 *   - the OWN-bit hand-off: touching a descriptor the NIC owns reads
 *     garbage; rearming must set OWN and preserve EOR;
 *   - the ring wraps modulo the descriptor COUNT, while exactly one
 *     EOR bit marks the physical ring end.
 *
 * QEMU cannot be asked to produce any of these on demand -- there is
 * no 8169 model at all -- which is exactly why they live on the host.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/r8169/r8169_desc.h"

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

/* Build an RX opts1 the way the NIC writes it: OWN clear (driver owns),
 * FS|LS set, the given status bits, and the length field. */
static uint32_t mk_rx(uint32_t status, uint32_t len) {
    return R8169_DESC_FS | R8169_DESC_LS | status |
           (len & R8169_RX_LEN_MASK);
}

/*
 * The regression this file exists for.
 *
 * MEASURED from the datasheet, not theorised: the 8169 is descriptor
 * based, and its two load-bearing numbers are the 14-bit length field
 * (0x3fff) and the 256-byte descriptor alignment.  A first draft that
 * picks the 8139's ring-buffer mental model -- a 32-bit buffer base, an
 * 8 KiB ring, no OWN hand-off -- compiles clean and boots, and then the
 * receiver either truncates the size hint or never sees the ring-end.
 * The assertions below are the relationship, not a copy of the number,
 * so an edit that collapses the field width or the alignment back to
 * the wrong value fails here in milliseconds instead of on metal.
 */
static void test_pinned_surface(void) {
    printf("-- the pinned surface (datasheet facts) --\n");

    /* The length field is 14 bits; the armed buffer is the field's max. */
    ok(R8169_RX_LEN_MASK == 0x3fff,
       "the RX length field is bits 13:0 (0x3fff)");
    ok(R8169_RX_BUF_SZ == 16383,
       "the armed RX buffer is 16383 (Linux's rx_buf_sz)");
    ok(R8169_RX_BUF_SZ <= R8169_RX_LEN_MASK,
       "the armed buffer never exceeds what the field can express");
    ok(R8169_RX_BUF_SZ == R8169_RX_LEN_MASK,
       "the armed buffer IS the field max (no silent truncation room)");

    /* Descriptors are 4 double words; the ring is within the 1024 cap. */
    ok(sizeof(struct r8169_tx_desc) == R8169_DESC_SIZE,
       "a TX descriptor is 16 bytes");
    ok(sizeof(struct r8169_rx_desc) == R8169_DESC_SIZE,
       "an RX descriptor is 16 bytes");
    ok(R8169_NUM_RX_DESC <= R8169_MAX_DESC,
       "the RX ring stays within the datasheet's 1024-descriptor cap");
    ok(R8169_NUM_TX_DESC <= R8169_MAX_DESC,
       "the TX ring stays within the 1024-descriptor cap");
    ok(R8169_RX_RING_BYTES == R8169_NUM_RX_DESC * R8169_DESC_SIZE,
       "the RX ring byte count is count * descriptor size");
    ok(R8169_TX_RING_BYTES == R8169_NUM_TX_DESC * R8169_DESC_SIZE,
       "the TX ring byte count is count * descriptor size");

    /* The descriptor base must be 256-byte aligned (datasheet TNPDS/
     * RDSAR: "(256-byte alignment)"). */
    ok(R8169_DESC_ALIGN == 256, "the descriptor alignment is 256 bytes");
}

static void test_bit_positions(void) {
    printf("-- the opts1 bit positions (OWN/EOR/FS/LS) --\n");
    eq_u32(R8169_DESC_OWN, 1u << 31, "OWN is bit 31");
    eq_u32(R8169_DESC_EOR, 1u << 30, "EOR is bit 30");
    eq_u32(R8169_DESC_FS,  1u << 29, "FS is bit 29");
    eq_u32(R8169_DESC_LS,  1u << 28, "LS is bit 28");
    eq_u32(R8169_RX_FOVF,  1u << 23, "RxFOVF is bit 23");
    eq_u32(R8169_RX_RWT,   1u << 22, "RxRWT is bit 22");
    eq_u32(R8169_RX_RES,   1u << 21, "RxRES is bit 21");
    eq_u32(R8169_RX_RUNT,  1u << 20, "RxRUNT is bit 20");
    eq_u32(R8169_RX_CRC,   1u << 19, "RxCRC is bit 19");
    eq_u32(R8169_FCS_LEN, 4, "the FCS is 4 bytes");
}

static void test_register_widths(void) {
    printf("-- per-register access widths --\n");
    /* ISR/IMR are 16-bit; reading them as 32-bit spans a neighbour. */
    eq_u32(r8169_reg_width(R8169_ISR), R8169_W16,
           "the interrupt status register is 16-bit");
    eq_u32(r8169_reg_width(R8169_IMR), R8169_W16,
           "the interrupt mask register is 16-bit");
    eq_u32(r8169_reg_width(R8169_CR), R8169_W8,
           "the command register is 8-bit");
    eq_u32(r8169_reg_width(R8169_TPPOLL), R8169_W8,
           "the TX poll register is 8-bit");
    eq_u32(r8169_reg_width(R8169_TCR), R8169_W32,
           "the TX config register is 32-bit");
    eq_u32(r8169_reg_width(R8169_RCR), R8169_W32,
           "the RX config register is 32-bit");
    eq_u32(r8169_reg_width(R8169_RDSAR_LOW), R8169_W64,
           "the RX descriptor base is a 64-bit register");
    eq_u32(r8169_reg_width(R8169_TNPDS_LOW), R8169_W64,
           "the TX descriptor base is a 64-bit register");
}

static void test_crc_stripping(void) {
    printf("-- CRC stripping (the 8169 version of the 8139 bug) --\n");
    uint32_t out = 0;
    /* A 64-byte frame on the wire: 60 payload + 4 FCS. */
    enum r8169_rx_verdict v = r8169_rx_classify(mk_rx(0, 64), &out);
    ok(v == R8169_RX_OK, "a good frame is accepted");
    eq_u32(out, 60, "the 4-byte FCS is removed from the length");

    /* 1518 is the max Ethernet frame incl. FCS -> 1514 payload. */
    v = r8169_rx_classify(mk_rx(0, 1518), &out);
    ok(v == R8169_RX_OK, "a full-MTU frame is accepted");
    eq_u32(out, 1514, "full-MTU payload is 1514 after stripping");

    /* The field's own maximum must still classify (16383 -> 16379). */
    v = r8169_rx_classify(mk_rx(0, 0x3fff), &out);
    ok(v == R8169_RX_OK, "the field-maximum length is accepted");
    eq_u32(out, 0x3fff - 4, "the field max strips to max minus the FCS");
}

static void test_error_frames_dropped(void) {
    printf("-- hardware error bits --\n");
    uint32_t out = 0;
    struct { uint32_t bit; const char *name; } errs[] = {
        { R8169_RX_RES,  "receive error summary" },
        { R8169_RX_RWT,  "receive watchdog timeout" },
        { R8169_RX_FOVF, "RX FIFO overflow" },
        { R8169_RX_CRC,  "CRC error" },
        { R8169_RX_RUNT, "runt packet" },
    };
    for (unsigned i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        enum r8169_rx_verdict v = r8169_rx_classify(mk_rx(errs[i].bit, 64),
                                                    &out);
        ok(v == R8169_RX_DROP, errs[i].name);
        eq_u32(out, 0, "a dropped frame reports zero length");
    }
}

static void test_ownership_handoff(void) {
    printf("-- the OWN-bit hand-off --\n");
    uint32_t out = 0;
    /* OWN set means the NIC still owns the slot: stop, do not read. */
    enum r8169_rx_verdict v =
        r8169_rx_classify(R8169_DESC_OWN | mk_rx(0, 64), &out);
    ok(v == R8169_RX_OWNED, "a NIC-owned RX descriptor stops the drain");
    ok(!r8169_rx_owned(mk_rx(0, 64)),
       "OWN clear means the driver owns the descriptor");
    ok(r8169_tx_owned(r8169_tx_opts1(64, 0)),
       "a handed-over TX descriptor has OWN set (the chip owns it)");
}

static void test_fragmented_frames_dropped(void) {
    printf("-- fragmented frames --\n");
    uint32_t out = 0;
    /* A frame is FS|LS BOTH set; any other combination is a fragment
     * of a larger packet the legacy path cannot stitch (Linux's
     * rtl8169_fragmented_frame). */
    uint32_t base = mk_rx(0, 64);
    ok(!r8169_rx_fragmented(base), "FS|LS both set is a whole frame");
    ok(r8169_rx_fragmented(base & ~R8169_DESC_FS), "FS clear is a fragment");
    ok(r8169_rx_fragmented(base & ~R8169_DESC_LS), "LS clear is a fragment");
    ok(r8169_rx_fragmented(base & ~(R8169_DESC_FS | R8169_DESC_LS)),
       "FS|LS both clear is a fragment");
    enum r8169_rx_verdict v =
        r8169_rx_classify(base & ~R8169_DESC_FS, &out);
    ok(v == R8169_RX_DROP, "a fragmented frame is dropped, not delivered");
}

static void test_impossible_lengths_reset(void) {
    printf("-- lengths that cannot be real --\n");
    uint32_t out = 0;
    /* A frame must at least carry its own CRC. */
    ok(r8169_rx_classify(mk_rx(0, 4), &out) == R8169_RX_RESET,
       "length == CRC length alone is a desync, not a frame");
    ok(r8169_rx_classify(mk_rx(0, 2), &out) == R8169_RX_RESET,
       "length below the CRC length is a desync");
}

static void test_advance_wraps_on_count(void) {
    printf("-- advancing the RX cursor wraps on the count --\n");
    eq_u32(r8169_rx_advance(0, R8169_NUM_RX_DESC), 1, "0 -> 1");
    eq_u32(r8169_rx_advance(R8169_NUM_RX_DESC - 1, R8169_NUM_RX_DESC), 0,
           "the last descriptor wraps to the first (modulo the COUNT)");
    /* The wrap is the count, never a byte offset into the ring. */
    eq_u32(r8169_rx_advance(R8169_NUM_RX_DESC - 1, R8169_NUM_RX_DESC),
           (R8169_NUM_RX_DESC) % R8169_NUM_RX_DESC,
           "the wrap is count-modulo, the EOR bit is the chip's");
}

static void test_tx_opts1_build(void) {
    printf("-- building a TX descriptor --\n");
    /* Single-fragment TX: OWN|FS|LS|len, EOR only on the last. */
    uint32_t o = r8169_tx_opts1(64, 0);
    ok(o & R8169_DESC_OWN, "TX hands ownership to the chip");
    ok(o & R8169_DESC_FS,  "TX sets FS");
    ok(o & R8169_DESC_LS,  "TX sets LS");
    eq_u32(o & R8169_TX_LEN_MASK, 64, "the length sits in bits 15:0");
    ok(!(o & R8169_DESC_EOR), "EOR is NOT set on a non-last descriptor");

    o = r8169_tx_opts1(64, 1);
    ok(o & R8169_DESC_EOR, "EOR IS set on the ring's last descriptor");

    /* The length is masked to 16 bits. */
    eq_u32(r8169_tx_opts1(0x10041, 0) & R8169_TX_LEN_MASK, 0x41,
           "the TX length is masked to the 16-bit field");
}

static void test_rearm_preserves_eor(void) {
    printf("-- rearming preserves the ring-end marker --\n");
    /* The mark-to-ASIC path: OWN set, EOR preserved, buffer size in
     * the low bits.  Dropping the EOR term here is the bug that breaks
     * the ring-end the chip depends on. */
    eq_u32(r8169_rx_rearm_opts1(0),
           R8169_DESC_OWN | (R8169_RX_BUF_SZ & R8169_RX_LEN_MASK),
           "rearming without EOR sets OWN and the buffer size");
    eq_u32(r8169_rx_rearm_opts1(R8169_DESC_EOR),
           R8169_DESC_OWN | R8169_DESC_EOR |
           (R8169_RX_BUF_SZ & R8169_RX_LEN_MASK),
           "rearming the last descriptor KEEPS the EOR bit");

    /* Freeing a TX descriptor clears OWN but keeps EOR where it was. */
    eq_u32(r8169_tx_free_opts1(0), 0, "freeing a non-last TX clears OWN");
    eq_u32(r8169_tx_free_opts1(R8169_DESC_EOR), R8169_DESC_EOR,
           "freeing the last TX descriptor keeps EOR");
}

static void test_desc_base_split_and_alignment(void) {
    printf("-- descriptor base: 64-bit split and 256-byte alignment --\n");
    /* The base is a 64-bit address in low+high registers; a driver
     * that programs only the low register is the 8139's truncation
     * bug in disguise. */
    uint64_t base = 0x0000000123456700ULL;
    ok(r8169_desc_base_is_aligned(base), "a 256-byte-aligned base passes");
    ok(!r8169_desc_base_is_aligned(base + 128),
       "a 128-byte-offset base is refused");
    eq_u32(r8169_desc_base_lo(base), 0x23456700u, "the low register value");
    eq_u32(r8169_desc_base_hi(base), 0x00000001u, "the high register value");

    /* A base above 4 GiB must still split correctly -- the 8169 has no
     * 32-bit DMA wall, so the HIGH register must carry the real bits. */
    uint64_t high = 0x0000000200000000ULL;
    eq_u32(r8169_desc_base_hi(high), 2, "the high register is real, not 0");
}

int main(void) {
    printf("test_r8169_desc: RTL8169 descriptor decision core\n");
    test_pinned_surface();
    test_bit_positions();
    test_register_widths();
    test_crc_stripping();
    test_error_frames_dropped();
    test_ownership_handoff();
    test_fragmented_frames_dropped();
    test_impossible_lengths_reset();
    test_advance_wraps_on_count();
    test_tx_opts1_build();
    test_rearm_preserves_eor();
    test_desc_base_split_and_alignment();

    printf("test_r8169_desc: %d passed, %d failed (%d total)\n",
           checks - failures, failures, checks);
    return failures ? 1 : 0;
}
