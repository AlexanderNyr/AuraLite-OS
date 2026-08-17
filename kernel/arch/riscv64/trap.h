/* kernel/arch/riscv64/trap.h -- S-mode trap handling (RISCV_PLAN V2). */

#ifndef AURALITE_ARCH_RISCV64_TRAP_H
#define AURALITE_ARCH_RISCV64_TRAP_H

#include <stdint.h>

/* The frame trapentry.S builds: x1..x31 at (N-1)*8, sepc at 248.
 * regs[1] (x2/sp) is display-only -- the exit path recomputes sp. */
typedef struct {
    uint64_t regs[31];          /* x1..x31 */
    uint64_t sepc;
} rv_trap_frame_t;

/* Install stvec, enable SIE.STIE + SIE.SEIE, start the 100 Hz timer.
 * timebase_freq comes from the DTB (/cpus timebase-frequency). */
void trap_init(uint32_t timebase_freq);

/* Ticks observed so far (the [timer] gate reads this). */
uint64_t timer_ticks(void);

/* The [isr] self-test: execute an illegal instruction, expect the
 * handler to name it and resume past.  Returns 0 on pass. */
int trap_selftest(void);

/* Jitter events collected so far (timer-trap rdtime deltas -- the N0
 * fallback entropy path's collection side; the DRBG consumes in V8). */
uint64_t trap_jitter_events(void);

/* Park the hart id in sscratch for the frame dump's cpu= field.
 * V2-only use of sscratch; V4 takes it for the trap-stack swap. */
void trap_set_hartid(uint64_t hartid);

/* V3 fault probes: run probe(arg) expecting scause cause1 or cause2.
 * Returns 0 if the expected fault happened (control unwinds back via
 * a setjmp in trapentry.S), -1 if the probe survived unfaulted.
 * Access-fault/page-fault pairs are passed together because PMPs
 * (firmware-owned) and PTEs (ours) report the same sin differently. */
int trap_run_fault_probe(int64_t cause1, int64_t cause2,
                         void (*probe)(void *), void *arg);

/* rdtime -- the timebase counter, for the jitter pool and timers. */
static inline uint64_t rv_rdtime(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

#endif /* AURALITE_ARCH_RISCV64_TRAP_H */
