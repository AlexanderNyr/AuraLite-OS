#ifndef AURALITE_DRIVERS_R8169_R8169_CORE_H
#define AURALITE_DRIVERS_R8169_R8169_CORE_H

/*
 * r8169_core.h -- the RTL8169 driver state machine, pure C, no hardware.
 * (REALTEK_PLAN.md phase RT2.)
 *
 * This is the 8169's version of the D2 pattern (rtl8139_ring.h for the
 * 8139, tcp_x5.h/tcp_cc.h for TCP): the logic that is easy to get wrong
 * lives in a header with no hardware in it, so a HOST test can drive the
 * whole data path against a register-level model of the chip -- because
 * QEMU has no 8169 and no real silicon can be asked to produce a
 * wrapped ring or an OWN-bit protocol violation on demand.
 *
 * All device access goes through `struct r8169_io` -- read/write
 * callbacks of a fixed width, not raw MMIO.  The width discipline is
 * structural: a 16-bit register can only be touched with a 16-bit
 * accessor, which is the same rule r8169_reg_width() pins and the model
 * enforces with a fault.  The two implementations of the seam are
 *   - the kernel driver (drivers/r8169/r8169.c), which binds inb/outb/
 *     inw/outw/inl/outl to the BAR0 I/O window;
 *   - the host model (tests/unit/r8169_model.h), which binds the
 *     register file + the descriptor-ring DMA behaviour.
 *
 * The includer must provide memcpy() and memset() before this header
 * (kernel/lib/string.h in the kernel, <string.h> on the host).
 *
 * The bug classes this core exists to get right (pinned in
 * r8169_desc.h, driven here end to end):
 *   1. the 14-bit RX length field (the armed buffer must be 16383);
 *   2. the FCS IS in the length (strip 4 bytes before handing up);
 *   3. the OWN-bit hand-off (rearm sets OWN, preserves EOR);
 *   4. the ring wraps modulo the COUNT while EOR marks the physical end.
 */

#include <stdint.h>

#include "drivers/r8169/r8169_desc.h"
#include "drivers/r8169/r8169.h"

/* ------------------------------------------------------------------ *
 * The narrow register-I/O seam.  Widths are typed, so an access of the
 * wrong width is impossible to express (the 8139's lesson made
 * structural: ISR/IMR are 16-bit, CR/TPPOLL are 8-bit).
 * ------------------------------------------------------------------ */

struct r8169_io {
    uint8_t  (*rd8)(struct r8169_io *io, uint16_t reg);
    uint16_t (*rd16)(struct r8169_io *io, uint16_t reg);
    uint32_t (*rd32)(struct r8169_io *io, uint16_t reg);
    void (*wr8)(struct r8169_io *io, uint16_t reg, uint8_t  v);
    void (*wr16)(struct r8169_io *io, uint16_t reg, uint16_t v);
    void (*wr32)(struct r8169_io *io, uint16_t reg, uint32_t v);
    /* Back-off between poll iterations (arch_cpu_relax() in the kernel,
     * a no-op on the host, where the model completes synchronously). */
    void (*relax)(struct r8169_io *io);
};

/* ------------------------------------------------------------------ *
 * Driver state.  The ring pointers are the CPU view of DMA memory; the
 * *_phys fields are the bus addresses programmed into the chip.  On the
 * host gate the two are the same (identity); in the kernel they are
 * HHDM pointer vs physical address.
 * ------------------------------------------------------------------ */

struct r8169_state {
    struct r8169_tx_desc *tx_ring;
    struct r8169_rx_desc *rx_ring;
    uint64_t tx_ring_phys;
    uint64_t rx_ring_phys;

    uint8_t  *tx_buf;                      /* one 2048-byte bounce buffer */
    uint64_t  tx_buf_phys;
    uint8_t  *rx_pkt[R8169_NUM_RX_DESC];   /* one armed buffer per slot   */
    uint64_t  rx_pkt_phys[R8169_NUM_RX_DESC];

    uint8_t   mac[6];
    uint32_t  tx_cursor;
    uint32_t  rx_cursor;
    int       present;

    uint32_t  tx_packets;
    uint32_t  rx_packets;
};

/* ------------------------------------------------------------------ *
 * The state machine.
 * ------------------------------------------------------------------ */

/* Software reset: set CR.RST and wait for the chip to clear it. */
static inline int r8169_core_reset(struct r8169_io *io, struct r8169_state *st) {
    uint32_t to = 100000;
    (void)st;
    io->wr8(io, R8169_CR, R8169_CR_RESET);
    while (to-- > 0) {
        if (!(io->rd8(io, R8169_CR) & R8169_CR_RESET)) return 0;
        io->relax(io);
    }
    return -1;
}

/* Read the station address the chip loaded at reset (IDR0-5). */
static inline void r8169_core_load_mac(struct r8169_io *io,
                                       struct r8169_state *st) {
    uint32_t i;
    for (i = 0; i < 6; i++)
        st->mac[i] = io->rd8(io, (uint16_t)(R8169_MAC0 + i));
}

/* Program everything the legacy data path needs and arm the rings.
 * Returns 0 on success, -1 when a ring base is not 256-byte aligned
 * (refused by name -- the 8169's analog of the 8139's 4 GiB refusal;
 * see r8169_desc.h). */
static inline int r8169_core_config(struct r8169_io *io,
                                    struct r8169_state *st) {
    uint32_t i;

    /* Ring bases: 64-bit, 256-byte aligned, BOTH halves programmed.
     * Refuse BEFORE touching the chip, by name (the 8169's analog of
     * the 8139's 4 GiB refusal; see r8169_desc.h). */
    if (!r8169_desc_base_is_aligned(st->tx_ring_phys) ||
        !r8169_desc_base_is_aligned(st->rx_ring_phys))
        return -1;

    /* Multicast: accept everything (the e1000's R9 reasoning -- IPv6
     * NDP rides 33:33:xx group addresses). */
    for (i = 0; i < 8; i++)
        io->wr8(io, (uint16_t)(R8169_MAR0 + i), 0xFF);

    /* Mask interrupts while the rings are armed; clear any latched. */
    io->wr16(io, R8169_IMR, 0);
    io->wr16(io, R8169_ISR, 0xFFFF);

    io->wr32(io, R8169_TNPDS_LOW,  r8169_desc_base_lo(st->tx_ring_phys));
    io->wr32(io, R8169_TNPDS_HIGH, r8169_desc_base_hi(st->tx_ring_phys));
    io->wr32(io, R8169_RDSAR_LOW,  r8169_desc_base_lo(st->rx_ring_phys));
    io->wr32(io, R8169_RDSAR_HIGH, r8169_desc_base_hi(st->rx_ring_phys));

    /* TX: max DMA burst + shortest IFG.  RX: accept our own address,
     * broadcast, multicast; RX FIFO threshold + max burst (Linux's
     * RX_FIFO_THRESH | RX_DMA_BURST). */
    io->wr32(io, R8169_TCR, (7u << R8169_TCR_DMA_SHIFT) |
                            (3u << R8169_TCR_IFG_SHIFT));
    io->wr32(io, R8169_RCR, R8169_RCR_ACCEPT_MASK |
                            R8169_RCR_FIFO_THRESH |
                            R8169_RCR_DMA_BURST);

    /* Rx max size: one past the armed buffer, so the largest armed
     * frame is never refused at the size gate (Linux's +1). */
    io->wr16(io, R8169_RMS, (uint16_t)R8169_RMS_MAX);
    /* No early-TX threshold: transmit on FIFO-empty. */
    io->wr8(io, R8169_ETTHR, R8169_ETTHR_NO_EARLY);

    /* Arm the RX ring: each descriptor owns its 16383-byte buffer, EOR
     * exactly on the last descriptor (bug class #4). */
    for (i = 0; i < R8169_NUM_RX_DESC; i++) {
        st->rx_ring[i].addr  = st->rx_pkt_phys[i];
        st->rx_ring[i].opts1 = r8169_rx_rearm_opts1(
            (i == R8169_NUM_RX_DESC - 1) ? R8169_DESC_EOR : 0);
        st->rx_ring[i].opts2 = 0;
    }

    /* Clear the TX ring; each frame is written fresh (bug class #3's
     * hand-off happens per send, not per ring). */
    for (i = 0; i < R8169_NUM_TX_DESC; i++) {
        st->tx_ring[i].opts1 = 0;
        st->tx_ring[i].opts2 = 0;
        st->tx_ring[i].addr  = 0;
    }

    st->tx_cursor = 0;
    st->rx_cursor = 0;

    /* Enable receive + transmit. */
    io->wr8(io, R8169_CR, R8169_CR_RX_EN | R8169_CR_TX_EN);
    st->present = 1;
    return 0;
}

static inline int r8169_core_link_up(struct r8169_io *io,
                                     struct r8169_state *st) {
    if (!st->present) return 0;
    /* PHY status: the link bit is SET when up (inverse-free, unlike
     * the 8139's MSR.LINKB). */
    return (io->rd8(io, R8169_PHYSTATUS) & R8169_PHY_LINK) != 0;
}

/* Read and acknowledge the interrupt status (W1C). */
static inline uint16_t r8169_core_isr_ack(struct r8169_io *io) {
    uint16_t v = io->rd16(io, R8169_ISR);
    if (v) io->wr16(io, R8169_ISR, v);
    return v;
}

static inline void r8169_core_get_mac(const struct r8169_state *st,
                                      uint8_t mac[6]) {
    memcpy(mac, st->mac, 6);
}

/* Transmit one frame: runt-pad to 60 bytes, hand the descriptor over
 * (OWN set, EOR on the last slot), poll the TX queue, and wait for the
 * chip to hand the descriptor back (OWN clear).  Returns bytes sent, or
 * -1 on error/timeout. */
static inline int r8169_core_send(struct r8169_io *io, struct r8169_state *st,
                                  const void *data, uint32_t len) {
    uint32_t slot, xmit_len, to;
    uint16_t status;

    if (!st->present || !data) return -1;
    if (!r8169_core_link_up(io, st)) return -1;
    if (len == 0) return 0;
    if (len > R8169_PKT_BUF_SIZE) len = R8169_PKT_BUF_SIZE;

    /* The chip will not transmit a runt: pad to the 60-byte Ethernet
     * minimum (the chip appends the 4-byte FCS itself).  Without this
     * an ARP request -- 42 bytes -- is silently dropped by the wire. */
    xmit_len = len;
    if (xmit_len < R8169_MIN_FRAME) xmit_len = R8169_MIN_FRAME;

    slot = st->tx_cursor;
    if (st->tx_ring[slot].opts1 & R8169_DESC_OWN)
        return -1;   /* the previous frame on this slot is not done */

    memcpy(st->tx_buf, data, len);
    if (xmit_len > len) memset(st->tx_buf + len, 0, xmit_len - len);

    st->tx_ring[slot].addr  = st->tx_buf_phys;
    st->tx_ring[slot].opts2 = 0;
    st->tx_ring[slot].opts1 = r8169_tx_opts1(
        xmit_len, slot == R8169_NUM_TX_DESC - 1);

    /* Kick the normal-priority queue (the chip's transmit command). */
    io->wr8(io, R8169_TPPOLL, R8169_TPPOLL_NPQ);

    /* Wait for the chip to hand the descriptor back (OWN clear). */
    to = 100000;
    while ((st->tx_ring[slot].opts1 & R8169_DESC_OWN) && to-- > 0)
        io->relax(io);

    status = r8169_core_isr_ack(io);
    if (st->tx_ring[slot].opts1 & R8169_DESC_OWN)
        return -1;   /* timeout: the chip never took the frame */
    if (status & R8169_INT_TX_ERR)
        return -1;   /* FIFO underrun / transmit error */

    st->tx_cursor = r8169_rx_advance(st->tx_cursor, R8169_NUM_TX_DESC);
    st->tx_packets++;
    return (int)len;
}

/* Non-blocking receive: drain at most one completed frame out of the
 * RX ring, stripping the FCS (bug class #2), and rearm the slot (OWN
 * set, EOR preserved -- bug class #3).  Returns the payload length, 0
 * when nothing is complete, -1 on a ring desync. */
static inline int r8169_core_recv(struct r8169_state *st, void *buf,
                                  uint32_t bufsize) {
    uint32_t i;

    if (!st->present) return -1;
    if (!buf || bufsize == 0) return -1;

    for (i = 0; i < R8169_NUM_RX_DESC; i++) {
        uint32_t idx = st->rx_cursor;
        uint32_t opts1 = st->rx_ring[idx].opts1;
        enum r8169_rx_verdict v;
        uint32_t len = 0;

        if (r8169_rx_owned(opts1))
            return 0;   /* the chip still owns this slot: nothing more */

        v = r8169_rx_classify(opts1, &len);
        if (v == R8169_RX_OWNED)
            return 0;   /* defensive: the chip still owns it */
        if (v == R8169_RX_RESET) {
            /* The length is impossible: the cursor is no longer on a
             * real descriptor.  Rearm and give up rather than walk
             * garbage forever. */
            st->rx_ring[idx].opts1 = r8169_rx_rearm_opts1(
                (idx == R8169_NUM_RX_DESC - 1) ? R8169_DESC_EOR : 0);
            st->rx_cursor = r8169_rx_advance(idx, R8169_NUM_RX_DESC);
            return -1;
        }
        if (v == R8169_RX_DROP) {
            /* Hardware flagged an error (CRC/runt/FIFO-overflow) or a
             * fragment: rearm and move on. */
            st->rx_ring[idx].opts1 = r8169_rx_rearm_opts1(
                (idx == R8169_NUM_RX_DESC - 1) ? R8169_DESC_EOR : 0);
            st->rx_cursor = r8169_rx_advance(idx, R8169_NUM_RX_DESC);
            continue;
        }

        /* OK: len is the payload WITHOUT the FCS (bug class #2). */
        if (len > bufsize) len = bufsize;
        memcpy(buf, st->rx_pkt[idx], len);
        st->rx_ring[idx].opts1 = r8169_rx_rearm_opts1(
            (idx == R8169_NUM_RX_DESC - 1) ? R8169_DESC_EOR : 0);
        st->rx_cursor = r8169_rx_advance(idx, R8169_NUM_RX_DESC);
        st->rx_packets++;
        return (int)len;
    }
    return 0;
}

#endif /* AURALITE_DRIVERS_R8169_R8169_CORE_H */
