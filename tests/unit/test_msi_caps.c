/*
 * test_msi_caps.c — RESIDUE3 C4: the MSI/MSI-X capability walker, driven
 * against a synthetic device.
 *
 * kernel/drivers/pci/msi_caps.c is compiled UNMODIFIED; the only thing this
 * file supplies is a fake config space (256 bytes) and a fake MSI-X table
 * page, reached through the same accessor table the real transports use.
 *
 * The cases are the ones a QEMU-only walker never sees:
 *   - a device with MSI-X only, one with MSI only, one with both (MSI-X wins)
 *     and one with none of them;
 *   - the programming path for each kind, byte for byte (address, data,
 *     mask, enable) — including that the enable is VERIFIED by read-back;
 *   - two negative controls: a device that silently drops the enable bit
 *     must be reported as refusing, and a self-referential capability list
 *     must terminate instead of hanging.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/pci/msi_caps.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a,e) do { long _a=(long)(a), _e=(long)(e); if (_a!=_e) { \
    printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- the fake device ---- */
typedef struct {
    uint8_t  cfg[256];
    uint8_t  table[2048];   /* the fake BAR: offset 0x100 is entry 0 */    /* the MSI-X table BAR: offset 0x100 = entry 0 */
    int      drop_enable;    /* negative control: writes do not stick */
    int      bar_map_calls;
    uint32_t bar_last_len;
} fake_dev_t;

static fake_dev_t dev;

static uint8_t  f_cfg8 (void *c, uint16_t o) { fake_dev_t *d = c; return d->cfg[o & 0xFF]; }
static uint16_t f_cfg16(void *c, uint16_t o) { fake_dev_t *d = c; uint16_t v = 0; \
    memcpy(&v, &d->cfg[o & 0xFF], 2); return v; }
/* 4-byte alignment (0xFC), not 8: the parked draft masked with 0xF8, so the
 * MSI-X table dword at cap+4 (0x44) was read from 0x40 and the walker got the
 * capability id back as a BAR offset -- a wild pointer and an ASan SEGV that
 * looked like a driver bug and was a test-fixture bug. */
static uint32_t f_cfg32(void *c, uint16_t o) { fake_dev_t *d = c; uint32_t v = 0; \
    memcpy(&v, &d->cfg[o & 0xFC], 4); return v; }
static void f_w16(void *c, uint16_t o, uint16_t v) {
    fake_dev_t *d = c;
    if (d->drop_enable) return;              /* negative control: swallowed */
    memcpy(&d->cfg[o & 0xFF], &v, 2);
}
static void f_w32(void *c, uint16_t o, uint32_t v) {
    fake_dev_t *d = c;
    memcpy(&d->cfg[o & 0xFC], &v, 4);
}
/* the table entry 0 .. 3 dwords, as the walker sees them */
static uint32_t *tbl_words(void) { return (uint32_t *)(void *)(dev.table + 0x100); }
static uint32_t tbl_get(int i) { return tbl_words()[i]; }

static volatile uint8_t *f_bar(void *c, uint8_t bar, uint32_t off, uint32_t len) {
    fake_dev_t *d = c;
    d->bar_map_calls++;
    d->bar_last_len = len;
    (void)bar;
    return (volatile uint8_t *)((uint8_t *)d->table + off);
}

static msi_pci_io_t io;

static void fake_reset(void) {
    memset(&dev, 0, sizeof(dev));
    io.ctx = &dev; io.cfg8 = f_cfg8; io.cfg16 = f_cfg16; io.cfg32 = f_cfg32;
    io.w16 = f_w16; io.w32 = f_w32; io.bar_map = f_bar;
    /* status: capability list present */
    dev.cfg[0x06] = 0x10;
}

/* Lay out a capability list: [0x40]=MSI-X?, [0x50]=MSI?, chained. */
static void build_caps(int want_msix, int want_msi, uint16_t msix_nvectors) {
    fake_reset();
    uint8_t first = 0, prev = 0;
    if (want_msix) {
        dev.cfg[0x40] = PCI_CAP_ID_MSIX;
        dev.cfg[0x41] = 0;
        uint16_t mc = (uint16_t)((msix_nvectors - 1u) & 0x7FFu);
        memcpy(&dev.cfg[0x42], &mc, 2);
        uint32_t tbl = (2u /* BIR 2 */) | (0x100u /* offset */);
        memcpy(&dev.cfg[0x44], &tbl, 4);
        first = 0x40; prev = 0x40;
    }
    if (want_msi) {
        uint8_t at = 0x50;
        dev.cfg[at] = PCI_CAP_ID_MSI;
        dev.cfg[at + 1] = 0;
        /* 64-bit capable, per-vector masking, 4 vectors capable */
        uint16_t mc = (uint16_t)(0x0008u | 0x0010u | (2u << 11));
        memcpy(&dev.cfg[at + 2], &mc, 2);
        if (!first) { first = at; } else { dev.cfg[prev + 1] = at; }
        prev = at;
    }
    if (first) dev.cfg[0x34] = first;
}

/* ---- cases ---- */

static void test_scan_msix(void) {
    msi_caps_t caps;
    build_caps(1, 0, 4);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    CHECK_EQ(caps.n_vectors, 4);
    CHECK_EQ(caps.bir, 2);
    CHECK_EQ(caps.table_off, 0x100);
    CHECK_EQ(caps.cap_off, 0x40);
}

static void test_scan_msi_only(void) {
    msi_caps_t caps;
    build_caps(0, 1, 0);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSI);
    CHECK_EQ(caps.n_vectors, 4);        /* 1 << MMC */
    CHECK_EQ(caps.cap64, 1);
}

static void test_msix_wins_when_both(void) {
    msi_caps_t caps;
    build_caps(1, 1, 2);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    CHECK_EQ(caps.cap_off, 0x40);
}

static void test_scan_none(void) {
    msi_caps_t caps;
    fake_reset();
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_NONE);
    /* and no capability list at all */
    dev.cfg[0x06] = 0;
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_NONE);
}

static void test_program_msix(void) {
    msi_caps_t caps;
    char why[96] = "";
    build_caps(1, 0, 4);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    CHECK_EQ(msi_program(&io, &caps, 96, 0, why, sizeof(why)), 0);
    /* entry 0: addr, addr-hi, data, control */
    CHECK_EQ(tbl_get(0), MSI_APIC_ADDR_BASE | 0u);
    CHECK_EQ(tbl_get(1), 0);
    CHECK_EQ(tbl_get(2), 96);
    CHECK_EQ(tbl_get(3), 0);                       /* unmasked */
    /* the enable bit went in AND came back */
    uint16_t mc; memcpy(&mc, &dev.cfg[0x42], 2);
    CHECK(mc & 0x8000u);
    CHECK_EQ(mc & 0x4000u, 0);
    /* the table BAR was asked for once, with a 16-byte-entry length */
    CHECK_EQ(dev.bar_map_calls, 1);
    CHECK_EQ(dev.bar_last_len, 4u * 16u);
}

static void test_program_msix_destination(void) {
    msi_caps_t caps;
    build_caps(1, 0, 1);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    CHECK_EQ(msi_program(&io, &caps, 0x61, 3, NULL, 0), 0);
    CHECK_EQ(tbl_get(0), MSI_APIC_ADDR_BASE | (3u << 12));
    CHECK_EQ(tbl_get(2), 0x61);
}

static void test_program_msi(void) {
    msi_caps_t caps;
    char why[96] = "";
    build_caps(0, 1, 0);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSI);
    CHECK_EQ(msi_program(&io, &caps, 0x62, 1, why, sizeof(why)), 0);
    uint32_t addr; memcpy(&addr, &dev.cfg[0x54], 4);
    CHECK_EQ(addr, MSI_APIC_ADDR_BASE | (1u << 12));
    uint32_t hi; memcpy(&hi, &dev.cfg[0x58], 4);
    CHECK_EQ(hi, 0);                                 /* 64-bit: high = 0 */
    uint16_t data; memcpy(&data, &dev.cfg[0x5C], 2);
    CHECK_EQ(data, 0x62);
    uint16_t mc; memcpy(&mc, &dev.cfg[0x52], 2);
    CHECK(mc & 0x0001u);                             /* enabled */
    CHECK_EQ(mc & 0x0700u, 0);                       /* one message */
    uint32_t mask; memcpy(&mask, &dev.cfg[0x60], 4);
    CHECK_EQ(mask, 0);                               /* unmasked */
}

static void test_negative_control_dropped_enable(void) {
    /* A device that quietly refuses: every config write is swallowed, so the
     * enable bit never sets.  The walker must NOT report success -- reading
     * the enable back is the whole reason this is a separate step. */
    msi_caps_t caps;
    char why[96] = "";
    build_caps(1, 0, 1);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    dev.drop_enable = 1;
    CHECK_EQ(msi_program(&io, &caps, 0x60, 0, why, sizeof(why)), -2);
    CHECK(why[0] != '\0');
    dev.drop_enable = 0;
    /* and the same device, once it accepts the write, succeeds: the failure
     * above was the device's, not the walker's */
    CHECK_EQ(msi_program(&io, &caps, 0x60, 0, why, sizeof(why)), 0);
}

static void test_negative_control_msi_dropped_enable(void) {
    msi_caps_t caps;
    char why[96] = "";
    build_caps(0, 1, 0);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSI);
    dev.drop_enable = 1;
    CHECK_EQ(msi_program(&io, &caps, 0x61, 0, why, sizeof(why)), -2);
    dev.drop_enable = 0;
}

static void test_transport_without_writer(void) {
    /* A read-only transport cannot program anything and must say so rather
     * than pretend: this is the shape a page-table-less early boot has. */
    msi_caps_t caps;
    char why[96] = "";
    build_caps(1, 0, 1);
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_MSIX);
    msi_pci_io_t ro = io;
    ro.w16 = NULL;
    CHECK_EQ(msi_program(&ro, &caps, 0x60, 0, why, sizeof(why)), -3);
    CHECK(why[0] != '\0');
}

static void test_malformed_list_terminates(void) {
    msi_caps_t caps;
    fake_reset();
    dev.cfg[0x40] = 0xEE;      /* not MSI, not MSI-X */
    dev.cfg[0x41] = 0x40;      /* points at itself  */
    dev.cfg[0x34] = 0x40;
    /* Must return, not spin: the fuse is 48 hops. */
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_NONE);
}

static void test_program_without_caps(void) {
    msi_caps_t caps;
    char why[64] = "";
    fake_reset();
    CHECK_EQ(msi_scan(&io, &caps), MSI_KIND_NONE);
    CHECK_EQ(msi_program(&io, &caps, 96, 0, why, sizeof(why)), -1);
    CHECK(why[0] != '\0');
}

int main(void) {
    printf("=== AuraLite OS MSI/MSI-X capability unit tests ===\n\n");
    RUN(test_scan_msix);
    RUN(test_scan_msi_only);
    RUN(test_msix_wins_when_both);
    RUN(test_scan_none);
    RUN(test_program_msix);
    RUN(test_program_msix_destination);
    RUN(test_program_msi);
    RUN(test_negative_control_dropped_enable);
    RUN(test_negative_control_msi_dropped_enable);
    RUN(test_transport_without_writer);
    RUN(test_malformed_list_terminates);
    RUN(test_program_without_caps);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
