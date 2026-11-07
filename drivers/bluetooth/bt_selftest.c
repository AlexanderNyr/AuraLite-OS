/* bt_selftest.c — a scripted Bluetooth controller, in the kernel build
 * (RESIDUE3 C6).
 *
 * The responder is the one from tests/unit/test_bt_transport.c, byte for byte:
 * Command Complete for Reset / Read Local Version / Read BD_ADDR, Command
 * Status + two Inquiry Results + Inquiry Complete for an inquiry.  The point
 * is that bt.c and bt_transport.c are driven exactly as they would be by a
 * dongle; only the wire is replaced.
 */
#include "drivers/bluetooth/bt_selftest.h"

#include <stdint.h>

#include "drivers/bluetooth/bt_hci.h"
#include "drivers/bluetooth/bt_transport.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

#define STQ 16

typedef struct {
    uint8_t evt[STQ][128];
    int     evt_len[STQ];
    int     head, tail;
    int     cmds;
    uint16_t last_opcode;
    int     failures;
} st_ctrl_t;

static st_ctrl_t sc;

static const uint8_t ST_BD_ADDR[6] = { 0x78, 0x56, 0x34, 0x12, 0x9A, 0xBC };
static const uint8_t ST_PEER1[6]   = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t ST_PEER2[6]   = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

static void sc_push(const uint8_t *pkt, int len) {
    if (sc.head - sc.tail >= STQ || len > 128) return;
    int i = sc.head % STQ;
    memcpy(sc.evt[i], pkt, (size_t)len);
    sc.evt_len[i] = len;
    sc.head++;
}

static void sc_cc(uint16_t opcode, uint8_t status, const uint8_t *ret,
                  int ret_len) {
    uint8_t e[128];
    e[0] = HCI_EVT_PKT;
    e[1] = HCI_EVT_CMD_COMPLETE;
    e[2] = (uint8_t)(3 + ret_len);
    e[3] = 1;
    e[4] = (uint8_t)(opcode & 0xFF);
    e[5] = (uint8_t)(opcode >> 8);
    e[6] = status;
    for (int i = 0; i < ret_len; i++) e[7 + i] = ret[i];
    sc_push(e, 7 + ret_len);
}

static void sc_inq_result(const uint8_t addr[6], uint8_t tag) {
    uint8_t e[4 + HCI_INQ_REC_LEN];
    memset(e, 0, sizeof(e));
    e[0] = HCI_EVT_PKT;
    e[1] = HCI_EVT_INQUIRY_RESULT;
    e[2] = (uint8_t)(1 + HCI_INQ_REC_LEN);
    e[3] = 1;
    memcpy(e + 4, addr, 6);
    e[4 + 9] = tag;
    sc_push(e, (int)sizeof(e));
}

static int sc_cmd_out(void *ctx, const uint8_t *pkt, uint16_t len) {
    (void)ctx;
    if (len < 4 || pkt[0] != HCI_CMD_PKT) return -1;
    uint16_t opcode = (uint16_t)(pkt[1] | ((uint16_t)pkt[2] << 8));
    sc.cmds++;
    sc.last_opcode = opcode;

    switch (opcode) {
    case HCI_RESET:
        sc_cc(HCI_RESET, 0, NULL, 0);
        break;
    case HCI_READ_LOCAL_VERSION: {
        /* HCI 5.3, revision 0x0F23, LMP 12, manufacturer 0x001D (CSR),
         * LMP subversion 0x001F. */
        uint8_t ret[8] = { 12, 0x23, 0x0F, 12, 0x1D, 0x00, 0x1F, 0x00 };
        sc_cc(HCI_READ_LOCAL_VERSION, 0, ret, 8);
        break;
    }
    case HCI_READ_BD_ADDR:
        sc_cc(HCI_READ_BD_ADDR, 0, ST_BD_ADDR, 6);
        break;
    case HCI_INQUIRY: {
        uint8_t st[7] = { HCI_EVT_PKT, HCI_EVT_CMD_STATUS, 4, 0, 1,
                          (uint8_t)(HCI_INQUIRY & 0xFF),
                          (uint8_t)(HCI_INQUIRY >> 8) };
        sc_push(st, 7);
        sc_inq_result(ST_PEER1, 1);
        sc_inq_result(ST_PEER2, 2);
        {
            uint8_t done[4] = { HCI_EVT_PKT, HCI_EVT_INQUIRY_COMPLETE, 1, 0 };
            sc_push(done, 4);
        }
        break;
    }
    default: {
        uint8_t st[7] = { HCI_EVT_PKT, HCI_EVT_CMD_STATUS, 4, 0, 1,
                          (uint8_t)(opcode & 0xFF),
                          (uint8_t)(opcode >> 8) };
        sc_push(st, 7);
        break;
    }
    }
    return 0;
}

static int sc_evt_in(void *ctx, uint8_t *buf, uint16_t bufsize) {
    (void)ctx;
    if (sc.tail == sc.head) return 0;
    int i = sc.tail % STQ;
    int n = sc.evt_len[i];
    if (n > bufsize) n = bufsize;
    memcpy(buf, sc.evt[i], (size_t)n);
    sc.tail++;
    return n;
}

static void st_check(int ok, const char *what) {
    if (!ok) {
        sc.failures++;
        kprintf("[bt] selftest: FAIL %s\n", what);
    }
}

void bt_selftest_run(void) {
    memset(&sc, 0, sizeof(sc));
    sc.failures = 0;

    bt_io_t io;
    memset(&io, 0, sizeof(io));
    io.ctx = &sc;
    io.cmd_out = sc_cmd_out;
    io.evt_in = sc_evt_in;

    bt_ctrl_t st;
    memset(&st, 0, sizeof(st));

    if (bt_transport_bringup(&io, &st) != 0) {
        kprintf("[bt] selftest: FAIL bring-up (status=%d)\n", st.last_status);
        return;
    }
    kprintf("[bt] selftest: bring-up -> %d command(s), HCI version code 0x%02x "
            "(5.3) rev 0x%04x, manufacturer 0x%04x (CSR)\n",
            sc.cmds, (unsigned)st.hci_version,
            (unsigned)st.hci_revision, (unsigned)st.manufacturer);
    kprintf("[bt] selftest: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x read from a "
            "Command Complete\n",
            st.bd_addr[5], st.bd_addr[4], st.bd_addr[3],
            st.bd_addr[2], st.bd_addr[1], st.bd_addr[0]);

    st_check(sc.cmds == 3, "Reset + Read Local Version + Read BD_ADDR");
    st_check(st.reset_done == 1, "Reset completed with status 0");
    st_check(st.have_bd_addr == 1, "BD_ADDR kept from the controller");
    st_check(st.bd_addr[0] == 0x78 && st.bd_addr[5] == 0xBC,
             "BD_ADDR in wire order (LSB first)");
    st_check(st.manufacturer == 0x001D, "manufacturer id decoded");
    st_check(st.hci_version == 12 && st.lmp_version == 12,
             "HCI and LMP version decoded");

    int n = bt_transport_inquiry(&io, &st, 4, 0);
    st_check(n == 2, "inquiry returned both reported addresses");
    st_check(st.inquiry_complete == 1, "inquiry completed");
    st_check(st.inquiry_addr[0][0] == 0x11 && st.inquiry_addr[1][5] == 0xFF,
             "inquiry addresses in wire order");
    kprintf("[bt] selftest: inquiry -> %d device(s), GIAC LAP %02x%02x%02x, "
            "first %02x:%02x:%02x:%02x:%02x:%02x\n",
            n, HCI_GIAC_LAP2, HCI_GIAC_LAP1, HCI_GIAC_LAP0,
            st.inquiry_addr[0][5], st.inquiry_addr[0][4], st.inquiry_addr[0][3],
            st.inquiry_addr[0][2], st.inquiry_addr[0][1], st.inquiry_addr[0][0]);

    char why[192];
    int refused = bt_transport_pair_refuse(why, sizeof(why));
    st_check(refused == -1 && why[0] != 0, "pairing refused with a reason");
    kprintf("[bt] selftest: pairing -> refused (%s)\n", why);

    /* The negative control that matters: a controller answering the wrong
     * opcode must not be mistaken for success. */
    memset(&sc, 0, sizeof(sc));
    sc.cmds = 0;
    {
        /* Queue a completion that answers Read BD_ADDR while the driver is
         * about to send Reset: the opcode mismatch must be refused. */
        uint8_t e[7] = { HCI_EVT_PKT, HCI_EVT_CMD_COMPLETE, 3, 1,
                         (uint8_t)(HCI_READ_BD_ADDR & 0xFF),
                         (uint8_t)(HCI_READ_BD_ADDR >> 8), 0 };
        sc_push(e, 7);
        bt_ctrl_t badst;
        memset(&badst, 0, sizeof(badst));
        int rc = bt_transport_cmd(&io, &badst, HCI_RESET, NULL, 0);
        st_check(rc == -1, "a completion for the wrong opcode is refused");
        kprintf("[bt] selftest: wrong-opcode control -> refused (%d)\n", rc);
    }

    if (sc.failures == 0)
        kprintf("[bt] selftest: PASS HCI transport round trip (no controller "
                "attached; QEMU has no Bluetooth device)\n");
    else
        kprintf("[bt] selftest: FAIL %d check(s)\n", sc.failures);
}
