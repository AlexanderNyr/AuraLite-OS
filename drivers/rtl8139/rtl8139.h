#ifndef AURALITE_DRIVERS_RTL8139_RTL8139_H
#define AURALITE_DRIVERS_RTL8139_RTL8139_H

#include <stdint.h>

/*
 * Realtek RTL8139 family Fast Ethernet driver (10ec:8139 and relatives).
 *
 * The RTL8139 is the most widely cloned 100 Mbit NIC ever shipped: it is
 * QEMU's `-device rtl8139`, VirtualBox's "PCnet alternative", and the chip
 * on a very large number of PCI cards and older motherboards.  This driver
 * drives the real data path (no synthesis): a receive RING BUFFER read
 * through CAPR/CBR, four round-robin transmit descriptors, and an INTx
 * handler that drains RX into a software queue and wakes sleepers.
 *
 * Register access is PORT I/O (BAR0), not MMIO.  That is a deliberate
 * choice, and it is also why this driver needs no paging_map(): the 8139
 * exposes the same 256-byte register file at BAR0 (I/O space) and BAR1
 * (memory space), and the I/O route keeps the driver inside the portable
 * include budget that tools/check_width_sweep.py ratchets.
 *
 * DMA WIDTH — the one hardware constraint that bites on real machines:
 * the 8139 is a 32-bit PCI device with 32-bit DMA registers (RBSTART,
 * TSAD0..3).  Every buffer handed to it must live BELOW 4 GiB.  The
 * driver checks this explicitly and refuses with a named error rather
 * than programming a truncated address and silently corrupting memory.
 */

#define RTL8139_VENDOR_ID  0x10EC

/* Device IDs this driver accepts.  0x8139 is the canonical part; the
 * others are the same MAC core behind different marketing numbers
 * (8100/8130 are the "8139C+"-era low-power variants) and clone-vendor
 * rebadges that answer the identical register file. */
#define RTL8139_DEVICE_8139   0x8139   /* RTL8139/8139A/B/C/D, QEMU's model */
#define RTL8139_DEVICE_8138   0x8138   /* RTL8139B CardBus                  */
#define RTL8139_DEVICE_8100   0x8100   /* RTL8100                           */
#define RTL8139_DEVICE_8130   0x8130   /* RTL8139C+/8130                    */

/* Ring/queue geometry. */
#define RTL8139_NUM_TX_DESC   4        /* fixed by the hardware             */
#define RTL8139_PKT_BUF_SIZE  2048
/* RTL8139_RX_BUF_LEN lives in rtl8139_ring.h, next to the arithmetic
 * that depends on it, so the driver and its host test cannot drift. */

/* Probe PCI for a supported Realtek NIC and bring it up.
 * Returns 0 on success, -1 when no supported device is present or the
 * device cannot be programmed (e.g. buffers above the 4 GiB DMA limit). */
int rtl8139_init(void);

/* Copy the 6-byte station address into mac[6]. */
void rtl8139_get_mac(uint8_t mac[6]);

/* Non-zero when the PHY reports link up. */
int rtl8139_link_up(void);

/* Transmit one Ethernet frame.  Returns bytes sent, or -1 on error. */
int rtl8139_send(const void *data, uint32_t len);

/* Non-blocking receive: returns frame length, or 0 when none queued. */
int rtl8139_recv(void *buf, uint32_t bufsize);

/* Timed receive: timeout_ticks == 0 waits indefinitely, otherwise returns
 * 0 on timeout.  Returns < 0 when the link drops. */
int rtl8139_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks);

/* Register this NIC with the netdev layer (after rtl8139_init succeeds). */
void rtl8139_register_netdev(void);

#endif /* AURALITE_DRIVERS_RTL8139_RTL8139_H */
