/*
 * test_usb_hid_report.c — RESIDUE3 C5b (RES-62): the HID report-descriptor
 * parser, driven against fixtures.
 *
 * drivers/usb/hid_report.c is compiled UNMODIFIED.  What this file supplies is
 * descriptors: a boot keyboard, a two-report-ID device (keyboard + consumer
 * controls), a truncated descriptor and one with nothing usable in it.
 *
 * Why it matters: before C5b the parser kept ONE `report_id` for the whole
 * descriptor and one running bit cursor, so a device declaring IDs 1 and 2
 * folded report 2's fields onto ID 1 and started its layout at bit 8 of the
 * wrong report.  Every case below is one that a QEMU-only parser never saw --
 * QEMU's `usb-kbd` and `usb-tablet` happen to use layouts where the bug is
 * invisible.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/usb/hid_report.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a,e) do { long _a=(long)(a), _e=(long)(e); if (_a!=_e) { \
    printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the classic 8-byte boot keyboard, no Report ID ------------------- */
static const uint8_t desc_boot_kbd[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x05, 0x07,        /*   Usage Page (Keyboard) */
    0x19, 0xE0,        /*   Usage Min (0xE0) */
    0x29, 0xE7,        /*   Usage Max (0xE7) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x01,        /*   Logical Max (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data,Var,Abs)  -> 8 modifier bits */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x01,        /*   Input (Const)         -> 1 reserved byte */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Min (0) */
    0x25, 0x65,        /*   Logical Max (101) */
    0x05, 0x07,        /*   Usage Page (Keyboard) */
    0x19, 0x00,        /*   Usage Min (0) */
    0x29, 0x65,        /*   Usage Max (101) */
    0x81, 0x00,        /*   Input (Data,Array)    -> 6 keycodes */
    0xC0,              /* End Collection */
};

/* ---- two reports: ID 1 keyboard (mods + 2 keys), ID 2 consumer --------- */
static const uint8_t desc_two_ids[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */

    0x85, 0x01,        /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Keyboard) */
    0x19, 0xE0,        /*   Usage Min (0xE0) */
    0x29, 0xE7,        /*   Usage Max (0xE7) */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,        /*   Input: 8 modifier bits, bits 0..7   */
    0x95, 0x02,        /*   Report Count (2) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,        /*   Input: 2 keycodes, bits 8..23      */

    0x85, 0x02,        /*   Report ID (2)  -- new report, cursor restarts */
    0x05, 0x0C,        /*   Usage Page (Consumer) */
    0x09, 0xE9,        /*   Usage (Volume Up) */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x04,
    0x81, 0x02,        /*   Input: 4 bits, bits 0..3 of report 2 */
    0xC0,              /* End Collection */
};

/* Padding-only: parses, but declares nothing this driver interprets. */
static const uint8_t desc_padding_only[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x75, 0x08, 0x95, 0x04, 0x81, 0x01,   /* Input (Const) x4 */
    0xC0,
};

static void test_boot_keyboard(void) {
    hid_report_t r;
    CHECK_EQ(hid_report_parse(&r, desc_boot_kbd, sizeof(desc_boot_kbd)), 0);
    CHECK_EQ(r.has_report_id, 0);
    CHECK_EQ(r.is_keyboard, 1);
    CHECK_EQ(r.is_mouse, 0);
    /* 8 modifier bits + 6 keycodes; the reserved byte is constant and is not
     * a field, but it DOES advance the cursor. */
    CHECK_EQ(r.field_count, 14);
    CHECK_EQ(hid_report_wire_len(&r), 8);
    CHECK_EQ(r.fields[0].report_id, 0);
    CHECK_EQ(r.fields[0].bit_offset, 0);
    CHECK_EQ(r.fields[0].size, 1);
    /* The first keycode starts after the 8 modifier bits and the reserved
     * byte: bit 16. */
    CHECK_EQ(r.fields[8].bit_offset, 16);
    CHECK_EQ(r.fields[8].size, 8);
    CHECK_EQ(r.fields[8].usage_page, HID_USAGE_PAGE_KEYBOARD);
}

static void test_two_report_ids(void) {
    hid_report_t r;
    CHECK_EQ(hid_report_parse(&r, desc_two_ids, sizeof(desc_two_ids)), 0);
    CHECK_EQ(r.has_report_id, 1);
    CHECK_EQ(r.report_id_count, 2);
    CHECK_EQ(r.report_ids[0], 1);
    CHECK_EQ(r.report_ids[1], 2);

    /* Report 1: 8 mod bits + 2 keycodes = 24 bits. */
    CHECK_EQ(hid_report_bits(&r, 1), 24);
    /* Report 2: 4 bits -- NOT 8+ something, because the cursor restarts at
     * the second Report ID item.  This is the assertion the old parser
     * failed: it would have reported 28 bits for "report 2". */
    CHECK_EQ(hid_report_bits(&r, 2), 4);

    /* Fields carry their own report ID. */
    CHECK_EQ(r.fields[0].report_id, 1);
    CHECK_EQ(r.fields[0].usage, 0xE0);
    uint8_t last_kbd = 0;
    for (uint8_t i = 0; i < r.field_count; i++)
        if (r.fields[i].report_id == 1) last_kbd = i;
    CHECK_EQ(r.fields[last_kbd].report_id, 1);
    /* The consumer-control bits are report 2, starting at bit 0 of ITS
     * report (four one-bit fields). */
    int consumer = -1, consumer_n = 0;
    for (uint8_t i = 0; i < r.field_count; i++) {
        if (r.fields[i].report_id != 2) continue;
        if (consumer < 0) consumer = i;
        consumer_n++;
    }
    CHECK_EQ(consumer_n, 4);
    CHECK(consumer >= 0);
    CHECK_EQ(r.fields[consumer].bit_offset, 0);
    CHECK_EQ(r.fields[consumer].usage_page, 0x0C);

    /* Wire length is the largest body (3 bytes) plus the ID prefix. */
    CHECK_EQ(hid_report_wire_len(&r), 4);
}

static void test_body_split(void) {
    hid_report_t r;
    uint32_t body_len = 0;
    uint8_t id = 0;
    CHECK_EQ(hid_report_parse(&r, desc_two_ids, sizeof(desc_two_ids)), 0);

    /* Report 1: ID 0x01, mods = 0x02 (Left Shift), keycodes {0x04, 0x00}. */
    const uint8_t rep1[] = { 0x01, 0x02, 0x04, 0x00 };
    const uint8_t *body = hid_report_body(&r, rep1, sizeof(rep1), &body_len, &id);
    CHECK(body != NULL);
    CHECK_EQ(id, 1);
    CHECK_EQ(body_len, 3);
    int kbd_mod = -1, kbd_key0 = -1;
    for (uint8_t i = 0; i < r.field_count; i++) {
        if (r.fields[i].report_id != 1) continue;
        if (r.fields[i].usage == 0xE1) kbd_mod = i;          /* Left Shift */
        if (r.fields[i].size == 8 && kbd_key0 < 0) kbd_key0 = i;
    }
    CHECK(kbd_mod >= 0);
    CHECK_EQ(hid_report_field_value(body, &r.fields[kbd_mod]), 1);
    CHECK(kbd_key0 >= 0);
    CHECK_EQ(hid_report_field_value(body, &r.fields[kbd_key0]), 0x04);

    /* Report 2: ID 0x02, one bit set -> decoded against ITS layout. */
    const uint8_t rep2[] = { 0x02, 0x01 };
    body = hid_report_body(&r, rep2, sizeof(rep2), &body_len, &id);
    CHECK(body != NULL);
    CHECK_EQ(id, 2);
    CHECK_EQ(body_len, 1);
    int consumer = -1;
    for (uint8_t i = 0; i < r.field_count; i++)
        if (r.fields[i].report_id == 2) { consumer = i; break; }
    CHECK(consumer >= 0);
    CHECK_EQ(hid_report_field_value(body, &r.fields[consumer]), 1);
}

static void test_negative_unknown_report_id(void) {
    hid_report_t r;
    uint32_t body_len = 123;
    uint8_t id = 99;
    CHECK_EQ(hid_report_parse(&r, desc_two_ids, sizeof(desc_two_ids)), 0);
    /* Report ID 7 was never declared: refused, and the outputs are cleared. */
    const uint8_t rep[] = { 0x07, 0xFF, 0xFF };
    CHECK(hid_report_body(&r, rep, sizeof(rep), &body_len, &id) == NULL);
    CHECK_EQ(body_len, 0);
    CHECK_EQ(id, 0);
    /* An ID byte with no body is not a report either. */
    const uint8_t bare[] = { 0x01 };
    CHECK(hid_report_body(&r, bare, sizeof(bare), &body_len, &id) == NULL);
    CHECK_EQ(hid_report_has_id(&r, 7), 0);
    CHECK_EQ(hid_report_has_id(&r, 1), 1);
}

static void test_negative_truncated(void) {
    hid_report_t r;
    /* Cut the descriptor in the middle of the last Input item's payload:
     * the parser must stop cleanly, keep what it understood, and not read
     * past the buffer (ASan is on for this binary). */
    uint8_t truncated[sizeof(desc_boot_kbd)];
    memcpy(truncated, desc_boot_kbd, sizeof(truncated));
    uint16_t len = (uint16_t)(sizeof(truncated) - 1);
    int rc = hid_report_parse(&r, truncated, len);
    CHECK_EQ(rc, 0);                 /* what was complete still counts */
    CHECK(r.field_count > 0);
    /* A descriptor cut down to its first item parses to nothing usable. */
    CHECK_EQ(hid_report_parse(&r, desc_boot_kbd, 2), -1);
}

static void test_negative_nothing_interpretable(void) {
    hid_report_t r;
    CHECK_EQ(hid_report_parse(&r, desc_padding_only, sizeof(desc_padding_only)), -1);
    CHECK_EQ(r.field_count, 0);
    /* Empty input is refused rather than "parsed". */
    CHECK_EQ(hid_report_parse(&r, desc_padding_only, 0), -1);
}

static void test_no_report_id_body_is_whole_report(void) {
    hid_report_t r;
    uint32_t body_len = 0;
    uint8_t id = 77;
    CHECK_EQ(hid_report_parse(&r, desc_boot_kbd, sizeof(desc_boot_kbd)), 0);
    const uint8_t rep[8] = { 0x02, 0x00, 0x04, 0, 0, 0, 0, 0 };
    const uint8_t *body = hid_report_body(&r, rep, sizeof(rep), &body_len, &id);
    CHECK(body == rep);              /* no prefix to strip */
    CHECK_EQ(body_len, 8);
    CHECK_EQ(id, 0);
}


/* A Report ID is shared by input, output and feature reports, but the lengths
 * are separate.  QEMU's usb-kbd is exactly this shape: 8 input bytes plus an
 * LED output report, both under ID 1, on a 9-byte interrupt packet.  If the
 * output bits were folded into the input length the driver would ask the
 * endpoint for 10 bytes and the short-packet accounting would be wrong. */
static void case_output_shares_id(void) {
    static const uint8_t d[] = {
        0x05, 0x01,        /* Usage Page (Generic) */
        0x09, 0x06,        /* Usage (Keyboard) */
        0xA1, 0x01,        /* Collection (Application) */
        0x85, 0x01,        /*   Report ID (1) */
        0x05, 0x07,        /*   Usage Page (Keyboard) */
        0x19, 0xE0, 0x29, 0xE7,   /* Usage Min/Max (modifiers) */
        0x15, 0x00, 0x25, 0x01,   /* Logical Min/Max */
        0x75, 0x01, 0x95, 0x08,   /* Report Size 1, Count 8 */
        0x81, 0x02,               /*   Input (Data,Var,Abs) */
        0x75, 0x08, 0x95, 0x01,   /* Report Size 8, Count 1 */
        0x81, 0x03,               /*   Input (Const) -- QEMU's reserved byte */
        0x75, 0x08, 0x95, 0x06,   /* Report Size 8, Count 6 */
        0x15, 0x00, 0x26, 0xFF, 0x00,
        0x19, 0x00, 0x2A, 0xFF, 0x00,
        0x81, 0x00,               /*   Input (Data,Array) */
        0x05, 0x08,               /*   Usage Page (LEDs) */
        0x19, 0x01, 0x29, 0x05,
        0x75, 0x01, 0x95, 0x05,   /* Report Size 1, Count 5 */
        0x91, 0x02,               /*   Output (Data,Var,Abs) */
        0x75, 0x03, 0x95, 0x01,   /* Report Size 3, Count 1 (pad) */
        0x91, 0x03,               /*   Output (Const) */
        0xC0,                     /* End Collection */
    };
    hid_report_t r;
    CHECK_EQ(hid_report_parse(&r, d, sizeof(d)), 0);
    CHECK_EQ(r.report_id_count, 1);
    CHECK_EQ(r.report_ids[0], 1);
    CHECK_EQ(r.report_bits_max, 64);          /* 8 mods + 48 key bits */
    CHECK_EQ(hid_report_wire_len(&r), 9);     /* 8 body bytes + the ID byte */
}

int main(void) {
    printf("=== AuraLite OS HID report-descriptor unit tests ===\n\n");
    RUN(test_boot_keyboard);
    RUN(test_two_report_ids);
    RUN(test_body_split);
    RUN(test_negative_unknown_report_id);
    RUN(test_negative_truncated);
    RUN(test_negative_nothing_interpretable);
    RUN(test_no_report_id_body_is_whole_report);
    RUN(case_output_shares_id);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
