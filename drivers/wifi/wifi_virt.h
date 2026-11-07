#ifndef AURALITE_DRIVERS_WIFI_WIFI_VIRT_H
#define AURALITE_DRIVERS_WIFI_WIFI_VIRT_H

/*
 * wifi_virt — the RESIDUE2 T6 reference backend for the Wi-Fi MAC
 * layer: a deterministic virtual AP (no radio claimed).
 */

/* Register the virtual AP as the wifi_driver_t backend and reset its
 * counters.  Idempotent. */
void wifi_virt_register(void);

/* Receipt counters, for the boot self-test's PASS lines. */
uint32_t wifi_virt_data_bytes(void);
int wifi_virt_data_frames(void);

/* ---- RESIDUE3 C6: the virtual AP as a DATA PATH -------------------------
 *
 * RESIDUE2 T6 stopped at "78 bytes rode the LLC/SNAP path": the frames went
 * into the MAC layer and were counted.  C6 makes the AP answer at the IP
 * layer, so the Ethernet frames the IP stack sends come back as frames it
 * parses: an ARP request for the AP's address is answered with a real ARP
 * reply, and an ICMP echo request addressed to it is answered with a real
 * echo reply.  Both are built here from the received bytes (addresses and
 * payload copied, checksums recomputed), encapsulated FromDS, and queued for
 * the netdev seam — which is what makes `net_ping()` an end-to-end receipt
 * over 802.11 rather than a loopback trick.
 *
 * Addresses are in host order, the same convention kernel/net/net.c uses. */
void     wifi_virt_set_ap_ip(uint32_t ap_ip);
uint32_t wifi_virt_ap_ip(void);

/* Number of ARP replies / ICMP echo replies the AP has produced. */
int wifi_virt_arp_replies(void);
int wifi_virt_icmp_replies(void);
/* Data frames that were not addressed to the AP (counted, then dropped). */
int wifi_virt_foreign_frames(void);

/* Frames the AP has queued towards us, and their total byte count. */
int      wifi_virt_eth_frames(void);
uint32_t wifi_virt_eth_bytes(void);

/* Pop one queued Ethernet frame (what the AP transmitted towards us).
 * Returns the frame length, or 0 when the queue is empty. */
int wifi_virt_eth_pop(void *buf, uint32_t bufsize);

/* Reset the C6 counters and drain the queue (used by the boot self-test so
 * its receipt describes this boot's traffic, not a previous run's). */
void wifi_virt_eth_reset(void);

#endif
