/* hid_report.c — USB HID report-descriptor parser (RESIDUE3 C5b, RES-62).
 *
 * Moved out of hid.c and made report-ID-aware.  The parsing rules implemented
 * here are the ones the driver actually needs (HID 1.11 §6.2.2):
 *
 *   - short items only (long items, prefix 0xFE, are skipped);
 *   - global items: usage page, logical min/max, report size/count, report ID;
 *   - local items: usages, usage ranges (consumed at the next main item);
 *   - main items: Collection, End Collection, Input, Output, Feature.
 *
 * Report IDs are the reason this got its own file: on a Report ID item the bit
 * cursor is per-report, so each report starts its own layout, and every field
 * remembers which report it belongs to.
 */
#include "drivers/usb/hid_report.h"

#include <stdint.h>

#include "kernel/lib/string.h"

static int32_t item_sdata(const uint8_t *p, int size) {
    if (size == 0) return 0;
    if (size == 1) return (int8_t)p[0];
    if (size == 2) return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t item_udata(const uint8_t *p, int size) {
    if (size == 0) return 0;
    if (size == 1) return p[0];
    if (size == 2) return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t sign_extend(uint32_t v, uint8_t bits) {
    if (bits == 0 || bits >= 32) return (int32_t)v;
    uint32_t sign = 1u << (bits - 1);
    if (v & sign) v |= (~0u << bits);
    return (int32_t)v;
}

static uint32_t get_bits(const uint8_t *buf, uint16_t bit, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 32; i++) {
        uint16_t b = (uint16_t)(bit + i);
        if (buf[b >> 3] & (1u << (b & 7))) v |= (1u << i);
    }
    return v;
}

/* Index into the report_id[] arrays, creating the entry if it is new. */
static int report_slot(hid_report_t *r, uint8_t report_id) {
    for (uint8_t i = 0; i < r->report_id_count; i++)
        if (r->report_ids[i] == report_id) return i;
    if (r->report_id_count >= HID_REPORT_ID_MAX) return -1;
    int slot = r->report_id_count++;
    r->report_ids[slot] = report_id;
    r->report_bits[slot] = 0;
    return slot;
}

int hid_report_parse(hid_report_t *r, const uint8_t *d, uint16_t len) {
    if (!r || !d || len == 0) return -1;
    memset(r, 0, sizeof(*r));

    uint16_t usage_page = 0;
    int32_t logical_min = 0, logical_max = 1;
    uint8_t report_size = 0, report_count = 0;
    uint8_t cur_id = 0;            /* 0 = no Report ID item seen yet */
    /* -1 until a report is actually being filled: a descriptor that never
     * declares a Report ID gets its slot created at the end, so a two-ID
     * descriptor does not also carry a phantom id-0 entry. */
    int cur_slot = -1;
    uint16_t bitpos = 0;           /* inside the CURRENT report */
    uint16_t usages[32];
    uint8_t usage_count = 0;
    uint16_t usage_min = 0, usage_max = 0;
    int have_range = 0;
    int mouse_collection = 0;

    for (uint16_t off = 0; off < len;) {
        uint8_t b = d[off++];
        /* Long item: bTag(1) bType(1) bSize(1) then bSize data bytes. */
        if (b == 0xFE) {
            if (off + 2 > len) break;
            uint8_t n = d[off + 1];
            off = (uint16_t)(off + 2 + n);
            continue;
        }
        int sz = b & 3;
        if (sz == 3) sz = 4;
        int type = (b >> 2) & 3;
        int tag = (b >> 4) & 0xF;
        if (off + sz > len) break;          /* truncated item: stop cleanly */
        const uint8_t *payload = d + off;
        uint32_t uval = item_udata(payload, sz);
        int32_t sval = item_sdata(payload, sz);
        off = (uint16_t)(off + sz);

        if (type == 1) { /* global */
            switch (tag) {
            case 0x0: usage_page = (uint16_t)uval; break;
            case 0x1: logical_min = sval; break;
            case 0x2: logical_max = sval; break;
            case 0x7: report_size = (uint8_t)uval; break;
            case 0x9: report_count = (uint8_t)uval; break;
            case 0x8: { /* Report ID: a new report starts here */
                /* Persist the size of the report we are leaving. */
                if (cur_slot >= 0) r->report_bits[cur_slot] = bitpos;
                cur_id = (uint8_t)uval;
                r->has_report_id = 1;
                cur_slot = report_slot(r, cur_id);
                bitpos = (cur_slot >= 0) ? r->report_bits[cur_slot] : 0;
                break;
            }
            default: break;
            }
        } else if (type == 2) { /* local */
            switch (tag) {
            case 0x0:
                if (usage_count < 32) usages[usage_count++] = (uint16_t)uval;
                if (usage_page == HID_USAGE_PAGE_GENERIC &&
                    (uval == HID_USAGE_MOUSE || uval == HID_USAGE_POINTER))
                    r->is_mouse = 1;
                if (usage_page == HID_USAGE_PAGE_GENERIC && uval == HID_USAGE_KEYBOARD)
                    r->is_keyboard = 1;
                break;
            case 0x1: usage_min = (uint16_t)uval; have_range = 1; break;
            case 0x2: usage_max = (uint16_t)uval; have_range = 1; break;
            default: break;
            }
        } else if (type == 0) { /* main */
            if (tag == 0xA) { /* Collection */
                if (usage_page == HID_USAGE_PAGE_GENERIC && usage_count &&
                    (usages[usage_count - 1] == HID_USAGE_MOUSE ||
                     usages[usage_count - 1] == HID_USAGE_POINTER))
                    mouse_collection++;
                usage_count = 0; have_range = 0;
            } else if (tag == 0xC) { /* End Collection */
                if (mouse_collection > 0) mouse_collection--;
                usage_count = 0; have_range = 0;
            } else if (tag == 0x8) { /* Input */
                uint8_t flags = (uint8_t)uval;
                for (uint8_t i = 0; i < report_count; i++) {
                    uint16_t usage = 0;
                    if (i < usage_count) usage = usages[i];
                    else if (have_range && usage_min + i <= usage_max) usage = (uint16_t)(usage_min + i);
                    if (!(flags & 0x01) && r->field_count < HID_REPORT_FIELD_MAX) {
                        hid_report_field_t *f = &r->fields[r->field_count++];
                        f->usage_page = usage_page;
                        f->usage = usage;
                        f->usage_min = have_range ? usage_min : usage;
                        f->usage_max = have_range ? usage_max : usage;
                        f->bit_offset = bitpos;
                        f->size = report_size;
                        f->logical_min = logical_min;
                        f->logical_max = logical_max;
                        f->flags = flags;
                        f->report_id = cur_id;
                        if (usage_page == HID_USAGE_PAGE_GENERIC &&
                            (usage == HID_USAGE_X || usage == HID_USAGE_Y ||
                             usage == HID_USAGE_WHEEL))
                            r->is_mouse = 1;
                        if (usage_page == HID_USAGE_PAGE_BUTTON) r->is_mouse = 1;
                        if (usage_page == HID_USAGE_PAGE_KEYBOARD) r->is_keyboard = 1;
                    }
                    /* Padding (constant) fields advance the cursor too. */
                    bitpos = (uint16_t)(bitpos + report_size);
                }
                usage_count = 0; have_range = 0;
            } else if (tag == 0x9 || tag == 0xB) { /* Output / Feature */
                /* Do NOT advance the input cursor.  A Report ID is shared by
                 * the input, output and feature reports of that ID (HID 1.11
                 * §6.2.2.7), but each has its own length: QEMU's keyboard has
                 * 8 input bytes and an 8-bit LED output report, both under ID
                 * 1.  Folding the LED bits in made the wire length 10 bytes
                 * for a 9-byte endpoint.  Only input bits count here. */
                usage_count = 0; have_range = 0;
            }
        }
    }
    {
        int slot = (cur_slot >= 0) ? cur_slot : report_slot(r, 0);
        if (slot >= 0) r->report_bits[slot] = bitpos;
    }
    for (uint8_t i = 0; i < r->report_id_count; i++)
        if (r->report_bits[i] > r->report_bits_max) r->report_bits_max = r->report_bits[i];
    return ((r->is_mouse || r->is_keyboard) && r->field_count > 0) ? 0 : -1;
}

int hid_report_has_id(const hid_report_t *r, uint8_t report_id) {
    if (!r) return 0;
    for (uint8_t i = 0; i < r->report_id_count; i++)
        if (r->report_ids[i] == report_id) return 1;
    return 0;
}

uint16_t hid_report_bits(const hid_report_t *r, uint8_t report_id) {
    if (!r) return 0;
    for (uint8_t i = 0; i < r->report_id_count; i++)
        if (r->report_ids[i] == report_id) return r->report_bits[i];
    return 0;
}

uint16_t hid_report_wire_len(const hid_report_t *r) {
    if (!r) return 0;
    uint16_t body = (uint16_t)((r->report_bits_max + 7) / 8);
    return (uint16_t)(body + (r->has_report_id ? 1 : 0));
}

const uint8_t *hid_report_body(const hid_report_t *r, const uint8_t *report,
                               uint32_t len, uint32_t *body_len,
                               uint8_t *id_out) {
    if (body_len) *body_len = 0;
    if (id_out) *id_out = 0;
    if (!r || !report || len == 0) return 0;
    if (!r->has_report_id) {
        if (body_len) *body_len = len;
        return report;
    }
    if (len < 2) return 0;                 /* an ID with no body */
    if (!hid_report_has_id(r, report[0])) return 0;
    if (id_out) *id_out = report[0];
    if (body_len) *body_len = len - 1;
    return report + 1;
}

int32_t hid_report_field_value(const uint8_t *body, const hid_report_field_t *f) {
    if (!body || !f) return 0;
    uint32_t raw = get_bits(body, f->bit_offset, f->size);
    if (f->logical_min < 0) return sign_extend(raw, f->size);
    return (int32_t)raw;
}
