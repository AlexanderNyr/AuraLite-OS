#ifndef AURALITE_DRIVERS_R8169_R8169_H
#define AURALITE_DRIVERS_R8169_R8169_H

#include <stdint.h>

/*
 * Realtek RTL8169/8168/8111 Gigabit Ethernet driver.
 *
 * The gigabit sibling of drivers/rtl8139/.  It is a DIFFERENT machine:
 * descriptor rings with an OWN-bit ownership hand-off instead of a ring
 * buffer walked through CAPR/CBR, and 64-bit descriptor/buffer addresses
 * instead of the 8139's 32-bit RBSTART/TSAD.  The register map, the
 * descriptor layouts and the hand-off arithmetic live in r8169_desc.h
 * (RT1, host-pinned); the state machine that drives them lives in
 * r8169_core.h and is proved against a register-level model of the chip
 * (tests/unit/r8169_model.h + test_r8169_driver.c), because QEMU has no
 * 8169 at all.
 *
 * Register access is PORT I/O (BAR0), not MMIO: the chip exposes the
 * same 256-byte register file at BAR0 (I/O space) and BAR1 (memory
 * space), and the I/O route keeps the driver inside the portable
 * include budget that tools/check_width_sweep.py ratchets -- exactly
 * the 8139's reasoning.
 *
 * Unlike the 8139 there is no 4 GiB DMA wall to refuse by name: the
 * descriptor ring bases and every descriptor's buffer address are full
 * 64-bit values.  The honest analog the driver enforces instead is the
 * 256-byte alignment of both ring bases and programming BOTH halves of
 * the TNPDS/RDSAR register pair (see r8169_desc.h).
 */

#define R8169_VENDOR_ID  0x10EC

/* Device IDs this driver accepts: the same MAC core behind the two
 * marketing numbers, and the two rows the virtual-driver catalog lists
 * under the r8169 key. */
#define R8169_DEVICE_8169   0x8169   /* RTL8169 Gigabit */
#define R8169_DEVICE_8168   0x8168   /* RTL8168/8111 Gigabit */

/* House geometry: every NIC driver in this tree (e1000, rtl8139,
 * vmxnet3) uses a 2048-byte packet buffer, and the Ethernet minimum
 * frame (60 bytes) is the runt pad for TX. */
#define R8169_PKT_BUF_SIZE  2048
#define R8169_MIN_FRAME     60

/* Probe PCI for a supported Realtek gigabit NIC and bring it up.
 * Returns 0 on success, -1 when no supported device is present or the
 * device cannot be programmed. */
int r8169_init(void);

/* Copy the 6-byte station address into mac[6]. */
void r8169_get_mac(uint8_t mac[6]);

/* Non-zero when the PHY reports link up. */
int r8169_link_up(void);

/* Transmit one Ethernet frame.  Returns bytes sent, or -1 on error. */
int r8169_send(const void *data, uint32_t len);

/* Non-blocking receive: returns frame length, or 0 when none queued. */
int r8169_recv(void *buf, uint32_t bufsize);

/* Timed receive: timeout_ticks == 0 waits indefinitely, otherwise returns
 * 0 on timeout.  Returns < 0 when the link drops. */
int r8169_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks);

/* Register this NIC with the netdev layer (after r8169_init succeeds). */
void r8169_register_netdev(void);

#endif /* AURALITE_DRIVERS_R8169_R8169_H */
