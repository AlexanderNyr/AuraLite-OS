#ifndef R8169_MODEL_H
#define R8169_MODEL_H

/*
 * r8169_model.h -- a register-level behaviour model of the RTL8169 MAC.
 *
 * QEMU has no 8169, so the chip itself becomes software under test:
 * the driver core (drivers/r8169/r8169_core.h) is run against this
 * model on the host (tests/unit/test_r8169_driver.c), and the model is
 * proven against the datasheet's documented invariants by its own
 * self-test -- a model that is wrong the same way the driver is wrong
 * would pass both, so the self-test is load-bearing, not decoration.
 *
 * What the model holds:
 *   - the register file (IDR0-5 / MAR0-7 / CR / TPPOLL / IMR / ISR /
 *     TCR / RCR / Cfg9346 / Config0-5 / RMS / C+CR / PHYstatus / ETThr /
 *     TNPDS / RDSAR), with per-register access WIDTH checked against
 *     r8169_reg_width() -- a write of the wrong width is a fault, not a
 *     silent truncation;
 *   - the two descriptor rings, RECONSTRUCTED from the TNPDS/RDSAR
 *     base registers the driver programmed (no side channel -- the model
 *     sees only what the chip would see), plus the DMA behaviours: TX
 *     consumes descriptors with OWN set and hands them back; RX writes
 *     a delivered frame into the armed buffer and hands the descriptor
 *     back.
 *
 * Deliberately NOT modelled (the RT3 metal-verification surface): the
 * PHY MII registers (0x60-0x63), the TBI (0x6A-0x6B), the EEPROM data
 * plane behind Cfg9346 (the lock itself is modelled), and the timing
 * registers' reset semantics.  This is the stated boundary in the plan:
 * the model proves the DRIVER; only the user's silicon proves the CHIP.
 *
 * The model never includes the driver; it only implements the seam the
 * driver talks through.  It may include <string.h> (host-only header).
 */

#include <stdint.h>
#include <string.h>

#include "drivers/r8169/r8169_desc.h"
#include "drivers/r8169/r8169_core.h"

struct r8169_model {
    /* --- register file --- */
    uint8_t  mac[6];        /* IDR0-5                             */
    uint8_t  mar[8];        /* MAR0-7                             */
    uint8_t  config[6];     /* Config0-5                          */
    uint8_t  cr;            /* 0x37 Command                        */
    uint8_t  cfg9346;       /* 0x50 EEPROM lock                    */
    uint8_t  phystatus;     /* 0x6C (link bit SET when up)         */
    uint8_t  etthr;         /* 0xEC EarlyTxThres                   */
    uint16_t imr;           /* 0x3C                                */
    uint16_t isr;           /* 0x3E                                */
    uint16_t rms;           /* 0xDA                                */
    uint16_t cplus;         /* 0xE0                                */
    uint32_t tcr;           /* 0x40                                */
    uint32_t rcr;           /* 0x44                                */
    uint32_t tnpds_lo;      /* 0x20 TX ring base low               */
    uint32_t tnpds_hi;      /* 0x24 TX ring base high              */
    uint32_t rdsar_lo;      /* 0xE4 RX ring base low               */
    uint32_t rdsar_hi;      /* 0xE8 RX ring base high              */

    /* --- flags the tests can read / plant --- */
    unsigned tnpds_hi_written  : 1;   /* the driver programmed the HIGH reg */
    unsigned rdsar_hi_written  : 1;
    unsigned tx_underrun       : 1;   /* planted FIFO underrun (TXERR)     */
    unsigned tx_stall          : 1;   /* planted: poll command does nothing */

    /* --- observability --- */
    uint32_t faults;                 /* every model fault increments this  */
    uint32_t tx_frames;              /* frames the DMA consumed            */
    uint8_t  tx_wire[R8169_PKT_BUF_SIZE];  /* bytes the last frame put out */
    uint32_t tx_wire_len;
};

/* A fault is observable: the self-test asserts that illegal accesses
 * move this counter, so a model with the discipline removed is caught
 * (the "live gate" proof). */
static inline void r8169_model_fault(struct r8169_model *m) { m->faults++; }

/* Forward: the TPPOLL write path (above) invokes the TX DMA. */
static inline uint32_t r8169_model_tx_drain(struct r8169_model *m);

static inline void r8169_model_init(struct r8169_model *m) {
    memset(m, 0, sizeof(*m));
    m->cfg9346 = R8169_CFG9346_LOCK;
    m->phystatus = R8169_PHY_LINK;      /* a cable is plugged in */
    m->isr = 0;
}

/* ------------------------------------------------------------------ *
 * Register access, width-checked.
 * ------------------------------------------------------------------ */

static inline uint8_t r8169_model_rd8(struct r8169_model *m, uint16_t reg) {
    if (r8169_reg_width(reg) != R8169_W8) { r8169_model_fault(m); return 0; }
    if (reg <= R8169_MAC5)                       /* MAC0 == 0 */
        return m->mac[reg];
    if (reg >= R8169_MAR0 && reg <= R8169_MAR7)
        return m->mar[reg - R8169_MAR0];
    if (reg >= R8169_CONFIG0 && reg <= R8169_CONFIG5)
        return m->config[reg - R8169_CONFIG0];
    switch (reg) {
    case R8169_CR:        return m->cr;
    case R8169_CFG9346:   return m->cfg9346;
    case R8169_PHYSTATUS: return m->phystatus;
    case R8169_ETTHR:     return m->etthr;
    default:
        r8169_model_fault(m);
        return 0;
    }
}

static inline void r8169_model_wr8(struct r8169_model *m, uint16_t reg, uint8_t v) {
    if (r8169_reg_width(reg) != R8169_W8) { r8169_model_fault(m); return; }
    if (reg <= R8169_MAC5) { m->mac[reg] = v; return; }   /* MAC0 == 0 */
    if (reg >= R8169_MAR0 && reg <= R8169_MAR7) { m->mar[reg - R8169_MAR0] = v; return; }
    if (reg >= R8169_CONFIG0 && reg <= R8169_CONFIG5) { m->config[reg - R8169_CONFIG0] = v; return; }
    switch (reg) {
    case R8169_CFG9346:
        m->cfg9346 = v;
        break;
    case R8169_CR:
        /* Performing a reset clears the interrupt/status surface; the
         * chip clears the RST bit itself (the driver polls it away). */
        if (v & R8169_CR_RESET) m->isr = 0;
        m->cr = v & (R8169_CR_STOP_REQ | R8169_CR_RX_EN | R8169_CR_TX_EN);
        if (m->cr & R8169_CR_STOP_REQ) m->cr |= R8169_CR_RX_EMPTY;
        break;
    case R8169_TPPOLL:
        /* The poll command tells the chip to transmit the queue. */
        if (!m->tx_stall && (v & (R8169_TPPOLL_NPQ | R8169_TPPOLL_HPQ)))
            r8169_model_tx_drain(m);
        break;
    case R8169_ETTHR:
        m->etthr = v;
        break;
    default:
        r8169_model_fault(m);
        break;
    }
}

static inline uint16_t r8169_model_rd16(struct r8169_model *m, uint16_t reg) {
    if (r8169_reg_width(reg) != R8169_W16) { r8169_model_fault(m); return 0; }
    switch (reg) {
    case R8169_IMR: return m->imr;
    case R8169_ISR: return m->isr;
    case R8169_RMS: return m->rms;
    case R8169_CPLUS_CMD: return m->cplus;
    default:
        r8169_model_fault(m);
        return 0;
    }
}

static inline void r8169_model_wr16(struct r8169_model *m, uint16_t reg, uint16_t v) {
    if (r8169_reg_width(reg) != R8169_W16) { r8169_model_fault(m); return; }
    switch (reg) {
    case R8169_IMR: m->imr = v; break;
    case R8169_ISR:
        /* ISR is write-1-to-clear. */
        m->isr &= (uint16_t)~v;
        break;
    case R8169_RMS: m->rms = v; break;
    case R8169_CPLUS_CMD: m->cplus = v; break;
    default:
        r8169_model_fault(m);
        break;
    }
}

/* The TNPDS/RDSAR pairs are 64-bit registers; the hardware programs
 * them as two 32-bit halves, so a 32-bit access at either half is the
 * legal way to touch a W64 register (an 8/16-bit access still faults). */
static inline uint32_t r8169_model_rd32(struct r8169_model *m, uint16_t reg) {
    enum r8169_reg_width w = r8169_reg_width(reg);
    if (w != R8169_W32 && w != R8169_W64) { r8169_model_fault(m); return 0; }
    switch (reg) {
    case R8169_TCR: return m->tcr;
    case R8169_RCR: return m->rcr;
    case R8169_TNPDS_LOW:  return m->tnpds_lo;
    case R8169_TNPDS_HIGH: return m->tnpds_hi;
    case R8169_RDSAR_LOW:  return m->rdsar_lo;
    case R8169_RDSAR_HIGH: return m->rdsar_hi;
    default:
        r8169_model_fault(m);
        return 0;
    }
}

static inline void r8169_model_wr32(struct r8169_model *m, uint16_t reg, uint32_t v) {
    enum r8169_reg_width w = r8169_reg_width(reg);
    if (w != R8169_W32 && w != R8169_W64) { r8169_model_fault(m); return; }
    switch (reg) {
    case R8169_TCR: m->tcr = v; break;
    case R8169_RCR: m->rcr = v; break;
    case R8169_TNPDS_LOW:  m->tnpds_lo = v; break;
    case R8169_TNPDS_HIGH: m->tnpds_hi = v; m->tnpds_hi_written = 1; break;
    case R8169_RDSAR_LOW:  m->rdsar_lo = v; break;
    case R8169_RDSAR_HIGH: m->rdsar_hi = v; m->rdsar_hi_written = 1; break;
    default:
        r8169_model_fault(m);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The DMA behaviours.
 * ------------------------------------------------------------------ */

/* The chip's view of the ring bases: reconstructed from the registers
 * the driver programmed (no side channel). */
static inline struct r8169_tx_desc *r8169_model_tx_ring(struct r8169_model *m) {
    return (struct r8169_tx_desc *)(uintptr_t)
           (((uint64_t)m->tnpds_hi << 32) | m->tnpds_lo);
}
static inline struct r8169_rx_desc *r8169_model_rx_ring(struct r8169_model *m) {
    return (struct r8169_rx_desc *)(uintptr_t)
           (((uint64_t)m->rdsar_hi << 32) | m->rdsar_lo);
}

/* Transmit: walk the ring in order; every descriptor with OWN set is a
 * frame the driver handed over.  The model "DMA"s the bytes out (into
 * tx_wire, where the test can assert them), clears OWN (handing the
 * descriptor back), and latches TxOK.  A planted underrun marks the
 * segment TXERR and stops.  Returns the number of completed frames. */
static inline uint32_t r8169_model_tx_drain(struct r8169_model *m) {
    uint32_t idx, frames = 0;
    struct r8169_tx_desc *ring = r8169_model_tx_ring(m);

    m->tx_wire_len = 0;
    if (!ring) { r8169_model_fault(m); return 0; }

    for (idx = 0; idx < R8169_NUM_TX_DESC; idx++) {
        struct r8169_tx_desc *d = &ring[idx];
        uint32_t len;

        if (!(d->opts1 & R8169_DESC_OWN))
            continue;                       /* hole / not yet armed */

        if (m->tx_underrun) {
            d->opts1 &= ~R8169_DESC_OWN;
            m->isr |= R8169_INT_TX_ERR;
            return frames;
        }

        len = d->opts1 & R8169_TX_LEN_MASK;
        if (len <= sizeof(m->tx_wire) - m->tx_wire_len) {
            memcpy(m->tx_wire + m->tx_wire_len,
                   (const void *)(uintptr_t)d->addr, len);
            m->tx_wire_len += len;
        } else {
            r8169_model_fault(m);
        }
        d->opts1 &= ~R8169_DESC_OWN;        /* hand the descriptor back */
        if (d->opts1 & R8169_DESC_LS)
            frames++;
    }

    m->isr |= R8169_INT_TX_OK;
    m->tx_frames += frames;
    return frames;
}

/* Receive: deliver one wire frame (payload + 4-byte FCS) into the armed
 * buffer of slot `idx`, write the descriptor status/length (the length
 * INCLUDES the FCS, bug class #2), hand the descriptor back (OWN clear)
 * and latch RxOK.  The chip only ever writes a descriptor it owns. */
static inline int r8169_model_rx_deliver(struct r8169_model *m, uint32_t idx,
                                         const uint8_t *frame, uint32_t frame_len) {
    struct r8169_rx_desc *ring = r8169_model_rx_ring(m);
    struct r8169_rx_desc *d;
    uint32_t slot = idx % R8169_NUM_RX_DESC;

    if (!ring) { r8169_model_fault(m); return 0; }
    d = &ring[slot];

    if (!(d->opts1 & R8169_DESC_OWN)) {
        /* The NIC never writes a descriptor it does not own. */
        r8169_model_fault(m);
        return 0;
    }

    if (frame_len > R8169_RX_BUF_SZ) {
        /* Overruns the armed buffer: the chip truncates at the buffer
         * and flags FIFO overflow. */
        d->opts1 = R8169_DESC_FS | R8169_DESC_LS | R8169_RX_FOVF |
                   (R8169_RX_BUF_SZ & R8169_RX_LEN_MASK);
        d->opts2 = 0;
        m->isr |= R8169_INT_RX_OK;
        return 1;
    }

    memcpy((void *)(uintptr_t)d->addr, frame, frame_len);
    d->opts1 = R8169_DESC_FS | R8169_DESC_LS |
               (frame_len & R8169_RX_LEN_MASK);
    d->opts2 = 0;
    m->isr |= R8169_INT_RX_OK;
    return 1;
}

/* ------------------------------------------------------------------ *
 * The seam binding: adapt the model's accessors to the driver's
 * `struct r8169_io`.  `struct r8169_model_io` embeds the seam first so
 * a cast recovers the container.
 * ------------------------------------------------------------------ */

struct r8169_model_io {
    struct r8169_io io;
    struct r8169_model *m;
};

static uint8_t  r8169_mio_rd8(struct r8169_io *io, uint16_t reg) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    return r8169_model_rd8(mio->m, reg);
}
static uint16_t r8169_mio_rd16(struct r8169_io *io, uint16_t reg) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    return r8169_model_rd16(mio->m, reg);
}
static uint32_t r8169_mio_rd32(struct r8169_io *io, uint16_t reg) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    return r8169_model_rd32(mio->m, reg);
}
static void r8169_mio_wr8(struct r8169_io *io, uint16_t reg, uint8_t v) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    r8169_model_wr8(mio->m, reg, v);
}
static void r8169_mio_wr16(struct r8169_io *io, uint16_t reg, uint16_t v) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    r8169_model_wr16(mio->m, reg, v);
}
static void r8169_mio_wr32(struct r8169_io *io, uint16_t reg, uint32_t v) {
    struct r8169_model_io *mio = (struct r8169_model_io *)io;
    r8169_model_wr32(mio->m, reg, v);
}
static void r8169_mio_relax(struct r8169_io *io) { (void)io; }

static inline void r8169_model_bind_io(struct r8169_model *m,
                                       struct r8169_model_io *mio) {
    mio->m = m;
    mio->io.rd8   = r8169_mio_rd8;
    mio->io.rd16  = r8169_mio_rd16;
    mio->io.rd32  = r8169_mio_rd32;
    mio->io.wr8   = r8169_mio_wr8;
    mio->io.wr16  = r8169_mio_wr16;
    mio->io.wr32  = r8169_mio_wr32;
    mio->io.relax = r8169_mio_relax;
}

#endif /* R8169_MODEL_H */
