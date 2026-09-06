#ifndef AURALITE_DRIVERS_BLUETOOTH_BT_HCI_H
#define AURALITE_DRIVERS_BLUETOOTH_BT_HCI_H

/*
 * bt_hci.h — the Bluetooth HCI wire protocol, pure and host-testable
 * (RESIDUE2 T6, ledger RES-39; the tcp_cc.h precedent).
 *
 * QEMU removed its Bluetooth subsystem (the bt-* devices died with it),
 * so no QEMU wire gate can exercise a real controller — but the missing
 * piece the ledger named was never the protocol, it was the transport:
 * bt.c rode UHCI bulk transfers directly.  The protocol half now lives
 * here as pure builders/parsers over HCI Core Specification 5.x byte
 * layouts, pinned by tests/unit/test_bt_hci.c on the host (D2), and
 * bt.c consumes it over the controller-agnostic usb_bulk_transfer().
 */

#include <stdint.h>

/* ---- packet types (first byte of each packet) ----------------------- */

#define HCI_CMD_PKT     0x01
#define HCI_ACL_PKT     0x02
#define HCI_SCO_PKT     0x03
#define HCI_EVT_PKT     0x04

/* ---- command opcodes (OGF << 10 | OCF) ------------------------------- */

#define HCI_RESET                0x0C03
#define HCI_READ_BD_ADDR         0x1009
#define HCI_READ_BUFFER_SIZE     0x1005
#define HCI_READ_LOCAL_VERSION   0x1001
#define HCI_WRITE_SCAN_ENABLE    0x0C1A
#define HCI_INQUIRY              0x0401
#define HCI_INQUIRY_CANCEL       0x0402
#define HCI_SET_EVENT_MASK       0x0C01

/* ---- event codes ------------------------------------------------------ */

#define HCI_EVT_INQUIRY_COMPLETE   0x01
#define HCI_EVT_INQUIRY_RESULT     0x02
#define HCI_EVT_CMD_COMPLETE       0x0E
#define HCI_EVT_CMD_STATUS         0x0F
#define HCI_EVT_NUM_COMPLETED_PKTS 0x13
#define HCI_EVT_LE_META            0x3E

/* ---- wire structs (little endian) -------------------------------------- */

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif

struct hci_cmd_hdr {
    uint8_t  packet_type;   /* HCI_CMD_PKT */
    uint16_t opcode;        /* OGF << 10 | OCF */
    uint8_t  param_len;
} __attribute__((packed));

struct hci_evt_hdr {
    uint8_t  packet_type;   /* HCI_EVT_PKT */
    uint8_t  event;
    uint8_t  param_len;
} __attribute__((packed));

/* Command Complete event parameters (immediately after the header). */
struct hci_cmd_complete {
    uint8_t  num_packets;
    uint16_t opcode;
} __attribute__((packed));

#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* BD_ADDR (Bluetooth Device Address) — 6 bytes. */
typedef struct { uint8_t b[6]; } bd_addr_t;

/* ---- builders ----------------------------------------------------------- */

/* Build an HCI command header (+ optional params already in place).
 * Returns the total packet length (4 + param_len). */
static inline int bt_hci_cmd(uint8_t *buf, uint16_t opcode, uint8_t param_len) {
    struct hci_cmd_hdr *h = (struct hci_cmd_hdr *)buf;
    h->packet_type = HCI_CMD_PKT;
    h->opcode      = opcode;
    h->param_len   = param_len;
    return 4 + (int)param_len;
}

/* ---- parsers (every one validates length; -1 on malformed) ------------- */

/* The Inquiry LAP for the General/Unlimited Inquiry Access Code. */
#define HCI_GIAC_LAP0 0x33
#define HCI_GIAC_LAP1 0x8B
#define HCI_GIAC_LAP2 0x9E

static inline int bt_hci_evt_is(const uint8_t *buf, int len) {
    return buf && len >= (int)sizeof(struct hci_evt_hdr) &&
           buf[0] == HCI_EVT_PKT;
}

static inline uint8_t bt_hci_evt_event(const uint8_t *buf, int len) {
    if (!bt_hci_evt_is(buf, len)) return 0;
    return buf[1];
}

static inline uint8_t bt_hci_evt_param_len(const uint8_t *buf, int len) {
    if (!bt_hci_evt_is(buf, len)) return 0;
    return buf[2];
}

/* Command Complete: the opcode the completion answers (or 0). */
static inline uint16_t bt_hci_cc_opcode(const uint8_t *buf, int len) {
    if (!bt_hci_evt_is(buf, len)) return 0;
    if (buf[1] != HCI_EVT_CMD_COMPLETE) return 0;
    if (len < 3 + (int)sizeof(struct hci_cmd_complete)) return 0;
    return (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
}

/* The status byte: Command Status carries it at param offset 0; a
 * Command Complete for a status-returning command carries it right
 * after the CC parameters.  Returns 0 (success) only when the event is
 * well-formed AND reports success; -1 when malformed. */
static inline int bt_hci_status(const uint8_t *buf, int len) {
    if (!bt_hci_evt_is(buf, len)) return -1;
    if (buf[1] == HCI_EVT_CMD_STATUS) {
        if (len < 4) return -1;
        return buf[3];
    }
    if (buf[1] == HCI_EVT_CMD_COMPLETE) {
        if (len < 7) return -1;
        return buf[6];
    }
    return 0;   /* informational events carry no status */
}

/* Read BD_ADDR return: the 6 address bytes at param offset 6. */
static inline int bt_hci_bd_addr(const uint8_t *buf, int len, uint8_t out[6]) {
    if (!bt_hci_evt_is(buf, len)) return -1;
    if (buf[1] != HCI_EVT_CMD_COMPLETE) return -1;
    if (bt_hci_cc_opcode(buf, len) != HCI_READ_BD_ADDR) return -1;
    if (len < 3 + 3 + 1 + 6) return -1;
    for (int i = 0; i < 6; i++) out[i] = buf[7 + i];
    return 0;
}

/* Inquiry Result: number of responses, and device i's BD_ADDR.
 * Per-response record: BD_ADDR[6] + 4 protocol bytes + class[3] + clock[2]. */
#define HCI_INQ_REC_LEN 14

static inline int bt_hci_inq_count(const uint8_t *buf, int len) {
    if (!bt_hci_evt_is(buf, len)) return -1;
    if (buf[1] != HCI_EVT_INQUIRY_RESULT) return -1;
    return buf[3];
}

static inline int bt_hci_inq_addr(const uint8_t *buf, int len, int i,
                                  uint8_t out[6]) {
    int n = bt_hci_inq_count(buf, len);
    if (n <= 0 || i >= n) return -1;
    int off = 4 + i * HCI_INQ_REC_LEN;
    if (len < off + 6) return -1;
    for (int k = 0; k < 6; k++) out[k] = buf[off + k];
    return 0;
}

#endif /* AURALITE_DRIVERS_BLUETOOTH_BT_HCI_H */
