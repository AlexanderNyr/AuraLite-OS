#ifndef AURALITE_ARCH_X86_64_APWAKE_H
#define AURALITE_ARCH_X86_64_APWAKE_H

/* apwake.h -- the RESIDUE2 T2 / ledger RES-16 receipt selftest.
 *
 * "A device IRQ waking a hlt-ed AP" was the one unproven SMP interrupt
 * path (MATURITY follow-up carried in the residue ledger).  This test
 * proves it end to end on a live -smp machine:
 *
 *   1. A kthread whose whole body is `while (!done) hlt;` is enqueued
 *      DIRECTLY onto cpu 1's run queue (not the least-loaded queue), so
 *      it executes on that AP.
 *   2. The RTC periodic interrupt (a real device IRQ on ISA IRQ 8 /
 *      GSI 8) is enabled and its I/O APIC redirection entry is aimed at
 *      cpu 1's APIC ID via ioapic_route_gsi().
 *   3. The handler counts deliveries and records WHICH cpu they landed
 *      on; after enough, the hlt looper leaves its hlt and exits.
 *
 * Prints "[smpwake] PASS ... RES-16 receipt" and returns 0 when the
 * deliveries reached the target AP (and no other), -1 otherwise.
 */
int smp_irq_ap_wake_selftest(void);

#endif /* AURALITE_ARCH_X86_64_APWAKE_H */
