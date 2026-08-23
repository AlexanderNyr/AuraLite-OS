/* fwcfg_a64.c — QEMU fw-cfg probe, aarch64 MMIO edition (RESIDUE_PLAN
 * R11, RES-34: AMEND-5's deferral ends).
 *
 * The x86 protocol transfers, the port-I/O reader does not — exactly
 * what AMEND-5 recorded.  On the virt board fw-cfg is a DTB node
 * (compatible "qemu,fw-cfg-mmio", base 0x09020000, inside the
 * Device-nGnRE MMIO plateau): the 16-bit selector lives at +8 and is
 * written BIG-ENDIAN (per QEMU docs/specs/fw_cfg.txt, MMIO flavour),
 * the data register at +0 is read byte-wise sequentially.  The file
 * directory walk and the opt/auralite.selftest knob are byte-for-byte
 * the x86 logic; the state it feeds (kernel/lib/selftest.c) is the
 * same shared file, now a KERNELA64_SHARED row.
 *
 * No fw-cfg node in the DTB (real hardware) → the probe never runs
 * and the build default stands, same honesty as the x86 open-bus
 * gate.
 */

#include <stdint.h>
#include "kernel/arch/aarch64/fwcfg_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/lib/selftest.h"

#define FW_CFG_SIGNATURE  0x0000
#define FW_CFG_FILE_DIR   0x0019

#define SELFTEST_FILE     "opt/auralite.selftest"

static volatile uint8_t  *fw_data;   /* base + 0 */
static volatile uint16_t *fw_sel;    /* base + 8, big-endian writes */

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static void fw_select(uint16_t key) {
    *fw_sel = bswap16(key);
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint8_t rd8(void) {
    return *fw_data;
}

static uint32_t read_be32(void) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | rd8();
    }
    return v;
}

static uint16_t read_be16(void) {
    uint16_t hi = rd8();
    uint16_t lo = rd8();
    return (uint16_t)((hi << 8) | lo);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void fwcfg_a64_selftest_probe(uint64_t base_phys) {
    if (!base_phys) {
        return;                            /* no DTB node: default stands */
    }
    fw_data = (volatile uint8_t *)p2v_a64(base_phys);
    fw_sel  = (volatile uint16_t *)p2v_a64(base_phys + 8);

    fw_select(FW_CFG_SIGNATURE);
    char sig[4];
    for (int i = 0; i < 4; i++) {
        sig[i] = (char)rd8();
    }
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return;                            /* not fw-cfg: default stands */
    }

    fw_select(FW_CFG_FILE_DIR);
    uint32_t count = read_be32();
    if (count > 256) {
        return;                            /* implausible: treat as absent */
    }

    uint32_t knob_size   = 0;
    uint16_t knob_select = 0;
    int      found       = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size   = read_be32();
        uint16_t select = read_be16();
        (void)read_be16();                 /* reserved */
        char name[57];
        for (int k = 0; k < 56; k++) {
            name[k] = (char)rd8();
        }
        name[56] = '\0';
        if (!found && str_eq(name, SELFTEST_FILE)) {
            knob_size   = size;
            knob_select = select;
            found       = 1;
        }
    }
    if (!found || knob_size == 0 || knob_size > 15) {
        return;
    }

    fw_select(knob_select);
    char val[16];
    uint32_t n;
    for (n = 0; n < knob_size; n++) {
        val[n] = (char)rd8();
    }
    val[n] = '\0';

    if (str_eq(val, "full")) {
        selftest_set_mode(SELFTEST_FULL, "fw-cfg");
    } else if (str_eq(val, "fast")) {
        selftest_set_mode(SELFTEST_FAST, "fw-cfg");
    } else if (str_eq(val, "off")) {
        selftest_set_mode(SELFTEST_OFF, "fw-cfg");
    }
    /* Unrecognised strings leave the default in place. */
}
