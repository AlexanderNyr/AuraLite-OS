/*
 * test_usb_ehci_qh.c — RESIDUE3 C5b: the EHCI queue-head capability words,
 * including the split-transaction fields.
 *
 * drivers/usb/ehci_qh.c is compiled UNMODIFIED; this file supplies endpoint
 * parameters and decodes the two 32-bit words back into named fields, so a
 * failure names the field that moved rather than a hex difference.
 *
 * WHY THIS EXISTS.  C5b's plan asked for an integration case with a FS/LS
 * device behind a high-speed hub on EHCI, so the TT fields would be shown to
 * schedule a real periodic transfer.  That case cannot be built on QEMU: its
 * only hub, `usb-hub`, is a full-speed device and QEMU refuses it on a
 * high-speed bus ("speed mismatch trying to attach usb device \"QEMU USB
 * Hub\" (full speed) to bus \"e.0\", port \"1\" (high speed)").  The register
 * encoding is therefore verified here, bit position by bit position, against
 * EHCI 1.0 §3.6 and §4.12 — and the split decision is logged by ehci.c at
 * runtime, so a machine that does have a TT shows it in the boot log.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/usb/ehci_qh.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a,e) do { long _a=(long)(a), _e=(long)(e); if (_a!=_e) { \
    printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

/* --- field extraction, spelled the way EHCI 1.0 §3.6 labels them --------- */
static uint32_t f_devaddr (uint32_t c1) { return  c1        & 0x7Fu; }
static uint32_t f_inact   (uint32_t c1) { return (c1 >> 7)  & 0x1u;  }
static uint32_t f_epnum   (uint32_t c1) { return (c1 >> 8)  & 0xFu;  }
static uint32_t f_eps     (uint32_t c1) { return (c1 >> 12) & 0x3u;  }
static uint32_t f_dtc     (uint32_t c1) { return (c1 >> 14) & 0x1u;  }
static uint32_t f_mpl     (uint32_t c1) { return (c1 >> 16) & 0x7FFu; }
static uint32_t f_smask   (uint32_t c2) { return  c2        & 0xFFu; }
static uint32_t f_cmask   (uint32_t c2) { return (c2 >> 8)  & 0xFFu; }
static uint32_t f_hubaddr (uint32_t c2) { return (c2 >> 16) & 0x7Fu; }
static uint32_t f_hubport (uint32_t c2) { return (c2 >> 23) & 0x7Fu; }
static uint32_t f_mult    (uint32_t c2) { return (c2 >> 30) & 0x3u;  }

static ehci_qh_params_t base_params(void) {
    ehci_qh_params_t p;
    memset(&p, 0, sizeof(p));
    p.dev_addr = 1;
    p.ep_num = 1;                 /* 0x81 -> endpoint number 1 */
    p.speed = EHCI_QH_SPEED_HIGH;
    p.max_packet = 8;
    p.mult = 1;
    p.start_microframe = 0;
    return p;
}

/* ---------------------------------------------------------------- cases --- */

/* A high-speed device on a root port: no TT, no split, S-mask still set
 * because the QH is periodic. */
static void test_high_speed_no_split(void) {
    ehci_qh_params_t p = base_params();
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);

    CHECK_EQ(c.is_split, 0);
    CHECK_EQ(f_eps(c.ep_caps1), EHCI_QH_SPEED_HIGH);
    CHECK_EQ(f_devaddr(c.ep_caps1), 1);
    CHECK_EQ(f_epnum(c.ep_caps1), 1);
    CHECK_EQ(f_mpl(c.ep_caps1), 8);
    CHECK_EQ(f_dtc(c.ep_caps1), 1);      /* data toggle comes from the qTD */
    CHECK_EQ(f_inact(c.ep_caps1), 0);
    CHECK_EQ(f_smask(c.ep_caps2), 0x01);
    CHECK_EQ(f_cmask(c.ep_caps2), 0x00); /* no complete-splits without a TT */
    CHECK_EQ(f_hubaddr(c.ep_caps2), 0);
    CHECK_EQ(f_hubport(c.ep_caps2), 0);
    CHECK_EQ(f_mult(c.ep_caps2), 1);
}

/* QEMU's actual case (C5b measurement): a full-speed keyboard attached
 * straight to the EHCI controller.  Full speed, hub address 0 -- and
 * therefore NOT a split, whatever the speed says. */
static void test_full_speed_direct_has_no_tt(void) {
    ehci_qh_params_t p = base_params();
    p.speed = EHCI_QH_SPEED_FULL;
    p.max_packet = 8;
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);

    CHECK_EQ(c.is_split, 0);
    CHECK_EQ(f_eps(c.ep_caps1), EHCI_QH_SPEED_FULL);
    CHECK_EQ(f_cmask(c.ep_caps2), 0);
    CHECK_EQ(f_hubaddr(c.ep_caps2), 0);
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_FULL, 0), 0);
}

/* The split case the plan wanted: full speed behind a TT at hub address 2,
 * port 3.  This is where the TT fields have to be exactly right. */
static void test_full_speed_behind_tt(void) {
    ehci_qh_params_t p = base_params();
    p.speed = EHCI_QH_SPEED_FULL;
    p.hub_addr = 2;
    p.hub_port = 3;
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);

    CHECK_EQ(c.is_split, 1);
    CHECK_EQ(f_eps(c.ep_caps1), EHCI_QH_SPEED_FULL);
    CHECK_EQ(f_hubaddr(c.ep_caps2), 2);
    CHECK_EQ(f_hubport(c.ep_caps2), 3);
    /* One start-split, in microframe 0. */
    CHECK_EQ(f_smask(c.ep_caps2), 0x01);
    CHECK_EQ(c.s_mask & (uint8_t)(c.s_mask - 1), 0);   /* exactly one bit */
    /* Complete-splits follow the start-split, and never overlap it. */
    CHECK_EQ(f_cmask(c.ep_caps2) & f_smask(c.ep_caps2), 0);
    CHECK(f_cmask(c.ep_caps2) != 0);
    /* Microframe 0 is the last start, so the three flags in 1..3. */
    CHECK_EQ(f_cmask(c.ep_caps2), 0x0E);
}

/* Low speed behind the same TT: same split fields, different EPS. */
static void test_low_speed_behind_tt(void) {
    ehci_qh_params_t p = base_params();
    p.speed = EHCI_QH_SPEED_LOW;
    p.hub_addr = 5;
    p.hub_port = 1;
    p.max_packet = 4;
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);

    CHECK_EQ(c.is_split, 1);
    CHECK_EQ(f_eps(c.ep_caps1), EHCI_QH_SPEED_LOW);
    CHECK_EQ(f_hubaddr(c.ep_caps2), 5);
    CHECK_EQ(f_hubport(c.ep_caps2), 1);
    CHECK_EQ(f_mpl(c.ep_caps1), 4);
}

/* A non-zero start microframe must move both masks and keep them disjoint,
 * for every one of the eight possible phases. */
static void test_start_microframe_rotates_masks(void) {
    for (uint8_t mf = 0; mf < 8; mf++) {
        ehci_qh_params_t p = base_params();
        p.speed = EHCI_QH_SPEED_FULL;
        p.hub_addr = 2;
        p.hub_port = 1;
        p.start_microframe = mf;
        ehci_qh_caps_t c;
        ehci_qh_build_caps(&p, &c);

        CHECK_EQ(f_smask(c.ep_caps2), 1u << mf);
        CHECK_EQ(f_cmask(c.ep_caps2) & f_smask(c.ep_caps2), 0);
        /* Three complete-split micro-frames, starting right after the start
         * and wrapping past bit 7 (a late start must not leave the mask
         * empty -- the controller would never see a complete-split). */
        CHECK(f_cmask(c.ep_caps2) != 0);
        {
            uint8_t cm = (uint8_t)f_cmask(c.ep_caps2);
            int bits = 0;
            for (int b = 0; b < 8; b++) bits += (cm >> b) & 1;
            CHECK_EQ(bits, 3);
        }
        CHECK_EQ(f_cmask(c.ep_caps2) & ~0xFFu, 0);
        CHECK_EQ(f_hubaddr(c.ep_caps2), 2);
    }
}

/* Field widths: address and port are 7 bits each, endpoints 4, MPL 11, Mult
 * 2.  Values wider than a field must be truncated at the field, not bleed
 * into the neighbouring one. */
static void test_field_widths(void) {
    ehci_qh_params_t p = base_params();
    p.dev_addr = 0xFF;          /* 7 bits  -> 0x7F */
    p.ep_num = 0xFF;            /* 4 bits  -> 0x0F */
    p.max_packet = 0xFFFF;      /* 11 bits -> 0x7FF */
    p.speed = EHCI_QH_SPEED_FULL;
    p.hub_addr = 0xFF;          /* 7 bits  -> 0x7F */
    p.hub_port = 0xFF;          /* 7 bits  -> 0x7F */
    p.mult = 0xFF;              /* 2 bits  -> 0x3  */
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);

    CHECK_EQ(f_devaddr(c.ep_caps1), 0x7F);
    CHECK_EQ(f_epnum(c.ep_caps1), 0x0F);
    CHECK_EQ(f_mpl(c.ep_caps1), 0x7FF);
    CHECK_EQ(f_eps(c.ep_caps1), EHCI_QH_SPEED_FULL);
    CHECK_EQ(f_hubaddr(c.ep_caps2), 0x7F);
    CHECK_EQ(f_hubport(c.ep_caps2), 0x7F);
    CHECK_EQ(f_mult(c.ep_caps2), 0x3);
    /* The two words must not collide: caps1 stops at bit 26, eps at 13. */
    CHECK_EQ(c.ep_caps1 & ~((1u << 27) - 1), 0);
}

/* Mult = 0 means "one transaction per microframe" in this driver; it must not
 * be encoded as the reserved value 0. */
static void test_mult_zero_becomes_one(void) {
    ehci_qh_params_t p = base_params();
    p.mult = 0;
    ehci_qh_caps_t c;
    ehci_qh_build_caps(&p, &c);
    CHECK_EQ(f_mult(c.ep_caps2), 1);
}

/* The split decision as a standalone predicate, including the combination
 * that would be a bug: a high-speed device reached "through a hub" needs no
 * split (hubs are transparent to high-speed traffic). */
static void test_needs_split_predicate(void) {
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_FULL, 0), 0);
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_LOW, 0), 0);
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_FULL, 1), 1);
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_LOW, 1), 1);
    CHECK_EQ(ehci_qh_needs_split(EHCI_QH_SPEED_HIGH, 3), 0);
}

/* NULL tolerance: called from an error path, must not fault. */
static void test_rejects_null(void) {
    ehci_qh_caps_t c;
    memset(&c, 0xAA, sizeof(c));
    ehci_qh_build_caps(NULL, &c);
    CHECK_EQ(c.ep_caps1, 0xAAAAAAAAu);
    ehci_qh_params_t p = base_params();
    ehci_qh_build_caps(&p, NULL);
}

int main(void) {
    printf("=== AuraLite OS EHCI queue-head capability tests ===\n");
    RUN(test_high_speed_no_split);
    RUN(test_full_speed_direct_has_no_tt);
    RUN(test_full_speed_behind_tt);
    RUN(test_low_speed_behind_tt);
    RUN(test_start_microframe_rotates_masks);
    RUN(test_field_widths);
    RUN(test_mult_zero_becomes_one);
    RUN(test_needs_split_predicate);
    RUN(test_rejects_null);
    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
