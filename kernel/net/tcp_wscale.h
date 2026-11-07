#ifndef AURALITE_NET_TCP_WSCALE_H
#define AURALITE_NET_TCP_WSCALE_H

#include <stdint.h>

/*
 * TCP window scaling (RFC 7323 s2.2 / s2.3) — RESIDUE3 C7.
 *
 * The 16-bit window field caps a connection at 65535 bytes in flight.  With
 * a 200 ms path that is 65535 * 8 / 0.2 = 2.6 Mbit/s no matter how fast the
 * link is, which is why every modern stack negotiates a shift count in the
 * handshake: both sides then read the window field as "field << shift".
 *
 * This header is pure: no kernel state, no I/O.  tcp.c applies the results,
 * and tests/unit/test_tcp_wscale.c pins them, including the malformed cases
 * a parser on network bytes has to survive.
 *
 * THE RULE THAT MATTERS MOST: the shift is offered in the SYN and answered in
 * the SYN-ACK, and it is NEVER negotiated again for that connection.  A
 * window field arriving on a later segment with a peer that never offered a
 * shift must be read unscaled — scaling it would invent a window the peer
 * never granted.
 */

/* RFC 7323: the shift count is 14 bits at most.  A peer that sends more is
 * misbehaving; clamping keeps every shift computation in range. */
#define TCP_WS_SHIFT_MAX 14u

#define TCP_WS_KIND      3u   /* option kind */
#define TCP_WS_OPT_LEN   3u   /* kind, length, shift */

/* What a peer's option block said about window scaling. */
typedef struct {
    uint8_t offered;   /* 1 = the peer put a WSCALE option in its SYN */
    uint8_t shift;     /* the shift it asked us to use (already clamped) */
} tcp_ws_opt_t;

static inline void tcp_ws_opt_init(tcp_ws_opt_t *o) {
    o->offered = 0;
    o->shift = 0;
}

/*
 * Walk a TCP option block and report the window-scale option.
 *
 * Returns 0 if the block was well-formed (even when no WSCALE was found, in
 * which case `out->offered` stays 0), -1 if it was malformed.  The caller
 * uses whatever was found: a peer that fills the block with garbage still
 * gets its window read unscaled, which is the safe reading.
 *
 * Every iteration consumes at least one byte.  That is not defensive
 * decoration: a zero-length option is the classic way to spin a parser
 * forever, and this runs before anything has been validated.
 */
static inline int tcp_ws_parse(const uint8_t *p, uint32_t len,
                               tcp_ws_opt_t *out) {
    tcp_ws_opt_init(out);
    uint32_t i = 0;
    int well_formed = 1;
    while (i < len) {
        uint8_t kind = p[i];
        if (kind == 0u) break;                  /* End of option list */
        if (kind == 1u) { i++; continue; }      /* NOP */
        if (i + 1u >= len) { well_formed = 0; break; }
        uint8_t olen = p[i + 1u];
        if (olen < 2u) { well_formed = 0; i++; continue; }
        if (i + olen > len) { well_formed = 0; break; }
        if (kind == TCP_WS_KIND) {
            if (olen == TCP_WS_OPT_LEN) {
                uint8_t s = p[i + 2u];
                if (s > TCP_WS_SHIFT_MAX) s = TCP_WS_SHIFT_MAX;
                out->offered = 1;
                out->shift = s;
            } else {
                /* Right kind, wrong length: not a scale we may act on. */
                well_formed = 0;
            }
        }
        i += olen;
    }
    return well_formed ? 0 : -1;
}

/*
 * Write a WSCALE option into `buf`, padded to a 4-byte boundary with NOPs so
 * the following options (and the header length) stay aligned.
 *
 * Returns the number of bytes written, or 0 when there is not enough room —
 * in which case the caller must forget the option rather than emit a
 * half-built one, because a truncated option block is worse than none.
 */
static inline uint32_t tcp_ws_emit(uint8_t *buf, uint32_t cap, uint8_t shift) {
    if (shift > TCP_WS_SHIFT_MAX) shift = TCP_WS_SHIFT_MAX;
    if (cap < 4u) return 0;
    uint32_t i = 0;
    buf[i++] = TCP_WS_KIND;
    buf[i++] = TCP_WS_OPT_LEN;
    buf[i++] = shift;
    buf[i++] = 1u;                     /* NOP padding to the 4-byte boundary */
    return i;
}

/* Window field -> real window: `field << shift`, saturating.  A shift of 0
 * is the identity, which is exactly what an unscaled peer means. */
static inline uint32_t tcp_ws_apply(uint32_t field, uint8_t shift) {
    if (shift > TCP_WS_SHIFT_MAX) shift = TCP_WS_SHIFT_MAX;
    if (shift == 0u) return field;
    if (field > (0xFFFFFFFFu >> shift)) return 0xFFFFFFFFu;
    return field << shift;
}

/* Real window -> the 16-bit field to put on the wire, for OUR advertised
 * window: `wnd >> shift`, clamped to 65535. */
static inline uint16_t tcp_ws_encode(uint32_t window, uint8_t shift) {
    if (shift > TCP_WS_SHIFT_MAX) shift = TCP_WS_SHIFT_MAX;
    uint32_t v = window >> shift;
    if (v > 0xFFFFu) v = 0xFFFFu;
    return (uint16_t)v;
}

/*
 * The shift we may ask for, given a receive buffer of `buf_bytes`.
 *
 * This is not a tuning knob: it is the promise we make to the peer about how
 * much unread data we can hold.  A buffer smaller than one window needs no
 * shift (0), and every doubling of the buffer above 65535 buys one bit —
 * flooring, never rounding up, because rounding up would advertise space
 * that does not exist.
 */
static inline uint8_t tcp_ws_shift_for_buffer(uint32_t buf_bytes) {
    uint8_t s = 0;
    /* Largest shift whose maximum encodable window (65535 << s) still fits
     * in the buffer: every value we could then advertise is one we can hold.
     * Floor, never round up.
     *
     * 32-bit arithmetic on purpose: the largest candidate shift is 15, and
     * 0xFFFF << 15 still fits a uint32_t, so no 64-bit widening is needed —
     * and this tree ratchets widening casts in portable code, correctly. */
    while (s < TCP_WS_SHIFT_MAX &&
           (0xFFFFu << (uint32_t)(s + 1u)) <= buf_bytes)
        s++;
    return s;
}

/*
 * Should we scale this connection at all?
 *
 * RFC 7323 s2.2: scaling is negotiated in the handshake and must not be
 * applied unless the peer also offered it.  `we_offered` is what we put in
 * our SYN/SYN-ACK; the peer's answer decides the shift we use for ITS window
 * and for OURS.  Returns 1 when scaling is in force.
 */
static inline int tcp_ws_engaged(int we_offered, const tcp_ws_opt_t *peer) {
    return we_offered && peer && peer->offered;
}

#endif /* AURALITE_NET_TCP_WSCALE_H */
