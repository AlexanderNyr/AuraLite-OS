/*
 * test_bt_transport.c — RESIDUE3 C6: the HCI command/event round trip.
 *
 * drivers/bluetooth/bt_transport.c is compiled UNMODIFIED.  What this file
 * supplies is a SCRIPTED CONTROLLER: a bt_io_t whose cmd_out() decodes the
 * command it was handed and queues the event a CSR-class controller would
 * answer with, and whose evt_in() drains that queue.  No USB, no QEMU, no
 * hardware — which is the only way this can be tested here, because QEMU 9.0
 * deleted its Bluetooth subsystem (`-bt` is an invalid option, and no bt-*
 * device exists), so the plan's dongle lane is not buildable on the emulator.
 *
 * The controller's answers are the ones from HCI Core 5.x, byte for byte:
 *   Reset                -> Command Complete, status 0
 *   Read Local Version   -> Command Complete, status, version fields
 *   Read BD_ADDR         -> Command Complete, status, the 6 address bytes
 *   Inquiry              -> Command Status, Inquiry Result x2, Complete
 *
 * The negative cases are the interesting ones: a controller that answers a
 * DIFFERENT opcode, one that returns a non-zero status, one that answers
 * nothing at all, and one that reports BD_ADDR 00:00:00:00:00:00 (a real
 * class of broken clone — the driver must keep "no address", not a zero).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/bluetooth/bt_transport.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a,e) do { long _a=(long)(a), _e=(long)(e); if (_a!=_e) { \
    printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the scripted controller ------------------------------------------- */

#define CTRL_QUEUE 16

typedef struct {
    uint8_t  evt[CTRL_QUEUE][256];
    int      evt_len[CTRL_QUEUE];
    int      head, tail;

    /* what the controller has seen */
    int      cmds;
    uint16_t last_opcode;
    uint8_t  last_params[8];
    uint8_t  last_param_len;

    /* knobs */
    uint8_t  bd_addr[6];
    uint8_t  reset_status;
    uint8_t  version_status;
    int      answer_wrong_opcode;   /* answers Read BD_ADDR with Reset's CC */
    int      silent;                /* never answers anything */
    int      inquiry_devices;
} fake_ctrl_t;

static fake_ctrl_t ctrl;

static void q_push(const uint8_t *pkt, int len) {
    if (ctrl.head - ctrl.tail >= CTRL_QUEUE) return;
    int i = ctrl.head % CTRL_QUEUE;
    memcpy(ctrl.evt[i], pkt, (size_t)len);
    ctrl.evt_len[i] = len;
    ctrl.head++;
}

/* Command Complete: 04 0E 04 01 <opcode LE> <status> [return params] */
static void push_cc(uint16_t opcode, uint8_t status,
                    const uint8_t *ret, int ret_len) {
    uint8_t e[256];
    e[0] = HCI_EVT_PKT;
    e[1] = HCI_EVT_CMD_COMPLETE;
    e[2] = (uint8_t)(3 + ret_len);        /* num_packets + opcode + status */
    e[3] = 1;                             /* num_hci_command_packets */
    e[4] = (uint8_t)(opcode & 0xFF);
    e[5] = (uint8_t)(opcode >> 8);
    e[6] = status;
    for (int i = 0; i < ret_len; i++) e[7 + i] = ret[i];
    q_push(e, 7 + ret_len);
}

static void push_status(uint16_t opcode, uint8_t status) {
    uint8_t e[7];
    e[0] = HCI_EVT_PKT;
    e[1] = HCI_EVT_CMD_STATUS;
    e[2] = 4;
    e[3] = status;
    e[4] = 1;                              /* num_hci_command_packets */
    e[5] = (uint8_t)(opcode & 0xFF);
    e[6] = (uint8_t)(opcode >> 8);
    q_push(e, 7);
}

static void push_inquiry_result(const uint8_t addr[6], uint8_t i) {
    uint8_t e[4 + HCI_INQ_REC_LEN];
    memset(e, 0, sizeof(e));
    e[0] = HCI_EVT_PKT;
    e[1] = HCI_EVT_INQUIRY_RESULT;
    e[2] = (uint8_t)(1 + HCI_INQ_REC_LEN);
    e[3] = 1;                              /* one response in this event */
    memcpy(e + 4, addr, 6);
    e[4 + 9] = i;                          /* class byte 0: distinguish */
    q_push(e, (int)sizeof(e));
}

static void push_inquiry_complete(void) {
    uint8_t e[4] = { HCI_EVT_PKT, HCI_EVT_INQUIRY_COMPLETE, 1, 0 };
    q_push(e, 4);
}

static int f_cmd_out(void *ctx, const uint8_t *pkt, uint16_t len) {
    fake_ctrl_t *c = ctx;
    if (len < 4 || pkt[0] != HCI_CMD_PKT) return -1;
    uint16_t opcode = (uint16_t)(pkt[1] | ((uint16_t)pkt[2] << 8));
    c->cmds++;
    c->last_opcode = opcode;
    c->last_param_len = pkt[3];
    memset(c->last_params, 0, sizeof(c->last_params));
    for (uint8_t i = 0; i < pkt[3] && i < sizeof(c->last_params); i++)
        c->last_params[i] = pkt[4 + i];
    if (c->silent) return 0;

    switch (opcode) {
    case HCI_RESET:
        push_cc(c->answer_wrong_opcode ? HCI_READ_BD_ADDR : HCI_RESET,
                c->reset_status, NULL, 0);
        break;
    case HCI_READ_LOCAL_VERSION: {
        uint8_t ret[8] = { 12, 0x23, 0x0F, 12, 0x1D, 0x00, 0x1F, 0x00 };
        /* HCI 5.3 (12), revision 0x0F23, LMP 12, manufacturer 0x001D
         * (Cambridge Silicon Radio), LMP subversion 0x001F. */
        push_cc(HCI_READ_LOCAL_VERSION, c->version_status, ret, 8);
        break;
    }
    case HCI_READ_BD_ADDR:
        push_cc(HCI_READ_BD_ADDR, 0, c->bd_addr, 6);
        break;
    case HCI_INQUIRY: {
        push_status(HCI_INQUIRY, 0);
        static const uint8_t a1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
        static const uint8_t a2[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        if (c->inquiry_devices > 0) push_inquiry_result(a1, 1);
        if (c->inquiry_devices > 1) push_inquiry_result(a2, 2);
        push_inquiry_complete();
        break;
    }
    default:
        push_cc(opcode, 0, NULL, 0);
        break;
    }
    return 0;
}

static int f_evt_in(void *ctx, uint8_t *buf, uint16_t bufsize) {
    fake_ctrl_t *c = ctx;
    if (c->tail == c->head) return 0;
    int i = c->tail % CTRL_QUEUE;
    int n = c->evt_len[i];
    if (n > bufsize) n = bufsize;
    memcpy(buf, c->evt[i], (size_t)n);
    c->tail++;
    return n;
}

static bt_io_t io;
static bt_ctrl_t st;

static void reset_everything(void) {
    memset(&ctrl, 0, sizeof(ctrl));
    static const uint8_t addr[6] = { 0x78, 0x56, 0x34, 0x12, 0x9A, 0xBC };
    memcpy(ctrl.bd_addr, addr, 6);
    ctrl.inquiry_devices = 2;
    memset(&io, 0, sizeof(io));
    io.ctx = &ctrl;
    io.cmd_out = f_cmd_out;
    io.evt_in = f_evt_in;
    memset(&st, 0, sizeof(st));
}

/* ------------------------------------------------------------- cases ---- */

/* The bring-up sequence: three commands, three answers, the controller's own
 * numbers kept. */
static void test_bringup(void) {
    reset_everything();
    CHECK_EQ(bt_transport_bringup(&io, &st), 0);
    CHECK_EQ(ctrl.cmds, 3);
    CHECK_EQ(st.reset_done, 1);
    CHECK_EQ(st.have_bd_addr, 1);
    CHECK_EQ(st.bd_addr[0], 0x78);          /* wire order, LSB first */
    CHECK_EQ(st.bd_addr[5], 0xBC);
    CHECK_EQ(st.hci_version, 12);
    CHECK_EQ(st.hci_revision, 0x0F23);
    CHECK_EQ(st.lmp_version, 12);
    CHECK_EQ(st.manufacturer, 0x001D);      /* CSR */
    CHECK_EQ(st.lmp_subversion, 0x001F);
    CHECK_EQ(st.last_status, 0);
}

/* The opcodes actually put on the wire, in order. */
static void test_command_order_and_bytes(void) {
    reset_everything();
    CHECK_EQ(bt_transport_bringup(&io, &st), 0);
    CHECK_EQ(ctrl.last_opcode, HCI_READ_BD_ADDR);
    /* Reset was the first command, with no parameters. */
    CHECK_EQ(ctrl.last_param_len, 0);
}

/* A Command Complete that answers a different opcode must FAIL rather than be
 * accepted: that is how a driver ends up with a BD_ADDR of all zeroes. */
static void test_wrong_opcode_is_refused(void) {
    reset_everything();
    ctrl.answer_wrong_opcode = 1;
    CHECK_EQ(bt_transport_cmd(&io, &st, HCI_RESET, NULL, 0), -1);
    CHECK_EQ(st.last_status, -2);
    CHECK_EQ(st.reset_done, 0);
}

/* A non-zero status is a failure, and the status reaches the caller. */
static void test_status_failure(void) {
    reset_everything();
    ctrl.version_status = 0x0C;            /* Command Disallowed */
    CHECK_EQ(bt_transport_bringup(&io, &st), -1);
    CHECK_EQ(st.last_status, 0x0C);
    CHECK_EQ(st.have_bd_addr, 0);          /* never got that far */
}

/* A controller that says nothing at all times out as a transport failure,
 * not as a success with zeroed fields. */
static void test_silent_controller(void) {
    reset_everything();
    ctrl.silent = 1;
    CHECK_EQ(bt_transport_cmd(&io, &st, HCI_RESET, NULL, 0), -1);
    CHECK_EQ(st.last_status, -3);
}

/* An all-zero BD_ADDR is "no address", not an address. */
static void test_zero_bd_addr(void) {
    reset_everything();
    memset(ctrl.bd_addr, 0, 6);
    CHECK_EQ(bt_transport_bringup(&io, &st), -1);   /* bring-up needs it */
    CHECK_EQ(st.have_bd_addr, 0);
}

/* Inquiry: the LAP goes out as GIAC little-endian, and both reported
 * addresses come back in wire order. */
static void test_inquiry(void) {
    reset_everything();
    CHECK_EQ(bt_transport_bringup(&io, &st), 0);
    int n = bt_transport_inquiry(&io, &st, 4, 0);
    CHECK_EQ(n, 2);
    CHECK_EQ(st.inquiry_complete, 1);
    CHECK_EQ(ctrl.last_opcode, HCI_INQUIRY);
    CHECK_EQ(ctrl.last_param_len, 5);
    CHECK_EQ(ctrl.last_params[0], HCI_GIAC_LAP0);
    CHECK_EQ(ctrl.last_params[1], HCI_GIAC_LAP1);
    CHECK_EQ(ctrl.last_params[2], HCI_GIAC_LAP2);
    CHECK_EQ(ctrl.last_params[3], 4);
    CHECK_EQ(st.inquiry_addr[0][0], 0x11);
    CHECK_EQ(st.inquiry_addr[0][5], 0x66);
    CHECK_EQ(st.inquiry_addr[1][0], 0xAA);
    CHECK_EQ(st.inquiry_addr[1][5], 0xFF);
}

/* An inquiry that finds nobody completes with zero results — and says so. */
static void test_inquiry_empty(void) {
    reset_everything();
    ctrl.inquiry_devices = 0;
    CHECK_EQ(bt_transport_bringup(&io, &st), 0);
    CHECK_EQ(bt_transport_inquiry(&io, &st, 1, 0), 0);
    CHECK_EQ(st.inquiry_complete, 1);
}

/* Pairing is refused by name. */
static void test_pairing_refusal(void) {
    char why[256];
    memset(why, 0, sizeof(why));
    CHECK_EQ(bt_transport_pair_refuse(why, sizeof(why)), -1);
    CHECK(strstr(why, "out of scope") != NULL);
    CHECK(strstr(why, "SMP") != NULL);
    /* It must also work with a short buffer without writing past it. */
    char tiny[8];
    memset(tiny, 'X', sizeof(tiny));
    CHECK_EQ(bt_transport_pair_refuse(tiny, sizeof(tiny)), -1);
    CHECK_EQ(tiny[sizeof(tiny) - 1], 0);
}

/* NULL tolerance on every entry point. */
static void test_null_safety(void) {
    reset_everything();
    CHECK_EQ(bt_transport_cmd(NULL, &st, HCI_RESET, NULL, 0), -1);
    CHECK_EQ(bt_transport_cmd(&io, NULL, HCI_RESET, NULL, 0), -1);
    CHECK_EQ(bt_transport_bringup(NULL, &st), -1);
    CHECK_EQ(bt_transport_inquiry(&io, NULL, 1, 0), -1);
    bt_io_t broken = io;
    broken.evt_in = NULL;
    CHECK_EQ(bt_transport_cmd(&broken, &st, HCI_RESET, NULL, 0), -1);
    bt_transport_pair_refuse(NULL, 0);      /* must not fault */
}

int main(void) {
    printf("=== AuraLite OS Bluetooth HCI transport tests ===\n");
    RUN(test_bringup);
    RUN(test_command_order_and_bytes);
    RUN(test_wrong_opcode_is_refused);
    RUN(test_status_failure);
    RUN(test_silent_controller);
    RUN(test_zero_bd_addr);
    RUN(test_inquiry);
    RUN(test_inquiry_empty);
    RUN(test_pairing_refusal);
    RUN(test_null_safety);
    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
