/* fwcfg.c — QEMU fw_cfg probe for the self-test knob (OPT_PLAN.md O2).
 *
 * Reads -fw_cfg name=opt/auralite.selftest,string=full|fast|off and, if
 * present, overrides the build-default self-test mode before any of the
 * scaled self-tests run.  This is how the integration lib pins every CI
 * boot to `full` (tests/integration/lib/lib.sh) while `make run` and
 * real hardware get the build default.
 *
 * Interface (QEMU docs/specs/fw_cfg.txt, x86 flavour): write a 16-bit
 * selector to port 0x510, then read the item's bytes one at a time from
 * port 0x511.  Selector 0x0000 must yield the "QEMU" signature; selector
 * 0x0019 (FILE_DIR) yields a big-endian u32 count followed by 64-byte
 * entries { be32 size; be16 select; u16 reserved; char name[56]; }.
 * Everything multi-byte in the directory is BIG-endian — composed here
 * byte by byte, which sidesteps both the endianness and the width-cast
 * ratchet in one move.
 *
 * On real hardware (no fw_cfg) the signature read returns open-bus
 * garbage, the probe returns quietly, and the build default stands.
 * This file lives in kernel/arch/x86_64/ because port I/O is an x86
 * instruction class and the I6 ratchet holds portable files at their
 * current count of it.
 */
#include <stdint.h>
#include "kernel/arch/x86_64/portio.h"
#include "kernel/lib/selftest.h"
#include "kernel/lib/string.h"

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

void fwcfg_selftest_probe(void) {
    /* Signature: absent fw_cfg reads as open bus, not "QEMU". */
    outw(FW_CFG_SELECTOR, FW_CFG_SIGNATURE);
    char sig[4];
    for (int i = 0; i < 4; i++) {
        sig[i] = (char)inb(FW_CFG_DATA);
    }
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return;                            /* no fw_cfg: default stands */
    }

    /* Walk the file directory for our knob. */
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
        if (!found && strcmp(name, SELFTEST_FILE) == 0) {
            knob_size   = size;
            knob_select = select;
            found       = 1;
            /* Keep reading: the data port is sequential, and bailing
             * mid-directory costs nothing anyway once we have the key. */
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

    if (strcmp(val, "full") == 0) {
        selftest_set_mode(SELFTEST_FULL, "fw_cfg");
    } else if (strcmp(val, "fast") == 0) {
        selftest_set_mode(SELFTEST_FAST, "fw_cfg");
    } else if (strcmp(val, "off") == 0) {
        selftest_set_mode(SELFTEST_OFF, "fw_cfg");
    }
    /* Unrecognised strings leave the default in place: a typo in a QEMU
     * flag should degrade to normal behaviour, not to silence. */
}
