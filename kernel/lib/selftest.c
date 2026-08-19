/* selftest.c — boot self-test intensity knob (OPT_PLAN.md O2).
 *
 * Portable state only; the fw_cfg probe that can override the build
 * default lives in kernel/arch/x86_64/fwcfg.c (port I/O stays in the
 * arch tree — the I6/V6 ratchets hold portable files at zero new port
 * I/O and zero new assembly, and they are right to).
 */
#include "kernel/lib/selftest.h"

/* Build default: make SELFTEST=full|fast|off (Makefile maps it onto one
 * of these defines; fast when unset). */
#if defined(SELFTEST_DEFAULT_FULL)
#define SELFTEST_DEFAULT SELFTEST_FULL
#elif defined(SELFTEST_DEFAULT_OFF)
#define SELFTEST_DEFAULT SELFTEST_OFF
#else
#define SELFTEST_DEFAULT SELFTEST_FAST
#endif

static selftest_mode_t mode   = SELFTEST_DEFAULT;
static const char     *source = "build default";

selftest_mode_t selftest_mode(void) { return mode; }

void selftest_set_mode(selftest_mode_t m, const char *src) {
    mode   = m;
    source = src ? src : "?";
}

const char *selftest_mode_name(void) {
    switch (mode) {
    case SELFTEST_FULL: return "full";
    case SELFTEST_FAST: return "fast";
    default:            return "off";
    }
}

const char *selftest_mode_source(void) { return source; }

uint64_t selftest_scale(uint64_t full_value, uint64_t fast_value) {
    switch (mode) {
    case SELFTEST_FULL: return full_value;
    case SELFTEST_FAST: return fast_value;
    default:            return 0;
    }
}
