/* kernel/arch/riscv64/trap.c -- scause decode, timer, self-tests
 * (RISCV_PLAN V2).  Sibling of isr32.c/isr.c: named diagnostics in
 * the FIX_R0 format (cpu= in every line, register dump, halt on
 * unhandled kernel faults), at the same bring-up scope I2 had.
 *
 * scause: bit 63 = interrupt, low bits = code.  Two tables, not one
 * -- exceptions and interrupts share code numbers and nothing else.
 *
 * The timer is SBI-owned (D2): sbi_set_timer arms ONE shot; the
 * handler re-arms.  100 Hz from the DTB's timebase-frequency -- on
 * virt that is 10 MHz, so 100000 counts per tick.  Each tick also
 * feeds the jitter pool the low rdtime-delta bits, the N0 fallback
 * entropy path; the DRBG itself joins the rv64 build with the shared
 * kernel code (V8 parity), so V2 collects and counts, honestly
 * labelled as not-yet-consumed.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"
#include "kernel/arch/riscv64/plic.h"
#include "kernel/arch/riscv64/thread_rv.h"
#include "kernel/arch/riscv64/user_rv.h"

/* ---- CSR helpers (names, not numbers, at every use site) ---------------- */

#define csr_read(csr)                                            \
    ({ uint64_t v;                                               \
       __asm__ volatile("csrr %0, " #csr : "=r"(v));             \
       v; })

#define csr_write(csr, v)                                        \
    __asm__ volatile("csrw " #csr ", %0" :: "r"((uint64_t)(v)))

#define csr_set(csr, v)                                          \
    __asm__ volatile("csrs " #csr ", %0" :: "r"((uint64_t)(v)))

/* sstatus/sie bits used here. */
#define SSTATUS_SIE  (1UL << 1)
#define SIE_STIE     (1UL << 5)     /* supervisor timer   */
#define SIE_SEIE     (1UL << 9)     /* supervisor external */

#define SCAUSE_INT   (1UL << 63)

/* ---- printing (sbi console; kprintf joins the build in V6) -------------- */

static void puts_(const char *s) { sbi_puts(s); }

static void put_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

static void put_dec(uint64_t v)
{
    char buf[20];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

/* ---- cause names (privileged spec table 4.2) ----------------------------- */

static const char *exception_names[16] = {
    "Instruction Address Misaligned",  /*  0 */
    "Instruction Access Fault",        /*  1 */
    "Illegal Instruction",             /*  2 */
    "Breakpoint",                      /*  3 */
    "Load Address Misaligned",         /*  4 */
    "Load Access Fault",               /*  5 */
    "Store/AMO Address Misaligned",    /*  6 */
    "Store/AMO Access Fault",          /*  7 */
    "Environment Call from U-mode",    /*  8 */
    "Environment Call from S-mode",    /*  9 */
    "Reserved (10)",                   /* 10 */
    "Reserved (11)",                   /* 11 */
    "Instruction Page Fault",          /* 12 */
    "Load Page Fault",                 /* 13 */
    "Reserved (14)",                   /* 14 */
    "Store/AMO Page Fault",            /* 15 */
};

/* ---- timer --------------------------------------------------------------- */

#define TICK_HZ 100

static uint64_t tick_interval;          /* timebase counts per tick */
static volatile uint64_t ticks;

uint64_t timer_ticks(void) { return ticks; }

/* ---- jitter pool (N0's fallback path, collection side) ------------------- */

static uint64_t jitter_pool[8];
static volatile uint64_t jitter_events;
static uint64_t jitter_last;

static void jitter_mix(uint64_t now)
{
    uint64_t delta = now - jitter_last;
    jitter_last = now;
    uint64_t *slot = &jitter_pool[jitter_events & 7];
    *slot = (*slot << 7) ^ (*slot >> 57) ^ delta;
    jitter_events++;
}

uint64_t trap_jitter_events(void) { return jitter_events; }

/* ---- the [isr] self-test hook -------------------------------------------- */

static volatile int expect_illegal = 0;
static volatile int illegal_seen   = 0;

/* ---- the V3 fault-probe hook ----------------------------------------------
 *
 * A W^X probe cannot resume by sepc += 4 (for execute-from-data, sepc
 * points INTO the unexecutable buffer).  Instead: setjmp before the
 * probe; on the expected scause the handler rewrites the frame so
 * sret lands in rv_longjmp_entry (trapentry.S) with a0 = the jmp buf,
 * unwinding to the setjmp with return value 1. */

extern int  rv_setjmp(uint64_t *buf);
extern void rv_longjmp_entry(void);

static uint64_t probe_jmpbuf[14];
static volatile int probe_armed;
static volatile int64_t probe_cause1 = -1, probe_cause2 = -1;

int trap_run_fault_probe(int64_t cause1, int64_t cause2,
                         void (*probe)(void *), void *arg)
{
    if (rv_setjmp(probe_jmpbuf)) {
        /* Landed back from the handler: the expected fault happened. */
        return 0;
    }
    probe_cause1 = cause1;
    probe_cause2 = cause2;
    probe_armed  = 1;
    probe(arg);
    /* Probe returned without faulting: that is the failure. */
    probe_armed = 0;
    return -1;
}

/* ---- dispatcher ----------------------------------------------------------- */

static uint64_t boot_hartid_for_dump;

static void dump_frame(const rv_trap_frame_t *f, uint64_t scause,
                       uint64_t stval)
{
    uint64_t hart = boot_hartid_for_dump;
    puts_("  cpu=hart");
    put_dec(hart);
    puts_("  scause=");
    put_hex(scause);
    puts_(" (");
    puts_((scause & SCAUSE_INT) ? "interrupt"
          : (scause < 16 ? exception_names[scause] : "Unknown"));
    puts_(")\n  sepc=");
    put_hex(f->sepc);
    puts_("  stval=");
    put_hex(stval);
    puts_("\n");

    static const char *names[31] = {
        " ra=", " sp=", " gp=", " tp=", " t0=", " t1=", " t2=", " s0=",
        " s1=", " a0=", " a1=", " a2=", " a3=", " a4=", " a5=", " a6=",
        " a7=", " s2=", " s3=", " s4=", " s5=", " s6=", " s7=", " s8=",
        " s9=", "s10=", "s11=", " t3=", " t4=", " t5=", " t6="
    };
    for (int i = 0; i < 31; i++) {
        puts_(i % 4 == 0 ? "  " : "  ");
        puts_(names[i]);
        put_hex(f->regs[i]);
        if (i % 4 == 3)
            puts_("\n");
    }
    puts_("\n");
}

void rv_trap(rv_trap_frame_t *f)
{
    uint64_t scause = csr_read(scause);
    uint64_t stval  = csr_read(stval);

    if (scause & SCAUSE_INT) {
        uint64_t code = scause & ~SCAUSE_INT;

        if (code == 5) {                        /* S-timer */
            uint64_t now = rv_rdtime();
            jitter_mix(now);
            ticks++;
            sched_rv_tick();
            sbi_set_timer(now + tick_interval); /* re-arm; also clears STIP */
            /* Preempt AFTER the re-arm -- the post-EOI placement:
             * sbi_set_timer is what clears STIP, and a context switch
             * before it would park this hart's timer until the
             * interrupted thread happens to run again (the phase-6
             * freeze on x86_64, inherited as a design input). */
            sched_rv_maybe_preempt();
            return;
        }
        if (code == 9) {                        /* S-external -> PLIC */
            plic_dispatch();
            return;
        }
        /* SSIP and anything else: nobody sends these yet.  Named,
         * not ignored -- an unexpected interrupt is a bug report. */
        puts_("[isr] UNEXPECTED interrupt\n");
        dump_frame(f, scause, stval);
        return;
    }

    /* The V3 fault probes: an EXPECTED W^X/identity fault unwinds to
     * the probe's setjmp instead of resuming (sepc += 4 cannot resume
     * an execute-from-data fault -- sepc IS the bad address). */
    if (probe_armed &&
        ((int64_t)scause == probe_cause1 || (int64_t)scause == probe_cause2)) {
        probe_armed = 0;
        puts_("[isr] ");
        puts_(scause < 16 ? exception_names[scause] : "Unknown");
        puts_(" at sepc=");
        put_hex(f->sepc);
        puts_(" -- expected (fault probe), unwinding\n");
        f->sepc    = (uint64_t)rv_longjmp_entry;
        f->regs[9] = (uint64_t)probe_jmpbuf;   /* regs[9] = x10 = a0 */
        return;
    }

    /* U-mode ecall: the syscall path (D4 -- a7 number, a0-a5 args,
     * a0 return).  sepc += 4 FIRST: sret must land on the instruction
     * after the ecall, and an exit-unwind never comes back here. */
    if (scause == 8) {
        f->sepc += 4;
        user_rv_syscall(f);
        return;
    }

    /* Any other U-mode exception: contained, never fatal.  The image
     * dies with 128+scause and the kernel thread that hosted it keeps
     * running -- isr32's cs&3 check, SPP-flavoured. */
    if (!(f->sstatus & (1UL << 8)) && user_rv_fault(f, scause, stval))
        return;

    /* Exception.  The deliberate-fault self-test first: an expected
     * illegal instruction is named, then RESUMED PAST -- sepc += 4
     * (the probe instruction is a full-width .word, never compressed). */
    if (scause == 2 && expect_illegal) {
        expect_illegal = 0;
        illegal_seen   = 1;
        puts_("[isr] Illegal Instruction at sepc=");
        put_hex(f->sepc);
        puts_(" -- expected (self-test), resuming past\n");
        f->sepc += 4;
        return;
    }

    /* Unhandled kernel fault: dump in the R0 format and halt.  No
     * U-mode until V4, so every exception here is the kernel's own
     * bug -- same policy as isr32.c's kernel-fault path. */
    puts_("\n[isr] UNHANDLED EXCEPTION -- halting\n");
    dump_frame(f, scause, stval);
    sbi_shutdown();
}

/* ---- self-test ------------------------------------------------------------ */

int trap_selftest(void)
{
    expect_illegal = 1;
    illegal_seen   = 0;
    /* An all-zero word is a guaranteed-illegal encoding on RISC-V
     * (defined so by the ISA spec, precisely for traps like this). */
    __asm__ volatile(".word 0x00000000");
    return illegal_seen ? 0 : -1;
}

/* ---- init ------------------------------------------------------------------ */

extern void rv_trap_vector(void);   /* trapentry.S */

void trap_init(uint32_t timebase_freq)
{
    /* Direct mode: low bits 00.  trapentry.S aligns the symbol to 4. */
    csr_write(stvec, (uint64_t)rv_trap_vector);

    /* The hart id, parked in sscratch for diagnostics ONLY in V2.
     * V4 repurposes sscratch for the U-mode trap-stack swap (the plan
     * says so); the dump reads it while it is free real estate. */
    /* (written by kmain_rv before trap_init: see the call site) */

    if (timebase_freq == 0)
        timebase_freq = 10000000;   /* virt's value; a DTB without the
                                     * property gets a loud default */
    tick_interval = timebase_freq / TICK_HZ;
    jitter_last = rv_rdtime();

    /* Arm the first tick, then open the gates: STIE + SEIE, then the
     * global SIE bit last -- nothing can fire half-configured. */
    sbi_set_timer(rv_rdtime() + tick_interval);
    csr_set(sie, SIE_STIE | SIE_SEIE);
    csr_set(sstatus, SSTATUS_SIE);
}

void trap_set_hartid(uint64_t hartid)
{
    /* V2-V3 parked this in sscratch; V4's trap entry owns that CSR
     * now (the swap-and-test stack switch), so a plain variable. */
    boot_hartid_for_dump = hartid;
}
