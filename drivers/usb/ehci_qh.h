/* ehci_qh.h — EHCI queue-head capability words, built as pure data.
 *
 * WHY THIS IS A SEPARATE MODULE (RESIDUE3 C5b).
 *
 * A queue head for a full- or low-speed endpoint that sits behind a
 * high-speed hub has to carry split-transaction fields: the transaction
 * translator's address and port, a start-split mask and a complete-split
 * mask (EHCI 1.0 §4.12).  Get one of those bit positions wrong and the
 * controller schedules nothing, or schedules a transfer the hub never
 * forwards — silently, because the QH is opaque 32-byte words in memory.
 *
 * Those words are pure arithmetic over the endpoint's parameters, so they
 * are built here, in a translation unit with no register access and no
 * allocation, and unit-tested on the host against the bit positions in the
 * specification (tests/unit/test_usb_ehci_qh.c).  ehci.c keeps the parts that
 * need hardware: allocation, linking and polling.
 *
 * Measured limitation, stated rather than hidden (C5b): QEMU's only USB hub,
 * `usb-hub`, is a full-speed device and refuses to attach to a high-speed bus,
 * so an FS/LS device behind a TT on EHCI cannot be constructed there:
 *
 *   qemu-system-x86_64: -device usb-hub,bus=e.0,port=1: Warning: speed
 *   mismatch trying to attach usb device "QEMU USB Hub" (full speed) to bus
 *   "e.0", port "1" (high speed)
 *
 * The split path is therefore verified at the encoding level (here) and its
 * decision is logged by ehci.c on every periodic endpoint; on QEMU the
 * integration case can only assert the non-split branch.
 */
#ifndef AURALITE_DRIVERS_USB_EHCI_QH_H
#define AURALITE_DRIVERS_USB_EHCI_QH_H

#include <stdint.h>

/* EPS encoding, EHCI 1.0 table 3-2 (QH ep_caps1 bits 12-13). */
typedef enum {
    EHCI_QH_SPEED_FULL = 0,
    EHCI_QH_SPEED_LOW  = 1,
    EHCI_QH_SPEED_HIGH = 2,
} ehci_qh_speed_t;

typedef struct {
    uint8_t  dev_addr;        /* 0-127, QH bits 0-6 */
    uint8_t  ep_num;          /* 0-15,  QH bits 8-11 */
    uint8_t  speed;           /* ehci_qh_speed_t, QH bits 12-13 */
    uint16_t max_packet;      /* QH bits 16-26, 11 bits */
    uint8_t  hub_addr;        /* TT address; 0 = attached directly */
    uint8_t  hub_port;        /* TT port number (1-based) */
    uint8_t  mult;            /* QH bits 30-31; 0 is treated as 1 */
    uint8_t  start_microframe;/* S-mask bit, 0-7 */
} ehci_qh_params_t;

typedef struct {
    uint32_t ep_caps1;
    uint32_t ep_caps2;
    int      is_split;        /* 1 when the TT fields are populated */
    uint8_t  s_mask;          /* echo of the encoded masks, for receipts */
    uint8_t  c_mask;
} ehci_qh_caps_t;

/* Fill `out` from `p`.  Always writes both words; nothing here can fail. */
void ehci_qh_build_caps(const ehci_qh_params_t *p, ehci_qh_caps_t *out);

/* True when this endpoint needs split transactions: full/low speed AND
 * reached through a transaction translator.  A full-speed device on a
 * high-speed root port has no TT and cannot be split. */
int ehci_qh_needs_split(uint8_t speed, uint8_t hub_addr);

#endif /* AURALITE_DRIVERS_USB_EHCI_QH_H */
