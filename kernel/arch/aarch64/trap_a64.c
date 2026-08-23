/* kernel/arch/aarch64/trap_a64.c -- EL1 trap dispatch (ARM64_PLAN A2).
 *
 * rv_trap's counterpart.  What scause was there, ESR_EL1 is here --
 * with one structural difference worth a sentence: RISC-V splits
 * interrupt-vs-exception inside ONE cause register, while aarch64
 * splits them at the VECTOR (separate IRQ and synchronous slots) and
 * ESR only ever describes the synchronous kind.  So this file's
 * dispatch reads the frame's slot tag first and ESR second, and the
 * "unexpected interrupt is a bug report" policy attaches to whole
 * vector rows (FIQ, SError, anything from SP_EL0) instead of to
 * cause codes.
 *
 * Timer: the virtual timer (CNTV) at PPI INTID 27 -- measured in the
 * A1 walk (the DTB's <1 0xb 0x104> row) and re-measured live here
 * the first time the smoke ran.  Frequency from CNTFRQ_EL0, the
 * register (plan Fact 2.3): no DTB field, no calibration loop.
 * TICK_HZ = 100, the same 100 the other three kernels tick at.
 *
 * The re-arm is TVAL-relative (CNTV_TVAL := interval), which cannot
 * drift-accumulate the way a CVAL += interval chain can miss when a
 * tick is served late; and the write itself clears the ISTATUS
 * condition -- the "re-arm is what un-asserts the line" property
 * sbi_set_timer has, so the post-EOI preemption placement transfers
 * unchanged when A4 wants it (the phase-6 freeze, still pre-paid).
 */

#include <stdint.h>

#include "kernel/arch/aarch64/gic.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/psci.h"
#include "kernel/arch/aarch64/thread_a64.h"
#include "kernel/arch/aarch64/trap_a64.h"
#include "kernel/arch/aarch64/user_a64.h"

#define TICK_HZ 100

/* The virtual timer's PPI: DTB row 3 = <GIC_PPI 0xb 0x104>, INTID
 * 11 + 16 = 27 after the A1 normalisation.  Named here as arithmetic
 * so the constant has a paper trail instead of being folklore. */
#define TIMER_PPI_INTID (11u + 16u)

/* CNTV_CTL bits. */
#define CNTV_CTL_ENABLE  (1u << 0)
#define CNTV_CTL_IMASK   (1u << 1)

static volatile uint64_t ticks;
static uint64_t tick_interval;         /* CNTFRQ / TICK_HZ */

/* Jitter pool feed, the rv shape: mix the counter delta on every
 * timer trap; the DRBG consumes when the parity phase brings the
 * crypto over. */
static volatile uint64_t jitter_events;
static uint64_t jitter_last;

static volatile int expect_undef;
static volatile int undef_seen;
static volatile int expect_dabort;
static volatile int dabort_seen;

/* ---- the A3 fault-probe unwind (rv_setjmp's shape) ---------------------- */

extern int  a64_setjmp(uint64_t *buf);          /* vectors.S */
extern void a64_longjmp_entry(void);            /* vectors.S */

static uint64_t probe_jmpbuf[14];
static volatile int probe_armed;
static volatile int64_t probe_ec1 = -1, probe_ec2 = -1;

int trap_run_fault_probe_a64(int64_t ec1, int64_t ec2,
                             void (*probe)(void *), void *arg)
{
    if (a64_setjmp(probe_jmpbuf)) {
        /* Landed back from the handler: the expected fault happened. */
        return 0;
    }
    probe_ec1   = ec1;
    probe_ec2   = ec2;
    probe_armed = 1;
    probe(arg);
    /* Probe returned without faulting: that is the failure. */
    probe_armed = 0;
    return -1;
}

/* ---- tiny output (pl011.c's helpers, kept local names to match
 * trap.c's puts_/put_hex reading rhythm) ---------------------------------- */

static void puts_(const char *s)  { pl011_puts(s); }
static void put_hex(uint64_t v)   { pl011_puthex64(v); }
static void put_dec(uint64_t v)   { pl011_putdec64(v); }

/* ---- ESR decode ------------------------------------------------------------
 *
 * EC (exception class, ESR[31:26]): the 6-bit "what happened".  The
 * table names the classes this kernel can meet today plus the ones
 * A4 arms; everything else prints as a hex EC -- named-not-silent,
 * the exception_names[] policy with a sparser table. */

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown/Undefined instruction";
    case 0x0E: return "Illegal Execution state";
    case 0x15: return "SVC (AArch64)";          /* A4's syscall gate */
    case 0x18: return "MSR/MRS trapped";
    case 0x20: return "Instruction Abort, lower EL";
    case 0x21: return "Instruction Abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort, lower EL";
    case 0x25: return "Data Abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "FP exception";
    case 0x2F: return "SError";
    default:   return 0;
    }
}

static const char *kind_names[16] = {
    "SYNC/SP_EL0", "IRQ/SP_EL0", "FIQ/SP_EL0", "SERROR/SP_EL0",
    "SYNC",        "IRQ",        "FIQ",        "SERROR",
    "SYNC/EL0",    "IRQ/EL0",    "FIQ/EL0",    "SERROR/EL0",
    "SYNC/A32",    "IRQ/A32",    "FIQ/A32",    "SERROR/A32",
};

static uint64_t esr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static uint64_t far_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

/* The R0 dump format, fourth edition: cpu= first (FIX_R0's rule),
 * then the cause line, then the registers four to a row. */
static void dump_frame(const a64_trap_frame_t *f,
                       uint64_t esr, uint64_t far)
{
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;
    const char *nm = ec_name(ec);

    puts_("  cpu=0  vector=");
    puts_(f->kind < 16 ? kind_names[f->kind] : "?");
    puts_("  esr=");
    put_hex(esr);
    puts_("\n  ec=");
    if (nm) {
        puts_(nm);
    } else {
        puts_("class 0x");
        put_hex(ec);
    }
    puts_("\n  elr=");
    put_hex(f->elr);
    puts_("  far=");
    put_hex(far);
    puts_("  spsr=");
    put_hex(f->spsr);
    puts_("\n");

    for (int i = 0; i < 31; i++) {
        puts_("  x");
        put_dec((uint64_t)i);
        puts_("=");
        put_hex(f->regs[i]);
        if (i % 4 == 3)
            puts_("\n");
    }
    puts_("\n");
}

/* ---- the timer -------------------------------------------------------------- */

static void cntv_write_ctl(uint32_t v)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)v));
}

static void cntv_write_tval(uint64_t v)
{
    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(v));
}

static uint64_t cntfrq_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void timer_irq(uint32_t intid)
{
    uint64_t now = a64_cntvct();

    /* Jitter feed before anything time-shaped happens to the value. */
    jitter_events += ((now - jitter_last) & 0xFFu) != 0;
    jitter_last = now;

    ticks++;
    sched_a64_tick();
    /* TVAL re-arm: also what un-asserts the line (ISTATUS clears when
     * the deadline moves forward) -- the sbi_set_timer property.  A4's
     * preemption hook is NOT here: it sits after gic_dispatch returns
     * in a64_trap (post-EOI -- EOIR must complete this INTID before a
     * context switch can suspend the interrupted thread). */
    cntv_write_tval(tick_interval);
}

/* ---- dispatch ---------------------------------------------------------------- */

void a64_trap(a64_trap_frame_t *f)
{
    uint32_t kind = (uint32_t)f->kind;

    /* IRQ -- from the kernel's own row OR from EL0 (the timer does
     * not care whom it interrupted): the GIC has the answer, and the
     * preemption hook runs AFTER the dispatch loop returns -- every
     * claimed INTID has been EOIR-completed by then (the post-EOI
     * placement, fourth edition; see timer_irq's comment). */
    if (kind == 5 || kind == 9) {           /* IRQ / SP_ELx, IRQ / EL0 */
        gic_dispatch();
        sched_a64_maybe_preempt();
        return;
    }

    /* Synchronous from EL0: the svc gate first, contained faults
     * second -- isr32's cs&3 check, vector-row-flavoured (the origin
     * is IN the slot index here; no SPSR bit to test). */
    if (kind == 8) {                        /* SYNC / EL0 (AArch64) */
        uint64_t esr = esr_read();
        uint32_t ec  = (uint32_t)(esr >> 26) & 0x3F;

        if (ec == 0x15) {                   /* SVC (AArch64) */
            user_a64_syscall(f);
            return;
        }
        if (user_a64_fault(f, esr, far_read()))
            return;
        /* An EL0 trap with no active image: fall through to the
         * halt -- it IS a kernel bug (nobody armed EL0). */
        puts_("\n[isr] EL0 trap with no active image -- halting\n");
        dump_frame(f, esr, far_read());
        psci_system_off();
    }

    if (kind == 4) {                        /* SYNC / SP_ELx */
        uint64_t esr = esr_read();
        uint32_t ec  = (uint32_t)(esr >> 26) & 0x3F;

        /* The A3 fault probes: an EXPECTED W^X/identity fault unwinds
         * to the probe's setjmp instead of resuming (elr += 4 cannot
         * resume an execute-from-data fault -- elr IS the bad
         * address).  The rv probe_armed path, EC-flavoured. */
        if (probe_armed &&
            ((int64_t)ec == probe_ec1 || (int64_t)ec == probe_ec2)) {
            probe_armed = 0;
            puts_("[isr] ");
            puts_(ec_name(ec) ? ec_name(ec) : "fault");
            puts_(" at elr=");
            put_hex(f->elr);
            puts_(" far=");
            put_hex(far_read());
            puts_(" -- expected (fault probe), unwinding\n");
            f->elr     = (uint64_t)a64_longjmp_entry;
            f->regs[0] = (uint64_t)probe_jmpbuf;
            return;
        }

        /* The deliberate-fault self-test: an expected undefined
         * instruction is named, then RESUMED PAST -- elr += 4 (the
         * probe word is a fixed-width all-zero encoding). */
        if (ec == 0x00 && expect_undef) {
            expect_undef = 0;
            undef_seen   = 1;
            puts_("[isr] Unknown/Undefined instruction at elr=");
            put_hex(f->elr);
            puts_(" -- expected (self-test), resuming past\n");
            f->elr += 4;
            return;
        }

        /* The alignment probe: with the MMU off all memory is
         * Device-nGnRnE and an unaligned load MUST fault (plan Fact
         * 5.1 -- the fact -mstrict-align exists to protect the
         * kernel's own code from).  EC 0x25, DFSC alignment; the
         * probe instruction is one fixed-width ldr, resume past. */
        if (ec == 0x25 && expect_dabort) {
            expect_dabort = 0;
            dabort_seen   = 1;
            puts_("[isr] Data Abort, same EL at elr=");
            put_hex(f->elr);
            puts_(" far=");
            put_hex(far_read());
            puts_(" -- expected (alignment probe), resuming past\n");
            f->elr += 4;
            return;
        }

        /* Unhandled kernel fault: dump in the R0 format and halt --
         * no EL0 until A4, so every synchronous trap here is the
         * kernel's own bug.  (A4 splices its SVC and EL0-fault
         * routing above this line, in the EL0 vector rows.) */
        puts_("\n[isr] UNHANDLED EXCEPTION -- halting\n");
        dump_frame(f, esr, far_read());
        psci_system_off();
    }

    /* Everything else -- FIQ, SError, anything on SP_EL0, anything
     * AArch32 -- is a row nobody arms in this kernel: a bug report,
     * named by its vector slot. */
    puts_("\n[isr] UNEXPECTED vector row -- halting\n");
    dump_frame(f, esr_read(), far_read());
    psci_system_off();
}

/* ---- self-test ---------------------------------------------------------------- */

int trap_selftest_a64(void)
{
    expect_undef = 1;
    undef_seen   = 0;
    /* 0x00000000 decodes as UDF #0 on AArch64 -- the guaranteed-
     * undefined encoding, the ISA's own trap bait (the .word 0
     * trick, same as riscv64's). */
    __asm__ volatile(".word 0x00000000");
    return undef_seen ? 0 : -1;
}

int trap_alignment_probe_a64(void)
{
    /* An unaligned doubleword load, forced in assembly (the compiler
     * under -mstrict-align will never emit one -- which is exactly
     * why the probe must be handwritten).  MMU off => Device memory
     * => alignment fault, EC 0x25.  The address is our own stack
     * page + 1: mapped, readable, wrong by one. */
    uint64_t scratch[2] = { 0, 0 };
    uint64_t dummy;

    expect_dabort = 1;
    dabort_seen   = 0;
    __asm__ volatile("ldr %0, [%1]"
                     : "=r"(dummy)
                     : "r"((uint8_t *)scratch + 1)
                     : "memory");
    (void)dummy;
    expect_dabort = 0;   /* disarm: on Normal memory (post-A3) the load
                          * SUCCEEDS and an armed expectation left behind
                          * would swallow the next real Data Abort */
    return dabort_seen ? 0 : -1;
}

uint64_t timer_ticks_a64(void)
{
    return ticks;
}

uint64_t trap_jitter_events_a64(void)
{
    return jitter_events;
}

/* ---- init -------------------------------------------------------------------- */

extern char a64_vectors[];             /* vectors.S; 2048-aligned */

/* R5: a secondary core's trap surface -- VBAR only.  No timer, no
 * DAIF unmask: the user job runs with IRQs masked (SVCs and faults
 * are synchronous), so the boot core's tick and GIC state are never
 * touched from here. */
void trap_init_a64_secondary(void)
{
    __asm__ volatile("msr vbar_el1, %0; isb"
                     : : "r"((uint64_t)a64_vectors));
    /* R8: open the EL0 counter gate (EL0PCTEN|EL0VCTEN) -- cntvct_
     * el0 at EL0 traps while CNTKCTL_EL1 sits at its UNKNOWN reset;
     * the Rust row's cycle read is the measurement.  Per core,
     * because R5 runs init ON a secondary. */
    __asm__ volatile("msr cntkctl_el1, %0" :: "r"(3UL));
}

void trap_init_a64(void)
{
    /* Vectors first: from this line on, a fault has a name. */
    __asm__ volatile("msr vbar_el1, %0; isb"
                     : : "r"((uint64_t)a64_vectors));

    /* R8: open the EL0 counter gate -- same line the secondary init
     * carries; see trap_init_a64_secondary. */
    __asm__ volatile("msr cntkctl_el1, %0" :: "r"(3UL));

    /* The tick, from the register that cannot lie (Fact 2.3). */
    uint64_t frq = cntfrq_read();
    tick_interval = frq / TICK_HZ;
    jitter_last = a64_cntvct();

    /* GIC line first, then the timer, then DAIF -- gates open
     * outward, nothing fires half-configured (trap_init's rule). */
    gic_enable(TIMER_PPI_INTID, timer_irq);
    cntv_write_tval(tick_interval);
    cntv_write_ctl(CNTV_CTL_ENABLE);       /* IMASK clear: line live */

    __asm__ volatile("msr daifclr, #2");   /* unmask IRQ */
}
