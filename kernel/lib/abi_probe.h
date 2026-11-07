/* abi_probe.h — RESIDUE3 C2: the in-guest receipt for the 16-byte C ABI.
 *
 * The DECLARATION lives here, in the portable tree, because kmain (a portable
 * file) arms the probe; the IMPLEMENTATION is kernel/arch/x86_64/abi_probe.c,
 * where fxsave belongs.  Keeping the prototype out of arch/ is what holds the
 * width-sweep ratchet (check_width_sweep.py: direct x86_64 includes from
 * portable code) at its pinned 69.
 *
 * The static model (tools/check_abi_align.py) proves the phase arithmetic on
 * paper.  This is the other half: an aligned SSE operation executed INSIDE
 * each of the two entry contexts, so a real #GP would happen in QEMU if the
 * arithmetic were wrong.  fxsave is the canonical probe — it requires a
 * 16-byte-aligned 512-byte operand and #GPs on anything else, which is
 * exactly how M5 discovered the problem in the first place.
 */
#ifndef AURALITE_ARCH_ABI_PROBE_H
#define AURALITE_ARCH_ABI_PROBE_H

void abi_probe_arm(void);      /* called once from kmain, before the shell */
void abi_probe_irq(void);      /* called from isr_handler's IRQ branch */
void abi_probe_syscall(void);  /* called from syscall_dispatch's entry */
int  abi_probe_all_done(void); /* 1 when both contexts have passed */

#endif
