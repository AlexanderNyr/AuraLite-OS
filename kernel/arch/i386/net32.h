/* kernel/arch/i386/net32.h -- e1000 + ARP/ICMP bring-up (I386_PLAN I8).
 *
 * The gate that moved here from I7: DHCP lease + echo reply under
 * qemu-system-i386.  Scope is the gate, no wider: e1000 82540EM
 * (QEMU's model) with one RX and one TX ring in low direct-mapped
 * memory, then DHCP/ARP/ICMP enough to acquire an address and answer
 * a ping check.  Sockets, TCP, DNS -- the full kernel/net port --
 * remain I8 residue tracked in the status matrix; this file is the
 * driver + proof that packets flow both ways on this arch.
 *
 * Width discipline (D6): the e1000 descriptor's buffer address field
 * is 64-bit ON THE WIRE; the low dword gets the paddr, the high dword
 * is written 0 explicitly, not left to struct luck.
 */

#ifndef AURALITE_ARCH_I386_NET32_H
#define AURALITE_ARCH_I386_NET32_H

#include <stdint.h>

/* 0 when the NIC is found and rings are live. */
int  net32_init(void);

/* The boot self-test = the I7-inherited gate:
 *   1. DHCP DISCOVER/OFFER/REQUEST/ACK -> an address on QEMU's SLIRP
 *      (expected 10.0.2.15 from 10.0.2.2).
 *   2. ARP-resolve the gateway.
 *   3. ICMP echo to the gateway; PASS on a matching reply.
 * Returns 0 on PASS. */
int  net32_selftest(void);

#endif /* AURALITE_ARCH_I386_NET32_H */
