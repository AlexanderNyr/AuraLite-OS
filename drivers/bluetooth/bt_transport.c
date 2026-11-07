/* bt_transport.c — the HCI command/event round trip over an injected
 * transport (RESIDUE3 C6).
 *
 * The logic here is what bt.c used to do inline: build a command, hand it to
 * whatever the transport is, read one event, match it, and keep the
 * controller's own answers.  Nothing in this file knows about USB — which is
 * what makes it testable against a scripted controller, on the host and in the
 * kernel build.
 *
 * Two behaviours are deliberate and are asserted by the tests:
 *   - a Command Complete whose opcode does not match the command just sent is
 *     treated as a failure, not silently accepted (the controller answering a
 *     different question is exactly the kind of thing that otherwise shows up
 *     later as "the BD_ADDR is zeroes");
 *   - an event of a kind this driver does not use is skipped and the transport
 *     is polled again, because a controller legitimately interleaves
 *     informational events with completions.
 */
#include "drivers/bluetooth/bt_transport.h"

#include <stddef.h>
#include <stdint.h>

#define BT_EVT_POLL_MAX 16

int bt_transport_cmd(const bt_io_t *io, bt_ctrl_t *st, uint16_t opcode,
                     const void *params, uint8_t param_len) {
    if (!io || !io->cmd_out || !io->evt_in || !st) return -1;

    uint8_t pkt[4 + 255];
    int plen = bt_hci_cmd(pkt, opcode, param_len);
    if (plen < 0) return -1;
    if (params && param_len) {
        for (uint8_t i = 0; i < param_len; i++)
            pkt[4 + i] = ((const uint8_t *)params)[i];
    }
    if (io->cmd_out(io->ctx, pkt, (uint16_t)(4 + param_len)) < 0) {
        st->last_status = -1;
        return -1;
    }

    for (int poll = 0; poll < BT_EVT_POLL_MAX; poll++) {
        uint8_t evt[256];
        int n = io->evt_in(io->ctx, evt, sizeof(evt));
        if (n < 0) { st->last_status = -1; return -1; }
        if (n == 0) continue;                      /* nothing pending yet */
        if (!bt_hci_evt_is(evt, n)) continue;      /* not an HCI event */

        uint8_t code = bt_hci_evt_event(evt, n);

        if (code == HCI_EVT_CMD_COMPLETE) {
            uint16_t op = bt_hci_cc_opcode(evt, n);
            if (op != opcode) {
                /* Wrong answer to this question. */
                st->last_status = -2;
                return -1;
            }
            int status = bt_hci_status(evt, n);
            st->last_status = status;
            if (status != 0) return -1;

            if (opcode == HCI_RESET) st->reset_done = 1;
            if (opcode == HCI_READ_BD_ADDR) {
                if (bt_hci_bd_addr(evt, n, st->bd_addr) == 0) {
                    int zero = 1;
                    for (int i = 0; i < 6; i++)
                        if (st->bd_addr[i]) { zero = 0; break; }
                    st->bd_addr_zero = zero;
                    st->have_bd_addr = zero ? 0 : 1;
                    if (zero) { st->last_status = -4; return -1; }
                }
            }
            if (opcode == HCI_READ_LOCAL_VERSION && n >= 3 + 8) {
                /* Params after the 3-byte event header and the 3-byte CC
                 * prefix: status, hci_version, hci_rev(2), lmp_version,
                 * manufacturer(2), lmp_subversion(2). */
                const uint8_t *p = evt + 6;
                st->hci_version     = p[1];
                st->hci_revision    = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
                st->lmp_version     = p[4];
                st->manufacturer    = (uint16_t)(p[5] | ((uint16_t)p[6] << 8));
                st->lmp_subversion  = (uint16_t)(p[7] | ((uint16_t)p[8] << 8));
            }
            return 0;
        }

        if (code == HCI_EVT_CMD_STATUS) {
            int status = bt_hci_status(evt, n);
            st->last_status = status;
            return status == 0 ? 0 : -1;
        }

        /* Informational event (inquiry results arrive this way): handled by
         * bt_transport_inquiry(), or irrelevant here.  Keep polling. */
    }
    st->last_status = -3;                          /* no answer at all */
    return -1;
}

int bt_transport_bringup(const bt_io_t *io, bt_ctrl_t *st) {
    if (bt_transport_cmd(io, st, HCI_RESET, NULL, 0) != 0) return -1;
    if (bt_transport_cmd(io, st, HCI_READ_LOCAL_VERSION, NULL, 0) != 0) return -1;
    if (bt_transport_cmd(io, st, HCI_READ_BD_ADDR, NULL, 0) != 0) return -1;
    return st->have_bd_addr ? 0 : -1;
}

int bt_transport_inquiry(const bt_io_t *io, bt_ctrl_t *st,
                         uint8_t duration, uint8_t max_results) {
    if (!io || !io->cmd_out || !io->evt_in || !st) return -1;

    /* Inquiry (HCI 7.1.1): LAP (GIAC, little-endian), duration in 1.28 s
     * units, max responses (0 = unlimited). */
    uint8_t params[5];
    params[0] = HCI_GIAC_LAP0;
    params[1] = HCI_GIAC_LAP1;
    params[2] = HCI_GIAC_LAP2;
    params[3] = duration;
    params[4] = max_results;

    st->inquiry_count = 0;
    st->inquiry_complete = 0;

    /* The command is acknowledged with Command Status, then results and the
     * completion arrive as events. */
    if (bt_transport_cmd(io, st, HCI_INQUIRY, params, 5) != 0) return -1;

    for (int poll = 0; poll < BT_EVT_POLL_MAX * 8; poll++) {
        uint8_t evt[256];
        int n = io->evt_in(io->ctx, evt, sizeof(evt));
        if (n < 0) return -1;
        if (n == 0) continue;
        if (!bt_hci_evt_is(evt, n)) continue;

        uint8_t code = bt_hci_evt_event(evt, n);
        if (code == HCI_EVT_INQUIRY_RESULT) {
            int cnt = bt_hci_inq_count(evt, n);
            for (int i = 0; i < cnt; i++) {
                if (st->inquiry_count >= BT_INQUIRY_MAX_RESULTS) break;
                if (bt_hci_inq_addr(evt, n, i,
                        st->inquiry_addr[st->inquiry_count]) != 0) continue;
                st->inquiry_pscan[st->inquiry_count] = 0;
                st->inquiry_count++;
            }
            continue;
        }
        if (code == HCI_EVT_INQUIRY_COMPLETE) {
            st->inquiry_complete = 1;
            break;
        }
        if (code == HCI_EVT_CMD_COMPLETE) {
            uint16_t op = bt_hci_cc_opcode(evt, n);
            if (op == HCI_INQUIRY) {
                int status = bt_hci_status(evt, n);
                st->last_status = status;
                if (status != 0) return -1;
            }
            continue;
        }
    }
    return st->inquiry_count;
}

int bt_transport_pair_refuse(char *why, unsigned why_len) {
    static const char msg[] =
        "pairing is out of scope at RESIDUE3 C6: it needs SMP/SSP key "
        "exchange and a link key store, neither of which exists here";
    if (why && why_len) {
        unsigned n = sizeof(msg) - 1;
        if (n >= why_len) n = why_len - 1;
        for (unsigned i = 0; i < n; i++) why[i] = msg[i];
        why[n] = 0;
    }
    return -1;
}
