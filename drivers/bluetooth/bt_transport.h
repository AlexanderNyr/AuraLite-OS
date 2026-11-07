/* bt_transport.h — the HCI command/event round trip, with the transport
 * injected (RESIDUE3 C6).
 *
 * WHY THIS EXISTS.  bt_hci.h (RESIDUE2 T6) pinned the HCI *bytes*: how a
 * command header is built, how a Command Complete is recognised, where the
 * status byte and the BD_ADDR sit.  What it could not pin is the thing bt.c
 * does with them: send the command, read an event off the transport, match the
 * opcode, keep the controller's own BD_ADDR, collect inquiry results.  That
 * logic lived inline in bt.c next to `usb_bulk_transfer()`, so proving it
 * needed a controller.
 *
 * QEMU cannot supply one.  Upstream removed the whole Bluetooth subsystem
 * (hw/bt) in QEMU 9.0; the `-bt` command line option is gone ("invalid
 * option") and `-device help` lists no usb-bt-dongle or bt-* device, so the
 * plan's "attach -device usb-bt-dongle" lane is not buildable on the emulator
 * this project tests against.  The transport is therefore injected:
 *
 *   bt_io_t.cmd_out  — hand one framed HCI command packet to the controller
 *   bt_io_t.evt_in   — read one HCI event packet from it
 *
 * bt.c binds these to usb_core (bulk OUT / bulk IN on the dongle's endpoints);
 * the unit test and the kernel self-test bind them to a scripted CSR-class
 * controller carried in the tree.  The driver logic under test is identical in
 * all three cases — which is the whole point.
 */
#ifndef AURALITE_DRIVERS_BLUETOOTH_BT_TRANSPORT_H
#define AURALITE_DRIVERS_BLUETOOTH_BT_TRANSPORT_H

#include <stdint.h>

#include "drivers/bluetooth/bt_hci.h"

typedef struct {
    void *ctx;
    /* Hand ONE framed HCI command packet (header + params) to the controller.
     * Returns 0 on success, < 0 when the transport failed. */
    int (*cmd_out)(void *ctx, const uint8_t *pkt, uint16_t len);
    /* Read ONE framed HCI event packet.  Returns its length, 0 when the
     * controller has nothing pending, < 0 on a transport error. */
    int (*evt_in)(void *ctx, uint8_t *buf, uint16_t bufsize);
} bt_io_t;

#define BT_LOCAL_VERSION_MAX 16
#define BT_INQUIRY_MAX_RESULTS 8

typedef struct {
    /* Read Local Version Information (HCI 4.6.3) — the controller's own
     * answer, kept so a receipt can quote it. */
    uint8_t  hci_version;
    uint16_t hci_revision;
    uint8_t  lmp_version;
    uint16_t manufacturer;
    uint16_t lmp_subversion;

    /* Read BD_ADDR (HCI 4.6.5).  Byte order is the wire's (LSB first). */
    uint8_t  bd_addr[6];
    int      have_bd_addr;
    /* A controller that answers Read BD_ADDR with 00:00:00:00:00:00 is a
     * real class of broken clone.  It is NOT an address: the flag says so,
     * bring-up fails, and a receipt can name the shape of the failure. */
    int      bd_addr_zero;

    int      reset_done;
    int      last_status;          /* status of the last command */

    /* Inquiry: addresses the controller reported (HCI 7.1.3). */
    int      inquiry_count;
    uint8_t  inquiry_addr[BT_INQUIRY_MAX_RESULTS][6];
    int      inquiry_pscan[BT_INQUIRY_MAX_RESULTS];
    int      inquiry_complete;
} bt_ctrl_t;

/* Issue one command and consume the matching event.  Returns 0 when the
 * command completed with status 0, < 0 otherwise (`st->last_status` carries
 * the controller's status when it answered). */
int bt_transport_cmd(const bt_io_t *io, bt_ctrl_t *st, uint16_t opcode,
                     const void *params, uint8_t param_len);

/* Reset, Read Local Version, Read BD_ADDR — the bring-up sequence.  Returns 0
 * when all three completed. */
int bt_transport_bringup(const bt_io_t *io, bt_ctrl_t *st);

/* Start an inquiry and consume Inquiry Result / Inquiry Complete events until
 * the controller says it is done.  Returns the number of addresses found, or
 * < 0 on a transport failure. */
int bt_transport_inquiry(const bt_io_t *io, bt_ctrl_t *st,
                         uint8_t duration, uint8_t max_results);

/* Pairing: deliberately not implemented at this phase, and said out loud.
 * Writes the reason into `why` (never NULL-terminates past `why_len`) and
 * returns -1.  This exists so the gap is a named refusal with a receipt
 * instead of a silent missing feature. */
int bt_transport_pair_refuse(char *why, unsigned why_len);

#endif /* AURALITE_DRIVERS_BLUETOOTH_BT_TRANSPORT_H */
