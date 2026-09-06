#ifndef AURALITE_DRIVERS_E1000E_E1000E_H
#define AURALITE_DRIVERS_E1000E_E1000E_H

#include <stdint.h>

/*
 * Intel 82574L (e1000e) PCI-e NIC driver — RESIDUE2 T6, ledger RES-46.
 *
 * Uses the 82574L's legacy-compatible TX/RX descriptor rings (RFCTL.EXTEN
 * clear) and the shared 8254x MMIO register file through the HHDM.
 * QEMU: -device e1000e.
 */

#define E1000E_VENDOR_ID  0x8086
#define E1000E_DEVICE_82574L 0x10D3

#define E1000E_NUM_TX_DESC  32
#define E1000E_NUM_RX_DESC  64
#define E1000E_PKT_BUF_SIZE 2048

/* Initialise the e1000e: find it on PCI, map MMIO, set up TX/RX rings.
 * Returns 0 on success, -1 if the NIC was not found. */
int e1000e_init(void);

/* netdev backend registration (kernel/net/netdev.h). */
void e1000e_register_netdev(void);

#endif
