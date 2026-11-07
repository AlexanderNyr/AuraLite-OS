/* hid_report.h — USB HID report-descriptor parser (RESIDUE3 C5b, RES-62).
 *
 * Why this is its own module: the parser is pure data-in/data-out, and it is
 * the part of HID that keeps being wrong.  Living inside hid.c it was only
 * ever exercised by whatever QEMU happened to attach, which is how it ended up
 * carrying ONE global `report_id` for a descriptor that may declare eight.
 * Here it is host-testable against fixtures (tests/unit/test_usb_hid_report.c)
 * and the driver is one more user of it.
 *
 * What "report ID" means: a HID descriptor may tag items with `Report ID`
 * (global item 0x85).  When it does, every input report starts with a byte
 * naming which report it is, and each report has its OWN bit layout starting
 * after that byte.  A parser that keeps one ID and one running bit offset
 * silently folds every report onto whichever ID was declared last -- so a
 * keyboard-with-consumer-controls device decodes as if all its fields were one
 * report.  Fields here carry their report ID, and offsets are relative to
 * their own report's body.
 */
#ifndef AURALITE_DRIVERS_USB_HID_REPORT_H
#define AURALITE_DRIVERS_USB_HID_REPORT_H

#include <stdint.h>

#define HID_REPORT_FIELD_MAX    32
#define HID_REPORT_ID_MAX       8      /* distinct Report IDs tracked */
#define HID_REPORT_DESC_MAX     256

/* Usage pages / usages this tree cares about (the rest are parsed and
 * reported, not interpreted). */
#define HID_USAGE_PAGE_GENERIC  0x01
#define HID_USAGE_PAGE_BUTTON   0x09
#define HID_USAGE_PAGE_KEYBOARD 0x07
#define HID_USAGE_MOUSE         0x02
#define HID_USAGE_POINTER       0x01
#define HID_USAGE_KEYBOARD      0x06
#define HID_USAGE_X             0x30
#define HID_USAGE_Y             0x31
#define HID_USAGE_WHEEL         0x38

typedef struct {
    uint16_t usage_page;
    uint16_t usage;
    uint16_t usage_min;
    uint16_t usage_max;
    /* Bit offset INSIDE the report this field belongs to, i.e. counted from
     * the byte after the report-ID prefix. */
    uint16_t bit_offset;
    uint8_t  size;
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  flags;         /* main-item flags: bit0 constant, bit1 variable,
                             * bit2 relative */
    uint8_t  report_id;     /* 0 when the descriptor declares no Report ID */
} hid_report_field_t;

typedef struct {
    int      is_mouse;
    int      is_keyboard;
    int      has_report_id;
    uint8_t  report_ids[HID_REPORT_ID_MAX];
    uint8_t  report_id_count;
    /* Body size in bits of each tracked report's INPUT report, parallel to
     * report_ids[].  The prefix byte is NOT included, and output/feature
     * reports are NOT folded in even when they share the same Report ID
     * (HID 1.11 §6.2.2.7: the ID is shared, the lengths are not). */
    uint16_t report_bits[HID_REPORT_ID_MAX];
    /* Largest body, in bits -- what a caller must be prepared to receive. */
    uint16_t report_bits_max;
    hid_report_field_t fields[HID_REPORT_FIELD_MAX];
    uint8_t  field_count;
} hid_report_t;

/* Parse a report descriptor.  Returns 0 when the descriptor yields at least
 * one field of a kind this driver interprets (keyboard or pointer), -1
 * otherwise -- same contract the in-driver parser had. */
int hid_report_parse(hid_report_t *r, const uint8_t *desc, uint16_t len);

/* Bytes a caller should ask the device for: the largest body plus the report-ID
 * prefix when the descriptor uses one. */
uint16_t hid_report_wire_len(const hid_report_t *r);

/* Split an incoming report into its body.  On success returns a pointer into
 * `report` past the ID prefix and writes the body length and the report ID.
 * Returns NULL when the report is empty or names an ID the descriptor never
 * declared. */
const uint8_t *hid_report_body(const hid_report_t *r, const uint8_t *report,
                               uint32_t len, uint32_t *body_len,
                               uint8_t *id_out);

/* Body size in bits of a report ID (0 when unknown). */
uint16_t hid_report_bits(const hid_report_t *r, uint8_t report_id);

/* Value of one field, read from the report BODY (no ID prefix). */
int32_t hid_report_field_value(const uint8_t *body, const hid_report_field_t *f);

/* 1 when the descriptor declares this report ID. */
int hid_report_has_id(const hid_report_t *r, uint8_t report_id);

#endif /* AURALITE_DRIVERS_USB_HID_REPORT_H */
