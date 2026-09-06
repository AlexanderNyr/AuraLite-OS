/* test_bt_hci.c — host gate for the Bluetooth HCI wire protocol
 * (RESIDUE2 T6, ledger RES-39; drivers/bluetooth/bt_hci.h).
 *
 * The D2 rationale, tcp_cc.h style: QEMU removed its Bluetooth
 * subsystem, so no QEMU wire gate can drive a real controller.  The
 * protocol half is therefore pinned HERE against the HCI Core
 * Specification 5.x byte layouts, deterministically; bt.c consumes the
 * same builders/parsers over the controller-agnostic usb_core
 * transport (the actual RES-39 gap that closed).
 *
 * What is pinned:
 *   - command headers for Reset / Read_BD_ADDR / Read_Local_Version /
 *     Inquiry (opcode endianness, param_len, total length);
 *   - the Inquiry parameter block (GIAC LAP, duration, max responses);
 *   - Command Complete parsing: event id, answered opcode, status;
 *   - Read_BD_ADDR return: the 6 address bytes land in the caller's
 *     buffer at the spec offset;
 *   - Command Status parsing (success vs unknown-command);
 *   - Inquiry Result: per-device records (14 bytes each), multi-device
 *     walks, truncation refused;
 *   - malformed input (short header, wrong packet type, short CC)
 *     fails closed, never reads out of bounds.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/bluetooth/bt_hci.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

static int eq(const uint8_t *a, const uint8_t *b, int n) {
    return memcmp(a, b, (size_t)n) == 0;
}

int main(void) {
    uint8_t buf[64];

    /* ---- command builders ------------------------------------------- */
    {
        int n = bt_hci_cmd(buf, HCI_RESET, 0);
        static const uint8_t want[4] = {0x01, 0x03, 0x0C, 0x00};
        CHECK(n == 4, "Reset cmd length %d", n);
        CHECK(eq(buf, want, 4), "Reset cmd bytes");
    }
    {
        int n = bt_hci_cmd(buf, HCI_READ_BD_ADDR, 0);
        static const uint8_t want[4] = {0x01, 0x09, 0x10, 0x00};
        CHECK(n == 4 && eq(buf, want, 4), "Read_BD_ADDR cmd bytes");
    }
    {
        int n = bt_hci_cmd(buf, HCI_READ_LOCAL_VERSION, 0);
        static const uint8_t want[4] = {0x01, 0x01, 0x10, 0x00};
        CHECK(n == 4 && eq(buf, want, 4), "Read_Local_Version cmd bytes");
    }
    {
        uint8_t p[5] = {HCI_GIAC_LAP0, HCI_GIAC_LAP1, HCI_GIAC_LAP2, 5, 0};
        int n = bt_hci_cmd(buf, HCI_INQUIRY, 5);
        memcpy(buf + 4, p, 5);                  /* driver-side, as bt.c */
        static const uint8_t want[9] =
            {0x01, 0x01, 0x04, 0x05, 0x33, 0x8B, 0x9E, 0x05, 0x00};
        CHECK(n == 9 && eq(buf, want, 9), "Inquiry cmd + GIAC params");
    }

    /* ---- Command Complete for Reset ---------------------------------- */
    {
        static const uint8_t evt[7] = {0x04, 0x0E, 0x04, 0x01,
                                       0x03, 0x0C, 0x00};
        CHECK(bt_hci_evt_is(evt, 7), "CC is an event packet");
        CHECK(bt_hci_evt_event(evt, 7) == HCI_EVT_CMD_COMPLETE, "CC event id");
        CHECK(bt_hci_cc_opcode(evt, 7) == HCI_RESET, "CC answers Reset");
        CHECK(bt_hci_status(evt, 7) == 0, "CC status success");
    }
    {   /* controller refused: status 0x0C */
        static const uint8_t evt[7] = {0x04, 0x0E, 0x04, 0x01,
                                       0x03, 0x0C, 0x0C};
        CHECK(bt_hci_status(evt, 7) == 0x0C, "CC status failure code");
    }

    /* ---- Command Complete for Read_BD_ADDR --------------------------- */
    {
        static const uint8_t evt[13] =
            {0x04, 0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00,
             0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
        uint8_t addr[6] = {0};
        CHECK(bt_hci_cc_opcode(evt, 13) == HCI_READ_BD_ADDR,
              "CC answers Read_BD_ADDR");
        CHECK(bt_hci_status(evt, 13) == 0, "BD_ADDR status success");
        CHECK(bt_hci_bd_addr(evt, 13, addr) == 0, "BD_ADDR extracted");
        CHECK(addr[0] == 0xA1 && addr[5] == 0xF6,
              "BD_ADDR bytes at the spec offset");
    }

    /* ---- Command Status ----------------------------------------------- */
    {
        static const uint8_t ok[7] = {0x04, 0x0F, 0x04, 0x00,
                                      0x01, 0x03, 0x0C};
        static const uint8_t unknown[7] = {0x04, 0x0F, 0x04, 0x01,
                                           0x01, 0x09, 0x10};
        CHECK(bt_hci_evt_event(ok, 7) == HCI_EVT_CMD_STATUS, "CS event id");
        CHECK(bt_hci_status(ok, 7) == 0, "CS pending/success");
        CHECK(bt_hci_status(unknown, 7) == 0x01, "CS unknown-command");
    }

    /* ---- Inquiry Result (1 and 2 devices) ----------------------------- */
    {
        static const uint8_t one[19] =
            {0x04, 0x02, 0x0F, 0x01,
             0x00, 0x11, 0x22, 0x33, 0x44, 0x55,         /* BD_ADDR */
             0x01, 0x00, 0x00,                            /* modes */
             0x25, 0x08, 0x04,                            /* class of device */
             0x32, 0x10};                                 /* clock offset */
        uint8_t addr[6] = {0};
        CHECK(bt_hci_inq_count(one, 19) == 1, "one inquiry response");
        CHECK(bt_hci_inq_addr(one, 19, 0, addr) == 0, "device 0 extracted");
        CHECK(addr[0] == 0x00 && addr[5] == 0x55, "device 0 BD_ADDR bytes");

        uint8_t two[33];
        memcpy(two, one, 19);
        two[3] = 2;
        two[2] = 0x1D;                                    /* 1 + 2*14 */
        memcpy(two + 18, one + 4, 15);                    /* 2nd record */
        two[18] = 0x99;                                   /* distinct lsb */
        CHECK(bt_hci_inq_count(two, 33) == 2, "two inquiry responses");
        CHECK(bt_hci_inq_addr(two, 33, 1, addr) == 0, "device 1 extracted");
        CHECK(addr[0] == 0x99 && addr[5] == 0x55, "device 1 BD_ADDR bytes");
        CHECK(bt_hci_inq_addr(two, 33, 2, addr) == -1, "device 2 refused");

        CHECK(bt_hci_inq_addr(one, 9, 0, addr) == -1,
              "record header truncated refused");
        CHECK(bt_hci_inq_addr(one, 17, 0, addr) == 0,
              "full 14-byte record still readable at len 17");
    }

    /* ---- malformed input fails closed --------------------------------- */
    {
        static const uint8_t short_hdr[2] = {0x04, 0x0E};
        static const uint8_t not_evt[7] = {0x02, 0x0E, 0x04, 0x01,
                                           0x03, 0x0C, 0x00};
        static const uint8_t short_cc[6] = {0x04, 0x0E, 0x04, 0x01,
                                            0x03, 0x0C};
        uint8_t addr[6] = {0};

        CHECK(!bt_hci_evt_is(short_hdr, 2), "short header rejected");
        CHECK(!bt_hci_evt_is(not_evt, 7), "non-event packet rejected");
        CHECK(bt_hci_evt_event(not_evt, 7) == 0, "event id of non-event");
        CHECK(bt_hci_cc_opcode(short_cc, 5) == 0, "short CC opcode");
        CHECK(bt_hci_status(short_cc, 6) == -1, "short CC status");
        static const uint8_t cc_reset_only[7] = {0x04, 0x0E, 0x04, 0x01,
                                                 0x03, 0x0C, 0x00};
        CHECK(bt_hci_bd_addr(cc_reset_only, 7, addr) == -1,
              "BD_ADDR from a Reset CC refused");
        static const uint8_t bad_bd[12] =
            {0x04, 0x0E, 0x09, 0x01, 0x09, 0x10, 0x00,
             0xA1, 0xB2, 0xC3, 0xD4, 0xE5};
        CHECK(bt_hci_bd_addr(bad_bd, 12, addr) == -1,
              "short BD_ADDR payload refused");
    }

    /* ---- informational events carry no status -------------------------- */
    {
        static const uint8_t done[3] = {0x04, 0x01, 0x00};
        CHECK(bt_hci_evt_is(done, 3), "Inquiry Complete is an event");
        CHECK(bt_hci_status(done, 3) == 0, "no status byte, no failure");
    }

    printf("test_bt_hci: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
