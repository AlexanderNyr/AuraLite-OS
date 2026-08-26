#ifndef AURALITE_DRIVERS_RTL8139_RTL8139_RING_H
#define AURALITE_DRIVERS_RTL8139_RTL8139_RING_H

#include <stdint.h>

/*
 * rtl8139_ring.h -- the RX ring decision core, as pure C.
 *
 * The D2 pattern this tree already uses for TCP (tcp_x5.h, tcp_cc.h,
 * dualstack.h): the arithmetic that is easy to get wrong lives in a
 * header with no hardware in it, so a HOST test can drive it through
 * the cases QEMU cannot be made to produce on demand -- a frame that
 * wraps the ring end, a CRC-error frame, a length field that lies.
 *
 * Three things about the 8139 receive path are genuinely tricky, and
 * all three are decided here rather than inside the IRQ handler:
 *
 *  1. THE PACKET HEADER.  Each frame in the ring is preceded by a
 *     4-byte header: 16-bit status, then 16-bit length.  The length
 *     INCLUDES the 4-byte Ethernet CRC, which must be subtracted
 *     before the frame is handed upwards.  Forgetting this is the
 *     classic 8139 bug: every frame arrives 4 bytes too long and
 *     upper layers see trailing garbage.
 *
 *  2. THE WRAP.  The ring is a flat byte buffer, and a frame may
 *     start near the end and continue past it.  The driver allocates
 *     RX_BUF_LEN = 8192 + 16 + 1500 so the hardware may run off the
 *     end into the pad (WRAP=1) instead of splitting the frame; the
 *     READ offset must still be taken modulo the ring size.
 *
 *  3. CAPR IS BIASED BY 16.  The "current address of packet read"
 *     register reads back 16 bytes BEHIND the true read offset --
 *     a documented hardware quirk, not a driver bug.  Every write
 *     must subtract 16 and every read must add it.  Getting the sign
 *     wrong desynchronises the ring after the first packet, which
 *     looks exactly like random packet loss.
 *
 * Nothing here touches a register, so the same object is exercised by
 * tests/unit/test_rtl8139_ring.c on the host and compiled into the
 * kernel driver.
 */

/* RX packet-header status bits (the low 16 bits of the 4-byte header). */
#define RTL8139_RX_STATUS_ROK   (1u << 0)   /* receive OK               */
#define RTL8139_RX_STATUS_FAE   (1u << 1)   /* frame alignment error    */
#define RTL8139_RX_STATUS_CRC   (1u << 2)   /* CRC error                */
#define RTL8139_RX_STATUS_LONG  (1u << 3)   /* runt/too-long packet     */
#define RTL8139_RX_STATUS_RUNT  (1u << 4)   /* runt packet received     */
#define RTL8139_RX_STATUS_ISE   (1u << 5)   /* invalid symbol error     */

/* The 4-byte per-packet header the chip writes ahead of each frame. */
#define RTL8139_RX_HDR_LEN      4
/* The Ethernet FCS the length field includes but the stack must not see. */
#define RTL8139_RX_CRC_LEN      4
/* CAPR's documented -16 bias. */
#define RTL8139_CAPR_BIAS       16
/* Frames are advanced to a 4-byte boundary after each packet. */
#define RTL8139_RX_ALIGN        4

/*
 * TWO SIZES, AND CONFUSING THEM KILLS THE RECEIVER.
 *
 * This distinction is the single most dangerous thing about the 8139
 * ring, so it is spelled out here rather than left to a reader of the
 * datasheet.  It was measured on this tree, not theorised: a first
 * draft used one size for both and RX died permanently after roughly
 * 128 packets -- DHCP and the first pings worked, then every ARP
 * timed out forever.
 *
 *   RTL8139_RX_WRAP_LEN (8192) is the RING PROPER: the size RCR.RBLEN
 *   selects, and therefore the modulus the CHIP uses when it wraps.
 *   Every offset the driver computes, and every CAPR it programs,
 *   must be taken modulo THIS.  Hand the chip an offset beyond it and
 *   the hardware concludes the buffer is full: CMD.BUFE never clears
 *   again and the receiver is dead until the next reset, with no
 *   error bit set anywhere to say why.
 *
 *   RTL8139_RX_BUF_LEN (8192 + 16 + 1500) is the ALLOCATION: the ring
 *   proper, plus 16 bytes of header slack, plus a 1500-byte pad the
 *   chip is allowed to overrun into because RCR.WRAP is set.  WRAP
 *   lets a frame that starts near the end run PAST it in one piece
 *   instead of being split in two, so the reader never has to stitch
 *   halves together -- but the bytes it writes there live outside the
 *   ring proper, which is exactly why the modulus is not this number.
 */
#define RTL8139_RX_WRAP_LEN     8192
#define RTL8139_RX_BUF_LEN      (RTL8139_RX_WRAP_LEN + 16 + 1500)

/* Verdicts rtl8139_rx_classify() can return. */
enum rtl8139_rx_verdict {
    RTL8139_RX_OK = 0,      /* deliver: *out_len holds the payload length */
    RTL8139_RX_DROP,        /* hardware flagged an error: skip, keep going */
    RTL8139_RX_EMPTY,       /* header not written yet -- stop draining     */
    RTL8139_RX_RESET        /* length is impossible: ring desynchronised   */
};

/* Read the little-endian 32-bit packet header at `off` in a ring of
 * `ring_len` bytes.  The read wraps, because a header can straddle the
 * ring end just like a payload can. */
static inline uint32_t rtl8139_rx_header(const uint8_t *ring, uint32_t ring_len,
                                         uint32_t off)
{
    uint32_t h = 0;
    for (uint32_t i = 0; i < RTL8139_RX_HDR_LEN; i++) {
        h |= (uint32_t)ring[(off + i) % ring_len] << (8u * i);
    }
    return h;
}

/*
 * Classify one ring slot.
 *
 * `hdr`     -- the 32-bit header read by rtl8139_rx_header().
 * `max_len` -- the largest payload the caller can accept (its buffer).
 * `out_len` -- payload length WITHOUT the CRC, valid when OK is returned.
 *
 * The length sanity check is deliberately strict: a frame shorter than
 * a bare Ethernet header plus CRC, or longer than the ring itself,
 * means the read offset no longer points at a real header.  Continuing
 * to walk from there reads garbage forever, so the caller is told to
 * RESET rather than to skip -- an honest "I am lost" beats a silent
 * infinite drain.
 */
static inline enum rtl8139_rx_verdict
rtl8139_rx_classify(uint32_t hdr, uint32_t max_len, uint32_t ring_len,
                    uint32_t *out_len)
{
    uint32_t status = hdr & 0xFFFFu;
    uint32_t len    = (hdr >> 16) & 0xFFFFu;

    if (out_len) *out_len = 0;

    /* The chip zeroes the header until it has committed a frame; a
     * length of 0 with no status bits means "nothing here yet".  The
     * 0xFFF0 probe is the documented "DMA still in progress" marker. */
    if (hdr == 0 || len == 0 || len == 0xFFF0u) return RTL8139_RX_EMPTY;

    /* A frame must at least carry its own CRC, and cannot exceed the
     * ring.  Anything else means we are not looking at a header. */
    if (len <= RTL8139_RX_CRC_LEN || len > ring_len) return RTL8139_RX_RESET;

    if (!(status & RTL8139_RX_STATUS_ROK)) return RTL8139_RX_DROP;
    if (status & (RTL8139_RX_STATUS_FAE | RTL8139_RX_STATUS_CRC |
                  RTL8139_RX_STATUS_LONG | RTL8139_RX_STATUS_RUNT |
                  RTL8139_RX_STATUS_ISE)) {
        return RTL8139_RX_DROP;
    }

    uint32_t payload = len - RTL8139_RX_CRC_LEN;   /* strip the FCS (bug #1) */
    if (payload > max_len) payload = max_len;      /* clamp, never overrun   */
    if (out_len) *out_len = payload;
    return RTL8139_RX_OK;
}

/* Advance the read offset past a frame of `len` bytes (the header's
 * own length field, CRC included), honouring the 4-byte alignment the
 * chip applies, and wrapping (bug #2). */
static inline uint32_t rtl8139_rx_advance(uint32_t off, uint32_t len,
                                          uint32_t ring_len)
{
    uint32_t next = off + len + RTL8139_RX_HDR_LEN;
    next = (next + (RTL8139_RX_ALIGN - 1)) & ~(uint32_t)(RTL8139_RX_ALIGN - 1);
    return next % ring_len;
}

/* Convert a read offset into the value CAPR must be programmed with
 * (bug #3).  Written as an unsigned 16-bit wrap so the subtraction is
 * correct even when off < 16. */
static inline uint16_t rtl8139_capr_from_offset(uint32_t off)
{
    return (uint16_t)((off - RTL8139_CAPR_BIAS) & 0xFFFFu);
}

/* The inverse: the true read offset implied by a CAPR readback.
 *
 * The +16 must wrap at SIXTEEN BITS before the ring modulo is taken,
 * because CAPR is a 16-bit register and the forward direction wrapped
 * there too.  Offset 0 programs CAPR 0xFFF0; adding 16 to that in
 * 32-bit arithmetic yields 65536, and 65536 % 9708 is 7288 -- a
 * plausible-looking offset pointing at nothing.  The host test caught
 * exactly this on the offsets below 16 (the wrap cases), which is why
 * the mask is here and not left implicit. */
static inline uint32_t rtl8139_offset_from_capr(uint16_t capr, uint32_t ring_len)
{
    uint32_t off = ((uint32_t)capr + RTL8139_CAPR_BIAS) & 0xFFFFu;
    return off % ring_len;
}

#endif /* AURALITE_DRIVERS_RTL8139_RTL8139_RING_H */
