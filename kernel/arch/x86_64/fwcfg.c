/* fwcfg.c — QEMU fw_cfg probes for the boot knobs (OPT_PLAN.md O2,
 * FSFULL_PLAN.md F1).
 *
 * Reads -fw_cfg name=opt/auralite.selftest,string=full|fast|off and
 * -fw_cfg name=opt/auralite.fsformat,string=0|1 and, when present,
 * overrides the build defaults (kernel/lib/selftest.c,
 * kernel/fs/fsformat.c) before any scaled self-test runs or any
 * experimental filesystem is mounted.  This is how the integration lib
 * pins every CI boot to `full` (tests/integration/lib/lib.sh) and how a
 * dev boot opts into auto-formatting, while `make run` and real hardware
 * get the build default.
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
 * garbage, the probes return quietly, and the build defaults stand.
 * This file lives in kernel/arch/x86_64/ because port I/O is an x86
 * instruction class and the I6 ratchet holds portable files at their
 * current count of it.
 */
#include <stdint.h>
#include "kernel/arch/x86_64/portio.h"
#include "kernel/lib/selftest.h"
#include "kernel/fs/fsformat.h"
#include "kernel/fs/fscheck.h"
#include "kernel/lib/string.h"

#define FW_CFG_SELECTOR   0x510
#define FW_CFG_DATA       0x511
#define FW_CFG_SIGNATURE  0x0000
#define FW_CFG_FILE_DIR   0x0019

#define SELFTEST_FILE     "opt/auralite.selftest"
#define FSFORMAT_FILE     "opt/auralite.fsformat"
#define FSCHECK_FILE      "opt/auralite.fscheck"

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

/* Find `wanted` in the fw_cfg file directory and read its bytes as a
 * NUL-terminated string into `out` (out_max bytes incl. the NUL).
 * Returns 1 on success, 0 when fw_cfg is absent, the file is missing,
 * or the value is implausibly large — each of which must degrade to the
 * build defaults, never to noise. */
static int fwcfg_read_string(const char *wanted, char *out, int out_max) {
    /* Signature: absent fw_cfg reads as open bus, not "QEMU". */
    outw(FW_CFG_SELECTOR, FW_CFG_SIGNATURE);
    char sig[4];
    for (int i = 0; i < 4; i++) {
        sig[i] = (char)inb(FW_CFG_DATA);
    }
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
        return 0;                            /* no fw_cfg: defaults stand */
    }

    /* Walk the file directory for our knob. */
    outw(FW_CFG_SELECTOR, FW_CFG_FILE_DIR);
    uint32_t count = read_be32();
    if (count > 256) {
        return 0;                            /* implausible: treat as absent */
    }

    uint32_t knob_size   = 0;
    uint16_t knob_select = 0;
    int      found       = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size   = read_be32();
        uint16_t select = read_be16();
        (void)read_be16();                   /* reserved */
        char name[57];
        for (int k = 0; k < 56; k++) {
            name[k] = (char)inb(FW_CFG_DATA);
        }
        name[56] = '\0';
        if (!found && strcmp(name, wanted) == 0) {
            knob_size   = size;
            knob_select = select;
            found       = 1;
            /* Keep reading: the data port is sequential, and bailing
             * mid-directory costs nothing anyway once we have the key. */
        }
    }
    if (!found || knob_size == 0 || knob_size > (uint32_t)out_max) {
        return 0;
    }

    outw(FW_CFG_SELECTOR, knob_select);
    uint32_t n;
    for (n = 0; n < knob_size; n++) {
        out[n] = (char)inb(FW_CFG_DATA);
    }
    out[n] = '\0';
    return 1;
}

void fwcfg_selftest_probe(void) {
    char val[16];
    if (!fwcfg_read_string(SELFTEST_FILE, val, (int)sizeof(val) - 1)) {
        return;
    }

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

void fwcfg_fsformat_probe(void) {
    char val[16];
    if (!fwcfg_read_string(FSFORMAT_FILE, val, (int)sizeof(val) - 1)) {
        return;
    }

    if (val[0] == '1') {
        fs_format_set(1, "fw_cfg");
    } else if (val[0] == '0') {
        fs_format_set(0, "fw_cfg");
    }
    /* Anything else (including a multi-byte value) leaves the build
     * default in place: a typo in a QEMU flag must degrade to the SAFE
     * state, and the safe state is the default (refuse). */
}

/* RESIDUE2 T3: the read-only FAT32/ext2 consistency walkers are an
 * opt-in boot diagnostic (fsformat.c shape).  Default OFF: a normal
 * boot pays nothing; a CI lane or a suspicious operator passes
 * -fw_cfg name=opt/auralite.fscheck,string=1 and gets named findings. */
void fwcfg_fscheck_probe(void) {
    char val[16];
    if (!fwcfg_read_string(FSCHECK_FILE, val, (int)sizeof(val) - 1)) {
        return;
    }

    if (val[0] == '1') {
        fscheck_set(1, "fw_cfg");
    } else if (val[0] == '0') {
        fscheck_set(0, "fw_cfg");
    }
    /* Unrecognised strings leave the default (off) in place. */
}
