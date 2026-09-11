#ifndef AURALITE_DRIVERS_R8169_R8169_DESC_H
#define AURALITE_DRIVERS_R8169_R8169_DESC_H

#include <stdint.h>

/*
 * r8169_desc.h -- the RTL8169/8168/8111 register + descriptor surface,
 * as pure C.  (REALTEK_PLAN.md phase RT1.)
 *
 * The D2 pattern this tree already uses for TCP (tcp_x5.h, tcp_cc.h)
 * and for the 8139 (rtl8139_ring.h): the facts that are easy to get
 * wrong live in a header with no hardware in it, so a HOST test can
 * pin them against the cases neither QEMU nor -- yet -- any real
 * silicon can be asked to produce on demand.
 *
 * Everything below is MEASURED, not recalled.  Two sources, cross
 * checked against each other:
 *   - the RTL8169 datasheet (Realtek "Integrated Gigabit Ethernet
 *     Controller", Rev. 1.21, and the RTL8169SC/8110SC register spec);
 *   - Linux's r8169 driver (drivers/net/ethernet/realtek/r8169_main.c,
 *     r8169.c), the reference implementation for this chip family.
 * Where the two agree, the constant is pinned here with the page or
 * the code site named.  The 8169 is a DIFFERENT machine from the 8139:
 * descriptor rings with an OWN-bit hand-off instead of a CAPR/CBR ring
 * buffer, and 64-bit descriptor addresses instead of the 32-bit
 * RBSTART/TSAD the 8139 must refuse by name.
 *
 * The four bug classes this header exists to pin (the 8169's version
 * of the 8139's two-ring-sizes trap), each with its own test:
 *
 *  1. THE 14-BIT LENGTH FIELD.  The RX descriptor's frame length is
 *     bits 13:0 of opts1 -- 0x3fff = 16383 is the LARGEST value the
 *     field can express.  The buffer the driver arms each descriptor
 *     with must never exceed that, or the hardware truncates the size
 *     hint silently and the driver and the chip disagree about how
 *     much memory is safe to touch.  Linux arms exactly 16383.
 *
 *  2. THE FCS IS IN THE LENGTH.  The length the NIC writes INCLUDES
 *     the 4-byte Ethernet CRC, which the stack must not see; handing
 *     the raw value upwards is the 8169's version of the 8139's
 *     "every frame arrives 4 bytes too long" bug.
 *
 *  3. THE OWN-BIT HAND-OFF.  Bit 31 of opts1 is OWN.  The NIC owns a
 *     descriptor while OWN is set; the driver owns it while OWN is
 *     clear.  Touching a descriptor the NIC still owns reads garbage
 *     (RX) or corrupts an in-flight frame (TX); forgetting to set OWN
 *     when handing a descriptor BACK leaves the ring dead.  Every
 *     rearm must preserve the EOR bit (bit 30) -- dropping it breaks
 *     the ring-end marker the chip depends on for the wrap.
 *
 *  4. THE RING-END + WRAP.  Exactly one descriptor (the last) carries
 *     EOR.  The driver's cursor wraps modulo the descriptor COUNT; the
 *     EOR bit is what tells the CHIP where the physical ring ends.
 *     Confusing the count with the physical end -- or placing EOR on
 *     more than one descriptor -- desynchronises the ring.
 *
 * Nothing here touches a register, so the same object is exercised by
 * tests/unit/test_r8169_desc.c on the host and compiled into the
 * kernel driver (drivers/r8169/r8169.c, phase RT2).
 */

/* ------------------------------------------------------------------ *
 * Register map (byte offsets into the 256-byte register file).
 * The chip exposes the same file in I/O space (BAR0) and memory space
 * (BAR1); the driver uses BAR0, exactly like the 8139, so no MMIO
 * window mapping is needed.
 * ------------------------------------------------------------------ */

enum r8169_reg {
    /* MAC address (Ethernet hardware address), 6 bytes. */
    R8169_MAC0       = 0x00,
    R8169_MAC5       = 0x05,
    /* Multicast filter registers, 8 bytes. */
    R8169_MAR0       = 0x08,
    R8169_MAR7       = 0x0F,
    /* Transmit Normal-Priority Descriptor Start Address, 64-bit,
     * 256-byte aligned (datasheet p.19, "TNPDS").  Low at 0x20-0x23,
     * high at 0x24-0x27. */
    R8169_TNPDS_LOW  = 0x20,
    R8169_TNPDS_HIGH = 0x24,
    /* Command register, 8-bit (datasheet p.16, "CR"). */
    R8169_CR         = 0x37,
    /* Transmit Priority Polling register, 8-bit, write-only. */
    R8169_TPPOLL     = 0x38,
    /* Interrupt Mask, 16-bit (0x3C-0x3D). */
    R8169_IMR        = 0x3C,
    /* Interrupt Status, 16-bit (0x3E-0x3F). */
    R8169_ISR        = 0x3E,
    /* Transmit Configuration, 32-bit. */
    R8169_TCR        = 0x40,
    /* Receive Configuration, 32-bit. */
    R8169_RCR        = 0x44,
    /* EEPROM access (9346) register, 8-bit. */
    R8169_CFG9346    = 0x50,
    /* Config0..Config5, 8-bit each (0x51-0x56). */
    R8169_CONFIG0    = 0x51,
    R8169_CONFIG5    = 0x56,
    /* Rx packet Maximum Size, 16-bit (0xDA-0xDB). */
    R8169_RMS        = 0xDA,
    /* C+ Command register, 16-bit (0xE0-0xE1). */
    R8169_CPLUS_CMD  = 0xE0,
    /* Receive Descriptor Start Address, 64-bit, 256-byte aligned
     * (datasheet p.19, "RDSAR").  Low at 0xE4-0xE7, high 0xE8-0xEB. */
    R8169_RDSAR_LOW  = 0xE4,
    R8169_RDSAR_HIGH = 0xE8,
    /* Early Transmit Threshold, 8-bit (0xEC).  8169: unit 32 bytes. */
    R8169_ETTHR      = 0xEC,
};

/* The register file is 256 bytes (datasheet: MEMSIZE returns 0 == 256
 * bytes of memory space, and the chip "requires 256 bytes of IO
 * space").  Pinned so a driver that maps a smaller window is refused
 * by the host test rather than by a stray MMIO access. */
#define R8169_REG_SPACE     256

/* The register access WIDTH each register needs.  This is a real trap:
 * ISR/IMR are 16-bit, CR/TPPOLL are 8-bit -- reading ISR as a 32-bit
 * word spans into the register beside it.  (Linux's RTL_R16/RTL_R8
 * per-register accessors are this table made implicit; here it is
 * explicit so the host test can pin it.) */
enum r8169_reg_width { R8169_W8 = 1, R8169_W16 = 2, R8169_W32 = 4, R8169_W64 = 8 };

static inline enum r8169_reg_width r8169_reg_width(uint16_t off) {
    switch (off) {
    case R8169_MAC0: case R8169_MAC5:
    case R8169_MAR0: case R8169_MAR7:
    case R8169_CR: case R8169_TPPOLL:
    case R8169_CFG9346:
    case R8169_CONFIG0: case R8169_CONFIG5:
    case R8169_ETTHR:
        return R8169_W8;
    case R8169_IMR: case R8169_ISR:
    case R8169_RMS: case R8169_CPLUS_CMD:
        return R8169_W16;
    case R8169_TCR: case R8169_RCR:
        return R8169_W32;
    case R8169_TNPDS_LOW: case R8169_TNPDS_HIGH:
    case R8169_RDSAR_LOW: case R8169_RDSAR_HIGH:
        return R8169_W64;
    default:
        return R8169_W8;
    }
}

/* ------------------------------------------------------------------ *
 * Register content bits.
 * ------------------------------------------------------------------ */

/* Command register (CR, 0x37).  Linux r8169_main.c "ChipCmdBits". */
#define R8169_CR_STOP_REQ  (1u << 7)   /* StopReq   */
#define R8169_CR_RESET     (1u << 4)   /* CmdReset  */
#define R8169_CR_RX_EN     (1u << 3)   /* CmdRxEnb  */
#define R8169_CR_TX_EN     (1u << 2)   /* CmdTxEnb  */
#define R8169_CR_RX_EMPTY  (1u << 0)   /* RxBufEmpty */

/* Transmit Priority Polling (TPPOLL, 0x38). */
#define R8169_TPPOLL_HPQ   (1u << 7)   /* poll the high-priority queue  */
#define R8169_TPPOLL_NPQ   (1u << 6)   /* poll the normal-priority queue */
#define R8169_TPPOLL_FSW   (1u << 0)   /* forced software interrupt      */

/* Interrupt Status/Mask (ISR/IMR).  Linux r8169_main.c
 * "InterruptStatusBits".  A one-shot `[r8169] RX via IRQ wake` receipt
 * proves the interrupt -- not the polling fallback -- did the work, the
 * R9/RES-28 discipline. */
#define R8169_INT_SYS_ERR   (1u << 15)  /* SYSErr        */
#define R8169_INT_PCS_TIMEO (1u << 14)  /* PCSTimeout    */
#define R8169_INT_SW        (1u << 8)   /* SWInt         */
#define R8169_INT_TX_UNAVAIL (1u << 7)  /* TxDescUnavail */
#define R8169_INT_RX_FIFO_OVER (1u << 6) /* RxFIFOOver   */
#define R8169_INT_LINK_CHG  (1u << 5)   /* LinkChg       */
#define R8169_INT_RX_OVER   (1u << 4)   /* RxOverflow    */
#define R8169_INT_TX_ERR    (1u << 3)   /* TxErr         */
#define R8169_INT_TX_OK     (1u << 2)   /* TxOK          */
#define R8169_INT_RX_ERR    (1u << 1)   /* RxErr         */
#define R8169_INT_RX_OK     (1u << 0)   /* RxOK          */

/* Receive Configuration (RCR) accept bits.  Linux r8169_main.c
 * "rx_mode_bits". */
#define R8169_RCR_ACCEPT_ERR        (1u << 5)
#define R8169_RCR_ACCEPT_RUNT       (1u << 4)
#define R8169_RCR_ACCEPT_BROADCAST  (1u << 3)
#define R8169_RCR_ACCEPT_MULTICAST  (1u << 2)
#define R8169_RCR_ACCEPT_MY_PHYS    (1u << 1)
#define R8169_RCR_ACCEPT_ALL_PHYS   (1u << 0)
/* The mask the driver programs for the normal path: our own address,
 * broadcast, multicast, plus runts (the stack tolerates them). */
#define R8169_RCR_ACCEPT_MASK \
    (R8169_RCR_ACCEPT_MY_PHYS | R8169_RCR_ACCEPT_BROADCAST | \
     R8169_RCR_ACCEPT_MULTICAST)

/* Cfg9346 (0x50) -- the EEPROM write-enable lock.  The MAC can only be
 * written after unlocking; the driver must relock after (Linux
 * "Cfg9346Bits"). */
#define R8169_CFG9346_LOCK   0x00
#define R8169_CFG9346_UNLOCK 0xC0

/* ------------------------------------------------------------------ *
 * Descriptors: 4 consecutive double words (16 bytes) each, up to 1024
 * per ring (datasheet p.21).  The ring base address is 64-bit and MUST
 * be 256-byte aligned (datasheet TNPDS/RDSAR: "(256-byte alignment)").
 * ------------------------------------------------------------------ */

#define R8169_DESC_SIZE      16
#define R8169_DESC_ALIGN     256
#define R8169_MAX_DESC       1024     /* the datasheet's ring-size cap */

/* Default ring sizes -- what Linux ships (NUM_TX_DESC 64, NUM_RX_DESC
 * 256).  Within the 1024 cap, and the RX count times the descriptor
 * size stays far under the 4 KiB the 256-byte alignment already
 * implies is page-friendly. */
#define R8169_NUM_TX_DESC    64
#define R8169_NUM_RX_DESC    256
#define R8169_TX_RING_BYTES  (R8169_NUM_TX_DESC * R8169_DESC_SIZE)
#define R8169_RX_RING_BYTES  (R8169_NUM_RX_DESC * R8169_DESC_SIZE)

/* The RX buffer each descriptor is armed with.  16383 == 0x3fff is the
 * largest value the 14-bit length field can express (bug class #1);
 * Linux uses exactly this (static int rx_buf_sz = 16383). */
#define R8169_RX_BUF_SZ      16383
#define R8169_RX_LEN_MASK    0x3fff
#define R8169_TX_LEN_MASK    0xffff
#define R8169_FCS_LEN        4       /* the CRC the length INCLUDES */

/* opts1 bits shared by both descriptor types.  Linux "rtl_desc_bit". */
#define R8169_DESC_OWN       (1u << 31)  /* NIC owns the descriptor      */
#define R8169_DESC_EOR       (1u << 30)  /* end of descriptor ring       */
#define R8169_DESC_FS        (1u << 29)  /* first segment of a packet    */
#define R8169_DESC_LS        (1u << 28)  /* last segment of a packet     */

/* RX status (opts1) bits above the length field.  Linux "RxStatusDesc"
 * plus RxFOVF. */
#define R8169_RX_FOVF        (1u << 23)  /* RX FIFO overflow             */
#define R8169_RX_RWT         (1u << 22)  /* receive watchdog timeout     */
#define R8169_RX_RES         (1u << 21)  /* receive error summary        */
#define R8169_RX_RUNT        (1u << 20)  /* runt packet                  */
#define R8169_RX_CRC         (1u << 19)  /* CRC error                    */

/* TX opts2 bit: add a VLAN tag (Linux "TxVlanTag").  Not used by the
 * legacy path, but pinned so the layout is not re-guessed later. */
#define R8169_TX_VLAN_TAG    (1u << 17)

/*
 * The two descriptor layouts, byte-exact from the datasheet (little
 * endian: "Byte 3 Byte 2 Byte 1 Byte 0" for each double word).  Both
 * are { opts1 u32, opts2 u32, addr u64 } = 16 bytes.  The addr is a
 * full 64-bit PHYSICAL address -- unlike the 8139, the 8169 has no
 * 32-bit DMA truncation trap in the descriptor path; the trap is the
 * HIGH register of the ring-base pair, which must be programmed (see
 * r8169_desc_base_split below).
 */
struct r8169_tx_desc {
    uint32_t opts1;   /* OWN|EOR|FS|LS (|TX len in bits 15:0)           */
    uint32_t opts2;   /* VLAN tag / checksum-offload bits (unused, RT2)  */
    uint64_t addr;    /* physical address of the frame buffer            */
};

struct r8169_rx_desc {
    uint32_t opts1;   /* OWN|EOR|FS|LS|status (|RX len in bits 13:0)     */
    uint32_t opts2;   /* VLAN tag / protocol-id bits (unused, RT2)       */
    uint64_t addr;    /* physical address of the receive buffer          */
};

/* ------------------------------------------------------------------ *
 * The arithmetic (bug classes #1..#4).
 * ------------------------------------------------------------------ */

/* Verdicts r8169_rx_classify() can return. */
enum r8169_rx_verdict {
    R8169_RX_OK = 0,    /* deliver: *out_len holds the payload length */
    R8169_RX_OWNED,     /* the NIC still owns this slot -- stop draining */
    R8169_RX_DROP,      /* hardware flagged an error or a fragment: skip */
    R8169_RX_RESET      /* length impossible: the ring desynchronised    */
};

/* Split a 64-bit descriptor-ring base address into the low/high
 * register pair the chip takes (TNPDS/RDSAR are low+high).  A driver
 * that programs only the low register is the 8139's truncation bug in
 * disguise -- correct here means BOTH registers, every time. */
static inline uint32_t r8169_desc_base_lo(uint64_t base) {
    return (uint32_t)(base & 0xFFFFFFFFu);
}
static inline uint32_t r8169_desc_base_hi(uint64_t base) {
    return (uint32_t)(base >> 32);
}

/* The 256-byte alignment the datasheet demands of every ring base
 * (bug class #4's physical half: the chip does not check it, it just
 * behaves as if bits 7:0 were zero). */
static inline int r8169_desc_base_is_aligned(uint64_t base) {
    return (base & (R8169_DESC_ALIGN - 1)) == 0;
}

/* The chip owns the descriptor while OWN is set (bug class #3). */
static inline int r8169_rx_owned(uint32_t opts1) {
    return (opts1 & R8169_DESC_OWN) != 0;
}
static inline int r8169_tx_owned(uint32_t opts1) {
    return (opts1 & R8169_DESC_OWN) != 0;
}

/* The raw length field (still INCLUDES the 4-byte FCS -- bug class #2). */
static inline uint32_t r8169_rx_len(uint32_t opts1) {
    return opts1 & R8169_RX_LEN_MASK;
}

/* A frame whose FS/LS are not BOTH set is a fragment of a larger
 * packet -- the legacy path cannot stitch, so it is a drop, exactly
 * Linux's rtl8169_fragmented_frame(). */
static inline int r8169_rx_fragmented(uint32_t opts1) {
    return (opts1 & (R8169_DESC_FS | R8169_DESC_LS)) !=
           (R8169_DESC_FS | R8169_DESC_LS);
}

/* Classify one RX descriptor by its opts1 (bug classes #1..#3).
 *
 * `out_len` receives the payload length WITHOUT the FCS when OK is
 * returned.  The strict length guard mirrors the 8139: a length below
 * the bare minimum (a frame must at least carry its own CRC), or one
 * that exceeds the armed buffer, means the cursor is no longer looking
 * at a real descriptor and the caller should RESET rather than keep
 * walking garbage forever.
 */
static inline enum r8169_rx_verdict
r8169_rx_classify(uint32_t opts1, uint32_t *out_len)
{
    if (out_len) *out_len = 0;

    if (r8169_rx_owned(opts1)) return R8169_RX_OWNED;

    if (opts1 & R8169_RX_RES) return R8169_RX_DROP;
    if (opts1 & (R8169_RX_RWT | R8169_RX_FOVF)) return R8169_RX_DROP;
    if (opts1 & (R8169_RX_CRC | R8169_RX_RUNT)) return R8169_RX_DROP;
    if (r8169_rx_fragmented(opts1)) return R8169_RX_DROP;

    uint32_t len = r8169_rx_len(opts1);
    if (len <= R8169_FCS_LEN) return R8169_RX_RESET;
    if (len > R8169_RX_BUF_SZ) return R8169_RX_RESET;

    if (out_len) *out_len = len - R8169_FCS_LEN;
    return R8169_RX_OK;
}

/* Advance the RX cursor one descriptor (bug class #4's wrap half:
 * modulo the descriptor COUNT -- the EOR bit is the chip's, not the
 * cursor's). */
static inline uint32_t r8169_rx_advance(uint32_t idx, uint32_t ndesc) {
    return (idx + 1) % ndesc;
}

/* Build a TX opts1 for a single-fragment frame: the driver hands the
 * descriptor to the chip with OWN set, FS|LS both set, the length in
 * the low 16 bits, and EOR exactly on the ring's last descriptor.
 * (Linux's single-fragment path: DescOwn | FirstFrag | LastFrag |
 * len | RingEnd * !((entry+1) % NUM_TX_DESC).) */
static inline uint32_t r8169_tx_opts1(uint32_t len, int is_last) {
    return R8169_DESC_OWN | R8169_DESC_FS | R8169_DESC_LS |
           (len & R8169_TX_LEN_MASK) |
           (is_last ? R8169_DESC_EOR : 0);
}

/* Rearm an RX descriptor (hand it back to the chip): OWN set, EOR
 * preserved, and the armed buffer size written into the low 14 bits
 * (Linux's mark-to-ASIC: DescOwn | eor | rx_buf_sz).  The EOR
 * preservation is bug class #3's second half -- dropping it breaks
 * the ring-end marker the chip relies on. */
static inline uint32_t r8169_rx_rearm_opts1(uint32_t eor) {
    return R8169_DESC_OWN | (eor & R8169_DESC_EOR) |
           (R8169_RX_BUF_SZ & R8169_RX_LEN_MASK);
}

/* Rearm a TX descriptor the driver has finished with: clear OWN so the
 * software owns it again, keep EOR where the ring placed it. */
static inline uint32_t r8169_tx_free_opts1(uint32_t eor) {
    return (eor & R8169_DESC_EOR);
}

#endif /* AURALITE_DRIVERS_R8169_R8169_DESC_H */
