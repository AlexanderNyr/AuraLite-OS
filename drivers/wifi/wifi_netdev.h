/* wifi_netdev.h — the virtual wireless link as a `struct netdev` (RESIDUE3 C6).
 *
 * WHY THIS EXISTS.  RESIDUE2 T6 proved the MAC layer against a deterministic
 * virtual AP, but the frames stopped at the Wi-Fi driver: nothing above it saw
 * them, so "Wi-Fi works" meant "the MAC built and parsed standards-shaped
 * frames".  C6 asks for a data path, and the IP stack's own seam for that is
 * `struct netdev` (kernel/net/netdev.h): send() takes an Ethernet frame,
 * recv()/recv_wait() hand one back.  This file is the adapter —
 *
 *   netdev.send      -> wifi_send_data()      (Ethernet -> 802.11 + LLC/SNAP)
 *   netdev.recv      -> wifi_virt_eth_pop()   (what the AP queued for us)
 *   netdev.get_mac   -> the Wi-Fi MAC
 *   netdev.link_up   -> the association state
 *
 * and with it in place `net_ping(10.0.2.2)` is a real receipt: ARP request
 * out over 802.11, ARP reply built by the AP from the received bytes, ICMP
 * echo request, echo reply, parsed by kernel/net/net.c.
 *
 * It is a FALLBACK NIC: kernel/net/net.c tries it only after every wired
 * driver has failed, so a boot with a real NIC never changes behaviour.
 */
#ifndef AURALITE_DRIVERS_WIFI_WIFI_NETDEV_H
#define AURALITE_DRIVERS_WIFI_WIFI_NETDEV_H

#include <stdint.h>

/* Bring the virtual link up and register it as a netdev: register the
 * virtual AP backend, scan, associate, then netdev_register().
 * Returns 0 when the association succeeded and the netdev is registered,
 * -1 otherwise (nothing is registered on failure). */
int wifi_virt_netdev_attach(void);

/* 1 when the virtual wireless link is the ACTIVE network device. */
int wifi_virt_netdev_active(void);

#endif /* AURALITE_DRIVERS_WIFI_WIFI_NETDEV_H */
