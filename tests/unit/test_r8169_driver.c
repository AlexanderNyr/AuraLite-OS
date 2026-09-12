/*
 * test_r8169_driver.c -- the RT2 host gate: the 8169 driver core, run
 * end to end against a register-level model of the chip.
 *
 * QEMU has no 8169, so the CHIP becomes software under test.  The
 * driver core (drivers/r8169/r8169_core.h) is linked here against the
 * model (r8169_model.h) and driven through the whole data path:
 * reset -> MAC load -> ring configuration -> TX (runt pad + OWN
 * hand-off) -> RX (FCS strip + EOR-preserving rearm) -> IRQ/status.
 * The model is separately proven by its own self-test against the
 * datasheet's invariants, because a model wrong the same way as the
 * driver passes both.
 *
 * Both objects are the REAL ones the kernel driver includes -- this
 * file re-declares nothing -- so the tested core and the shipped
 * driver cannot drift (the D2 pattern rtl8139_ring.h established).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/r8169/r8169_desc.h"
#include "drivers/r8169/r8169_core.h"
#include "r8169_model.h"

/* ------------------------------------------------------------------ *
 * Harness.
 * ------------------------------------------------------------------ */

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

static void eq_u64(uint64_t got, uint64_t want, const char *what) {
    checks++;
    if (got != want) {
        printf("  FAIL: %s (got %llu, want %llu)\n", what,
               (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void eq_mem(const uint8_t *got, const uint8_t *want, uint32_t n,
                   const char *what) {
    checks++;
    if (memcmp(got, want, n) != 0) {
        printf("  FAIL: %s (bytes differ over %u bytes)\n", what, n);
        failures++;
    }
}

/* ------------------------------------------------------------------ *
 * Fixtures.  The rings must be 256-byte aligned (the config refusal
 * otherwise fires by design), so _Alignas(256).  The RX buffers are
 * 16383 bytes each -- the armed size the 14-bit length field expresses.
 * ------------------------------------------------------------------ */

static _Alignas(256) struct r8169_tx_desc tx_ring[R8169_NUM_TX_DESC];
static _Alignas(256) struct r8169_rx_desc rx_ring[R8169_NUM_RX_DESC];
static uint8_t tx_buf[R8169_PKT_BUF_SIZE];
static uint8_t rx_pkt[R8169_NUM_RX_DESC][R8169_RX_BUF_SZ];

static struct r8169_model model;
static struct r8169_model_io mio;
static struct r8169_state st;

/* (Re)bind a fresh model + driver state and give the driver its rings
 * and buffers (identity-mapped: host address == bus address). */
static void bind(void) {
    uint32_t i;
    r8169_model_init(&model);
    r8169_model_bind_io(&model, &mio);
    memset(&st, 0, sizeof(st));
    memset(tx_ring, 0, sizeof(tx_ring));
    memset(rx_ring, 0, sizeof(rx_ring));
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(rx_pkt, 0, sizeof(rx_pkt));
    st.tx_ring      = tx_ring;
    st.rx_ring      = rx_ring;
    st.tx_ring_phys = (uint64_t)(uintptr_t)tx_ring;
    st.rx_ring_phys = (uint64_t)(uintptr_t)rx_ring;
    st.tx_buf       = tx_buf;
    st.tx_buf_phys  = (uint64_t)(uintptr_t)tx_buf;
    for (i = 0; i < R8169_NUM_RX_DESC; i++) {
        st.rx_pkt[i]      = rx_pkt[i];
        st.rx_pkt_phys[i] = (uint64_t)(uintptr_t)rx_pkt[i];
    }
}

/* Bring the chip up the way the kernel init() does: reset, read the
 * MAC, program + arm everything. */
static int bring_up(const uint8_t mac[6]) {
    uint32_t i;
    for (i = 0; i < 6; i++)
        r8169_model_wr8(&model, (uint16_t)(R8169_MAC0 + i), mac[i]);
    if (r8169_core_reset(&mio.io, &st) != 0) return -1;
    r8169_core_load_mac(&mio.io, &st);
    return r8169_core_config(&mio.io, &st);
}

/* Build a wire frame: `payload_len` payload bytes (seeded) followed by
 * a 4-byte FCS.  Returns the total wire length. */
static uint32_t mk_frame(uint8_t *out, uint32_t payload_len, uint8_t seed) {
    uint32_t i;
    for (i = 0; i < payload_len; i++) out[i] = (uint8_t)(seed + i);
    out[payload_len + 0] = 0xDE;
    out[payload_len + 1] = 0xAD;
    out[payload_len + 2] = 0xBE;
    out[payload_len + 3] = 0xEF;
    return payload_len + 4;
}

/* ------------------------------------------------------------------ *
 * The model's own self-test -- load-bearing, not decoration: every
 * check here is a datasheet invariant, and the fault-counter checks
 * prove the discipline is LIVE (a model with the width check removed
 * fails them).
 * ------------------------------------------------------------------ */

static void model_self_test(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[1600];
    uint32_t i;

    printf("  [model self-test]\n");

    /* Reset semantics: the chip clears CR.RST itself, clears ISR. */
    r8169_model_init(&model);
    model.isr = 0xABCD;
    r8169_model_wr8(&model, R8169_CR, R8169_CR_RESET);
    eq_u32(model.cr & R8169_CR_RESET, 0, "reset: the chip clears CR.RST");
    eq_u32(model.isr, 0, "reset: ISR is cleared");

    /* CR writable mask: STOP_REQ|RX_EN|TX_EN latched; RX_EMPTY is the
     * read-only status the chip sets while stopped. */
    r8169_model_wr8(&model, R8169_CR, 0xFF);
    eq_u32(model.cr, R8169_CR_STOP_REQ | R8169_CR_RX_EN | R8169_CR_TX_EN |
                      R8169_CR_RX_EMPTY, "CR writable mask + RX_EMPTY status");

    /* ISR is write-1-to-clear. */
    model.isr = 0xFFFF;
    r8169_model_wr16(&model, R8169_ISR, R8169_INT_TX_OK);
    eq_u32(r8169_model_rd16(&model, R8169_ISR), 0xFFFF & ~R8169_INT_TX_OK,
           "ISR is W1C: writing TxOK clears only TxOK");

    /* Width discipline is LIVE: touching a register at the wrong width
     * is a fault, not a silent truncation. */
    model.faults = 0;
    r8169_model_rd32(&model, R8169_CR);           /* 8-bit reg via 32-bit */
    eq_u32(model.faults, 1, "width: 32-bit read of an 8-bit reg faults");
    r8169_model_rd16(&model, R8169_CR);
    eq_u32(model.faults, 2, "width: 16-bit read of an 8-bit reg faults");
    r8169_model_wr8(&model, R8169_IMR, 0x12);     /* 16-bit reg via 8-bit */
    eq_u32(model.faults, 3, "width: 8-bit write of a 16-bit reg faults");
    r8169_model_rd8(&model, R8169_TCR);           /* 32-bit reg via 8-bit */
    eq_u32(model.faults, 4, "width: 8-bit read of a 32-bit reg faults");
    r8169_model_rd8(&model, R8169_TNPDS_LOW);     /* 64-bit reg via 8-bit */
    eq_u32(model.faults, 5, "width: 8-bit read of a 64-bit reg faults");
    r8169_model_rd32(&model, R8169_TNPDS_LOW);    /* legal: 32-bit half */
    eq_u32(model.faults, 5, "width: 32-bit half of a 64-bit reg is legal");

    /* MAC / MAR round-trip through IDR0-5 and MAR0-7. */
    for (i = 0; i < 6; i++)
        r8169_model_wr8(&model, (uint16_t)(R8169_MAC0 + i), mac[i]);
    for (i = 0; i < 6; i++)
        eq_u32(r8169_model_rd8(&model, (uint16_t)(R8169_MAC0 + i)), mac[i],
               "MAC byte round-trips through IDR");
    for (i = 0; i < 8; i++)
        r8169_model_wr8(&model, (uint16_t)(R8169_MAR0 + i), 0xFF);
    eq_u32(r8169_model_rd8(&model, R8169_MAR7), 0xFF, "MAR round-trips");

    /* Cfg9346 lock/unlock round-trip. */
    r8169_model_wr8(&model, R8169_CFG9346, R8169_CFG9346_UNLOCK);
    eq_u32(r8169_model_rd8(&model, R8169_CFG9346), R8169_CFG9346_UNLOCK,
           "Cfg9346 unlocks");
    r8169_model_wr8(&model, R8169_CFG9346, R8169_CFG9346_LOCK);
    eq_u32(r8169_model_rd8(&model, R8169_CFG9346), R8169_CFG9346_LOCK,
           "Cfg9346 relocks");

    /* TX DMA: the model consumes descriptors with OWN set, hands them
     * back (OWN clear), latches TxOK, and copies the bytes out. */
    bind();
    r8169_model_wr32(&model, R8169_TNPDS_LOW,
                     r8169_desc_base_lo((uint64_t)(uintptr_t)tx_ring));
    r8169_model_wr32(&model, R8169_TNPDS_HIGH,
                     r8169_desc_base_hi((uint64_t)(uintptr_t)tx_ring));
    {
        uint32_t n = mk_frame(frame, 100, 0x40);
        memcpy(tx_buf, frame, n);
        tx_ring[0].addr  = (uint64_t)(uintptr_t)tx_buf;
        tx_ring[0].opts2 = 0;
        tx_ring[0].opts1 = R8169_DESC_OWN | R8169_DESC_FS | R8169_DESC_LS | n;
        model.isr = 0;
        eq_u32(r8169_model_tx_drain(&model), 1, "TX drain completes 1 frame");
        eq_u32(tx_ring[0].opts1 & R8169_DESC_OWN, 0, "TX: OWN handed back");
        ok(model.isr & R8169_INT_TX_OK, "TX: TxOK latched");
        eq_u32(model.tx_wire_len, n, "TX: byte count on the wire");
        eq_mem(model.tx_wire, frame, n, "TX: the bytes match the frame");
    }

    /* A planted underrun marks TXERR and completes nothing. */
    bind();
    r8169_model_wr32(&model, R8169_TNPDS_LOW,
                     r8169_desc_base_lo((uint64_t)(uintptr_t)tx_ring));
    r8169_model_wr32(&model, R8169_TNPDS_HIGH,
                     r8169_desc_base_hi((uint64_t)(uintptr_t)tx_ring));
    tx_ring[0].addr  = (uint64_t)(uintptr_t)tx_buf;
    tx_ring[0].opts1 = R8169_DESC_OWN | R8169_DESC_FS | R8169_DESC_LS | 60;
    model.tx_underrun = 1;
    model.isr = 0;
    eq_u32(r8169_model_tx_drain(&model), 0, "underrun completes no frame");
    ok(model.isr & R8169_INT_TX_ERR, "underrun latches TxErr");
    model.tx_underrun = 0;

    /* RX delivery: the chip only writes a descriptor it owns; it writes
     * the FULL wire length (FCS included) and hands the descriptor
     * back; a delivery to a driver-owned slot is a fault. */
    bind();
    r8169_model_wr32(&model, R8169_RDSAR_LOW,
                     r8169_desc_base_lo((uint64_t)(uintptr_t)rx_ring));
    r8169_model_wr32(&model, R8169_RDSAR_HIGH,
                     r8169_desc_base_hi((uint64_t)(uintptr_t)rx_ring));
    rx_ring[0].addr  = (uint64_t)(uintptr_t)rx_pkt[0];
    rx_ring[0].opts1 = R8169_DESC_OWN | (R8169_RX_BUF_SZ & R8169_RX_LEN_MASK);
    rx_ring[0].opts2 = 0;
    {
        uint32_t n = mk_frame(frame, 60, 0x80);
        model.isr = 0;
        eq_u32(r8169_model_rx_deliver(&model, 0, frame, n), 1,
               "RX delivery completes");
        eq_u32(rx_ring[0].opts1 & R8169_DESC_OWN, 0, "RX: OWN handed back");
        eq_u32(rx_ring[0].opts1 & R8169_RX_LEN_MASK, n,
               "RX: descriptor length is the FULL wire length (FCS in)");
        ok(model.isr & R8169_INT_RX_OK, "RX: RxOK latched");
        eq_mem(rx_pkt[0], frame, n, "RX: frame bytes land in the armed buffer");
        /* delivering again to a descriptor the chip no longer owns: fault */
        model.faults = 0;
        r8169_model_rx_deliver(&model, 0, frame, n);
        eq_u32(model.faults, 1, "RX: writing a driver-owned slot faults");
    }

    /* Link status: the PHY status bit is SET when up (inverse-free). */
    model.phystatus = R8169_PHY_LINK | R8169_PHY_FULLDUP;
    eq_u32(r8169_model_rd8(&model, R8169_PHYSTATUS) & R8169_PHY_LINK,
           R8169_PHY_LINK, "PHY status link bit is set-when-up");
}

/* ------------------------------------------------------------------ *
 * The driver core, run against the model.
 * ------------------------------------------------------------------ */

static void test_probe_mac_and_config(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t out[6];
    uint32_t i;

    printf("  [probe -> MAC -> config]\n");
    bind();
    eq_u32(bring_up(mac), 0, "probe + config succeed");

    r8169_core_get_mac(&st, out);
    eq_mem(out, mac, 6, "MAC read back matches the IDR bytes");
    eq_u32(st.present, 1, "driver is marked present");

    /* Both halves of both 64-bit ring bases were programmed. */
    ok(model.tnpds_hi_written, "TNPDS HIGH register was programmed");
    ok(model.rdsar_hi_written, "RDSAR HIGH register was programmed");
    /* The model reconstructs the rings from those registers alone. */
    ok(r8169_model_tx_ring(&model) == tx_ring,
       "model's TX ring == the ring the driver programmed");
    ok(r8169_model_rx_ring(&model) == rx_ring,
       "model's RX ring == the ring the driver programmed");

    /* Config registers carry the Linux-pinned values. */
    eq_u32(model.tcr, (7u << R8169_TCR_DMA_SHIFT) | (3u << R8169_TCR_IFG_SHIFT),
           "TCR: max DMA burst + shortest IFG");
    eq_u32(model.rcr, R8169_RCR_ACCEPT_MASK | R8169_RCR_FIFO_THRESH |
                      R8169_RCR_DMA_BURST, "RCR: accept mask + FIFO + burst");
    eq_u32(model.rms, R8169_RMS_MAX, "RMS: one past the armed buffer");
    eq_u32(model.etthr, R8169_ETTHR_NO_EARLY, "ETThr: no early transmit");

    /* MAR0-7 accept-all multicast. */
    for (i = 0; i < 8; i++)
        eq_u32(r8169_model_rd8(&model, (uint16_t)(R8169_MAR0 + i)), 0xFF,
               "MAR accepts all multicast");

    /* The RX ring is armed: OWN set everywhere, EOR exactly on the
     * last descriptor, every descriptor pointing at its buffer. */
    for (i = 0; i < R8169_NUM_RX_DESC; i++) {
        ok(rx_ring[i].opts1 & R8169_DESC_OWN, "RX descriptor armed (OWN)");
        eq_u32(rx_ring[i].opts1 & R8169_DESC_EOR,
               (i == R8169_NUM_RX_DESC - 1) ? R8169_DESC_EOR : 0,
               "EOR exactly on the last RX descriptor");
        eq_u64(rx_ring[i].addr, (uint64_t)(uintptr_t)rx_pkt[i],
               "RX descriptor points at its armed buffer");
    }

    /* The TX ring is clear; the chip is enabled. */
    eq_u32(tx_ring[0].opts1, 0, "TX ring starts clear");
    eq_u32(model.cr, R8169_CR_RX_EN | R8169_CR_TX_EN, "RX + TX enabled");
}

static void test_config_refuses_unaligned_base(void) {
    printf("  [256-byte alignment refusal]\n");
    bind();
    st.tx_ring_phys = 0x80;   /* deliberately unaligned */
    eq_u32(r8169_core_config(&mio.io, &st), -1,
           "a non-256-byte-aligned TX base is refused by name");
    eq_u32(st.present, 0, "driver is not marked present after refusal");
}

static void test_link(void) {
    printf("  [link]\n");
    bind();
    eq_u32(bring_up((uint8_t[6]){0,1,2,3,4,5}), 0, "bring-up for the link test");
    model.phystatus = R8169_PHY_LINK;
    eq_u32(r8169_core_link_up(&mio.io, &st), 1, "link up when the bit is set");
    model.phystatus = 0;
    eq_u32(r8169_core_link_up(&mio.io, &st), 0, "link down when the bit is clear");
}

static void test_tx(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[R8169_PKT_BUF_SIZE];
    uint8_t zeros[60];
    uint32_t n;

    printf("  [TX]\n");
    bind();
    bring_up(mac);

    /* A normal 100-byte frame goes out byte-for-byte. */
    n = 100;
    for (uint32_t i = 0; i < n; i++) frame[i] = (uint8_t)(0xA0 + i);
    eq_u32(r8169_core_send(&mio.io, &st, frame, n), (int)n, "TX returns bytes sent");
    eq_u32(model.tx_frames, 1, "the chip consumed one frame");
    eq_u32(tx_ring[0].opts1 & R8169_DESC_OWN, 0, "descriptor handed back");
    eq_u32(model.tx_wire_len, n, "wire byte count");
    eq_mem(model.tx_wire, frame, n, "wire bytes match");

    /* Runt padding: a 42-byte ARP grows to the 60-byte minimum. */
    memset(zeros, 0, sizeof(zeros));
    n = 42;
    for (uint32_t i = 0; i < n; i++) frame[i] = (uint8_t)(0xB0 + i);
    eq_u32(r8169_core_send(&mio.io, &st, frame, n), 42, "TX returns the logical length");
    eq_u32(model.tx_wire_len, 60, "runt is padded to 60 bytes on the wire");
    eq_mem(model.tx_wire, frame, 42, "pad keeps the original bytes");
    eq_mem(model.tx_wire + 42, zeros, 18, "pad region is zeroed");

    /* The TX cursor advanced; EOR lands on the last slot when reached. */
    eq_u32(st.tx_cursor, 2, "TX cursor advanced twice");

    /* No transmit while the link is down. */
    model.phystatus = 0;
    eq_u32(r8169_core_send(&mio.io, &st, frame, 64), -1, "TX refuses when link is down");
    model.phystatus = R8169_PHY_LINK;
}

static void test_tx_underrun(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[100];
    printf("  [TX underrun]\n");
    bind();
    bring_up(mac);
    memset(frame, 0x5A, sizeof(frame));
    model.tx_underrun = 1;
    eq_u32(r8169_core_send(&mio.io, &st, frame, sizeof(frame)), -1,
           "TX reports the planted underrun");
    eq_u32(st.tx_packets, 0, "no packet counted on underrun");
    model.tx_underrun = 0;
}

static void test_tx_timeout(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[100];
    printf("  [TX timeout]\n");
    bind();
    bring_up(mac);
    memset(frame, 0x11, sizeof(frame));
    model.tx_stall = 1;   /* the poll command will do nothing */
    eq_u32(r8169_core_send(&mio.io, &st, frame, sizeof(frame)), -1,
           "a stalled chip times the send out");
    ok(tx_ring[0].opts1 & R8169_DESC_OWN, "the descriptor is still owned");
    model.tx_stall = 0;
}

static void test_rx(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[1600];
    uint8_t buf[R8169_PKT_BUF_SIZE];
    uint32_t wire, payload;

    printf("  [RX]\n");
    bind();
    bring_up(mac);

    /* Deliver a 60-byte-payload frame (64 on the wire): the driver must
     * return exactly the payload, FCS stripped. */
    payload = 60;
    wire = mk_frame(frame, payload, 0xC0);
    eq_u32(r8169_model_rx_deliver(&model, st.rx_cursor, frame, wire), 1,
           "model delivers to the armed slot");
    memset(buf, 0, sizeof(buf));
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), (int)payload,
           "RX returns the FCS-stripped payload length");
    eq_mem(buf, frame, payload, "RX bytes are the payload, not the FCS");
    eq_u32(rx_ring[0].opts1 & R8169_DESC_OWN, R8169_DESC_OWN,
           "the slot is re-armed (OWN back)");
    eq_u32(st.rx_packets, 1, "one packet counted");

    /* The next delivery lands on the advanced cursor. */
    payload = 100;
    wire = mk_frame(frame, payload, 0xD0);
    eq_u32(r8169_model_rx_deliver(&model, st.rx_cursor, frame, wire), 1,
           "model delivers to the next slot");
    memset(buf, 0, sizeof(buf));
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), (int)payload,
           "second frame received at the advanced cursor");
    eq_mem(buf, frame, payload, "second frame bytes match");

    /* Nothing pending: the first still-owned slot stops the drain. */
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), 0, "no frame when nothing delivered");
}

static void test_rx_error_drop(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[1600];
    uint8_t buf[R8169_PKT_BUF_SIZE];
    uint32_t wire, payload;

    printf("  [RX error drop]\n");
    bind();
    bring_up(mac);

    /* The chip flags a CRC error: the driver must drop it, rearm the
     * slot, and move on to the next delivered frame. */
    rx_ring[0].opts1 = R8169_DESC_FS | R8169_DESC_LS | R8169_RX_CRC | 100;
    payload = 80;
    wire = mk_frame(frame, payload, 0xE0);
    eq_u32(r8169_model_rx_deliver(&model, 1, frame, wire), 1,
           "a good frame is delivered to slot 1");
    memset(buf, 0, sizeof(buf));
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), (int)payload,
           "the errored slot is dropped and the good frame comes through");
    eq_mem(buf, frame, payload, "the good frame bytes match");
    eq_u32(st.rx_packets, 1, "only the good frame counted");
    eq_u32(st.rx_cursor, 2, "cursor advanced past both slots");
}

static void test_rx_desync_reset(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t buf[R8169_PKT_BUF_SIZE];

    printf("  [RX desync -> reset]\n");
    bind();
    bring_up(mac);

    /* An impossible length (0) means the cursor is no longer on a real
     * descriptor: the driver rearms and gives up, it does not walk
     * garbage. */
    rx_ring[0].opts1 = R8169_DESC_FS | R8169_DESC_LS | 0;
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), -1,
           "an impossible length returns -1 (desync)");
    ok(rx_ring[0].opts1 & R8169_DESC_OWN, "the desynced slot is re-armed");
    eq_u32(st.rx_cursor, 1, "cursor moved past the desynced slot");
}

static void test_full_ring_wrap(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[1600];
    uint8_t buf[R8169_PKT_BUF_SIZE];
    uint32_t i, wire, payload;

    printf("  [full-ring wrap]\n");
    bind();
    bring_up(mac);

    /* Drive all 256 slots in order, wrapping back to 0, and prove the
     * EOR bit survives every rearm of the last descriptor. */
    payload = 32;
    for (i = 0; i < R8169_NUM_RX_DESC; i++) {
        wire = mk_frame(frame, payload, (uint8_t)(i & 0xFF));
        eq_u32(r8169_model_rx_deliver(&model, st.rx_cursor, frame, wire), 1,
               "deliver to the current slot");
        memset(buf, 0, sizeof(buf));
        eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), (int)payload,
               "receive at the current slot");
        eq_mem(buf, frame, payload, "frame bytes survive the wrap");
    }
    eq_u32(st.rx_packets, R8169_NUM_RX_DESC, "every slot delivered exactly once");
    eq_u32(st.rx_cursor, 0, "cursor wrapped back to 0");
    ok(rx_ring[R8169_NUM_RX_DESC - 1].opts1 & R8169_DESC_EOR,
       "EOR is preserved on the last descriptor after 256 rearms");

    /* The wrap really is a wrap: slot 0 accepts again. */
    wire = mk_frame(frame, payload, 0x7F);
    eq_u32(r8169_model_rx_deliver(&model, st.rx_cursor, frame, wire), 1,
           "slot 0 is armed again after the wrap");
    memset(buf, 0, sizeof(buf));
    eq_u32(r8169_core_recv(&st, buf, sizeof(buf)), (int)payload,
           "frame received on slot 0 after the wrap");
}

static void test_irq_status(void) {
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t frame[1600];
    uint32_t wire;

    printf("  [IRQ/status]\n");
    bind();
    bring_up(mac);

    /* The chip latches RxOK when it delivers; the driver acks W1C. */
    wire = mk_frame(frame, 60, 0x30);
    eq_u32(r8169_model_rx_deliver(&model, st.rx_cursor, frame, wire), 1,
           "deliver latches RxOK");
    {
        uint16_t v = r8169_core_isr_ack(&mio.io);
        ok(v & R8169_INT_RX_OK, "the ack returns the latched RxOK");
        eq_u32(model.isr & R8169_INT_RX_OK, 0, "the ack cleared RxOK");
    }
}

int main(void) {
    printf("test_r8169_driver: RTL8169 driver core vs the chip model\n");
    model_self_test();
    test_probe_mac_and_config();
    test_config_refuses_unaligned_base();
    test_link();
    test_tx();
    test_tx_underrun();
    test_tx_timeout();
    test_rx();
    test_rx_error_drop();
    test_rx_desync_reset();
    test_full_ring_wrap();
    test_irq_status();

    printf("test_r8169_driver: %d passed, %d failed (%d total)\n",
           checks - failures, failures, checks);
    return failures ? 1 : 0;
}
