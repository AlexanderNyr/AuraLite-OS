/* selftest.h — boot self-test intensity knob (OPT_PLAN.md O2).
 *
 * Three modes, one question: how much of the boot should be spent
 * re-proving invariants that CI already proves on every push?
 *
 *   SELFTEST_FULL  — today's behaviour, byte-identical output.  What CI
 *                    boots (the integration lib pins it via fw_cfg), so
 *                    every PASS-line grep keeps its teeth.
 *   SELFTEST_FAST  — the default boot.  Every self-test still RUNS and
 *                    still prints PASS/FAIL — only the gauntlet sizes
 *                    shrink (1 s PIT window → 100 ms, 10 000 heap cycles
 *                    → 500, 1 000 PMM frames → 100, 16 KiB RNG analysis
 *                    → 2 KiB).  A mis-programmed divisor, a broken free
 *                    list or a stuck RNG still fails loudly.
 *   SELFTEST_OFF   — benchmarking only: the scaled tests print SKIPPED.
 *
 * The mode comes from (in priority order):
 *   1. QEMU fw_cfg: -fw_cfg name=opt/auralite.selftest,string=full
 *      (kernel/arch/x86_64/fwcfg.c — the O2 plan text assumed "the
 *      kernel command line the loaders already pass"; measured, no such
 *      plumbing exists anywhere in boot/, and fw_cfg is the honest
 *      QEMU-shaped channel for a QEMU-primary project).
 *   2. The build default: make SELFTEST=full|fast|off, the KEYMAP
 *      precedent (FIX_R8) — which is also all real hardware gets.
 */
#ifndef KERNEL_LIB_SELFTEST_H
#define KERNEL_LIB_SELFTEST_H

#include <stdint.h>

typedef enum {
    SELFTEST_OFF  = 0,
    SELFTEST_FAST = 1,
    SELFTEST_FULL = 2,
} selftest_mode_t;

selftest_mode_t selftest_mode(void);
void            selftest_set_mode(selftest_mode_t m, const char *source);
const char     *selftest_mode_name(void);
const char     *selftest_mode_source(void);

/* The one scaling rule: FULL keeps the historical value (output stays
 * byte-identical), FAST takes the reduced one, OFF returns 0 and the
 * caller prints its SKIPPED line. */
uint64_t selftest_scale(uint64_t full_value, uint64_t fast_value);

#endif /* KERNEL_LIB_SELFTEST_H */
