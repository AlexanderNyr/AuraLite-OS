#ifndef AURALITE_DRIVERS_E1000_E1000_H
#define AURALITE_DRIVERS_E1000_E1000_H

#include <stdint.h>

/*
 * Intel 82540EM e1000 NIC driver (QEMU's default NIC).
 *
 * Uses legacy (non-descriptor-split) TX/RX descriptor rings with an
 * interrupt-capable RX/TX path. The MMIO register file is accessed through the
 * HHDM.
 */

#define E1000_VENDOR_ID  0x8086

/* Intel e1000 variants commonly exposed by emulators/hypervisors:
 *   0x100E — 82540EM  (QEMU, VirtualBox PRO/1000 MT Desktop)
 *   0x100F — 82545EM  (VMware e1000, VirtualBox PRO/1000 MT Server)
 *   0x1004 — 82543GC  (VirtualBox PRO/1000 T Server)
 *
 * The driver uses the legacy 8254x register/descriptor interface shared by
 * these adapters. Prefer 0x100E/0x100F in VM settings; e1000e/vmxnet3 are not
 * supported by this driver.
 */
#define E1000_DEVICE_82540EM 0x100E
#define E1000_DEVICE_82545EM 0x100F
#define E1000_DEVICE_82543GC 0x1004
#define E1000_DEVICE_ID      E1000_DEVICE_82540EM

#define E1000_NUM_TX_DESC  8
#define E1000_NUM_RX_DESC  8
#define E1000_PKT_BUF_SIZE 2048

/* Initialise the e1000: find it on PCI, map MMIO, set up TX/RX rings.
 * Returns 0 on success, -1 if the NIC was not found. */
int e1000_init(void);

/* Get our MAC address (6 bytes written into mac[6]). */
void e1000_get_mac(uint8_t mac[6]);

/* Return non-zero when the emulated NIC reports link-up. */
int e1000_link_up(void);

/* Send a packet. Returns bytes sent, or -1 on error. */
int e1000_send(const void *data, uint32_t len);

/*
 * Poll for a received packet. If a packet is available, copies it into buf
 * (up to bufsize bytes) and returns the packet length. Returns 0 if no packet
 * is available.
 */
int e1000_recv(void *buf, uint32_t bufsize);

/*
 * Timed receive variant for socket/TCP paths that can sleep.
 * timeout_ticks == 0 means wait indefinitely; otherwise return 0 on timeout.
 */
int e1000_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks);

/* Blocking receive variant for socket/TCP paths that can sleep. */
int e1000_recv_blocking(void *buf, uint32_t bufsize);

/* Register the e1000 as a netdev backend (call after e1000_init succeeds). */
void e1000_register_netdev(void);

#endif /* AURALITE_DRIVERS_E1000_E1000_H */
