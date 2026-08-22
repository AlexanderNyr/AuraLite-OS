/* kernel/arch/aarch64/trap_a64.h -- EL1 trap handling (ARM64_PLAN A2). */

#ifndef AURALITE_ARCH_AARCH64_TRAP_A64_H
#define AURALITE_ARCH_AARCH64_TRAP_A64_H

#include <stdint.h>

/* The frame vectors.S builds: x0..x29 at N*8, x30 at 240, ELR_EL1 at
 * 248, SPSR_EL1 at 256, the vector-slot kind tag at 264; 288 bytes
 * total to keep sp 16-byte aligned (AAPCS64's rule holds inside the
 * kernel too). */
typedef struct {
    uint64_t regs[31];          /* x0..x30 */
    uint64_t elr;
    uint64_t spsr;
    uint64_t kind;              /* slot tag: kind*4 + origin */
    uint64_t _pad[2];
} a64_trap_frame_t;

/* Install VBAR_EL1, program the virtual timer at TICK_HZ from
 * CNTFRQ_EL0 (a register, not a DTB field -- plan Fact 2.3), enable
 * the timer INTID through the GIC, unmask IRQ in DAIF. */
void trap_init_a64(void);

/* R5: VBAR-only install for a secondary (no timer, no unmask). */
void trap_init_a64_secondary(void);

/* Ticks observed so far (the [timer] gate reads this). */
uint64_t timer_ticks_a64(void);

/* The [isr] self-test: execute a guaranteed-UNDEFINED instruction,
 * expect the handler to name EC 0x00 and resume past.  0 = pass. */
int trap_selftest_a64(void);

/* The A2 alignment probe: an unaligned load with the MMU off must
 * Data-Abort (Device-nGnRnE memory -- plan Fact 5.1).  0 = faulted
 * as expected; -1 = it silently succeeded, which would mean the
 * -mstrict-align world model is wrong and A3 must find out why. */
int trap_alignment_probe_a64(void);

/* A3 fault probes: run probe(arg) expecting ESR EC ec1 or ec2.
 * Returns 0 if the expected fault happened (control unwinds back via
 * a setjmp in vectors.S), -1 if the probe survived unfaulted.  Two
 * ECs because aborts report same-EL/lower-EL as different classes;
 * the A3 probes pass the same value twice. */
int trap_run_fault_probe_a64(int64_t ec1, int64_t ec2,
                             void (*probe)(void *), void *arg);

/* Jitter events collected so far (timer-trap CNTVCT deltas -- the
 * same N0 fallback-entropy shape the riscv64 port feeds). */
uint64_t trap_jitter_events_a64(void);

/* CNTVCT_EL0 -- the virtual counter (the ISB is architectural
 * hygiene: reads can be served speculatively early without it). */
static inline uint64_t a64_cntvct(void)
{
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

#endif /* AURALITE_ARCH_AARCH64_TRAP_A64_H */
