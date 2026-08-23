/* fwcfg32.c — QEMU fw_cfg probe, i386 edition (RESIDUE_PLAN R11,
 * RES-34: "fast-boot knob unwired on i386").
 *
 * The same port-I/O protocol as kernel/arch/x86_64/fwcfg.c — selector
 * at 0x510, sequential data at 0x511, "QEMU" signature gate, file-dir
 * walk for opt/auralite.selftest — kept as an ARCH file for the same
 * reason the x86_64 one is: the I6 ratchet holds portable files at
 * zero new port I/O, and it is right to.  The STATE it feeds
 * (kernel/lib/selftest.c) is the shared portable file, newly a
 * KERNEL32_SHARED row: one knob, one mode variable, three widths.
 *
 * On real hardware (no fw_cfg) the signature read returns open-bus
 * garbage, the gate fails, and the build default stands — identical
 * to the x86_64 behaviour.
 */

#include <stdint.h>
#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/fwcfg32.h"
#include "kernel/lib/selftest.h"

#define FW_CFG_SELECTOR   0x510
#define FW_CFG_DATA       0x511
#define FW_CFG_SIGNATURE  0x0000
#define FW_CFG_FILE_DIR   0x0019

#define SELFTEST_FILE     "opt/auralite.selftest"

static uint32_t read_be32(void) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | inb(FW_CFG_DATA);
    }
    return v;
}

static uint16_t read_be16(void) {
    uint16_t hi = inb(FW_CFG_DATA);
    uint16_t lo = inb(FW_CFG_DATA);
    return (uint16_t)((hi << 8) | lo);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void fwcfg32_selftest_probe(void) {
    outw(FW_CFG_SELECTOR, FW_CFG_SIGNATURE);
    char sig[4];
    for (int i = 0; i < 4; i++) {
        sig[i] = (char)inb(FW_CFG_DATA);
    }
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return;                            /* no fw_cfg: default stands */
    }

    outw(FW_CFG_SELECTOR, FW_CFG_FILE_DIR);
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
            name[k] = (char)inb(FW_CFG_DATA);
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

    outw(FW_CFG_SELECTOR, knob_select);
    char val[16];
    uint32_t n;
    for (n = 0; n < knob_size; n++) {
        val[n] = (char)inb(FW_CFG_DATA);
    }
    val[n] = '\0';

    if (str_eq(val, "full")) {
        selftest_set_mode(SELFTEST_FULL, "fw_cfg");
    } else if (str_eq(val, "fast")) {
        selftest_set_mode(SELFTEST_FAST, "fw_cfg");
    } else if (str_eq(val, "off")) {
        selftest_set_mode(SELFTEST_OFF, "fw_cfg");
    }
    /* Unrecognised strings leave the default in place. */
}
