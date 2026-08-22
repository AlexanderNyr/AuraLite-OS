/* kernel/arch/aarch64/main_a64.c -- aarch64 kernel main (ARM64_PLAN A1).
 *
 * A0 proved the chain (clang -> lld -> QEMU ELF load -> EL1 _start ->
 * PL011 -> PSCI power-off) and echoed the measured facts.  A1 makes
 * this kernel the FOURTH consumer of boot_info_t -- and the second
 * consumer of the SHARED DTB walker (kernel/dt/fdt.c, promoted from
 * kernel/arch/riscv64/ in this same patch): the walk that fills the
 * struct on riscv64 fills it here, one object file, two boards.
 *
 * The boot log deliberately rhymes with main_rv.c's: "handoff magic
 * OK", an mmap summary, an initrd line, a [hw] platform block.  Four
 * kernels, one shape -- a person reading any serial log knows where
 * they are.
 *
 * The A0 fact echoes stay: x0-at-entry and the RAM-base magic probe
 * are regression gates for QEMU behaviour, not one-time curiosities.
 */

#include <stdint.h>

#include "boot/shared/boot_info.h"
#include "kernel/dt/fdt.h"
#include "kernel/arch/aarch64/gic.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/psci.h"
#include "kernel/arch/aarch64/thread_a64.h"
#include "kernel/arch/aarch64/trap_a64.h"
#include "kernel/arch/aarch64/user_a64.h"
#include "kernel/arch/aarch64/initrd_a64.h"
#include "kernel/arch/aarch64/vblk_a64.h"
#include "kernel/arch/aarch64/fsglue_a64.h"
#include "kernel/lib/kprintf.h"
#include "kernel/arch/aarch64/smp_a64.h"
#include "kernel/arch/aarch64/vnet_a64.h"
#include "kernel/arch/aarch64/membench_a64.h"

#define RAM_BASE      0x40000000UL
#define FDT_MAGIC_BE  0xD00DFEEDUL

/* The structs the FDT walk fills.  Static in .bss (boot.S zeroed
 * it): ~9 KiB is too big for the boot stack -- the same note as
 * main_rv.c, because it is the same struct. */
static boot_info_t    boot_info;
static fdt_platform_t platform;

/* Contract 1 of the shared walker (kernel/dt/fdt.h): how a physical
 * DTB address becomes a pointer.  Since A3 boot.S turns the MMU on
 * before any C runs, so the HHDM answers -- the same one-liner
 * main_rv.c has, for the same reason (and bootargs points into this
 * buffer, so the mapping outlives the call by construction: the HHDM
 * is forever). */
const void *dt_phys_to_virt(uint64_t phys)
{
    return p2v_a64(phys);
}

static uint64_t read_currentel(void)
{
    uint64_t el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static uint64_t read_cntfrq(void)
{
    uint64_t f;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

/* The DTB magic, read big-endian byte-wise from the RAM base --
 * through the HHDM since A3 (the low half belongs to TTBR0, which
 * the final tables blank).  Byte loads: the byte-order answer and
 * the alignment answer in one (plan Fact 5.1). */
/* A5a: the magic probe, generalised — the Image path hands the DTB
 * address in x0 and it is NOT the RAM base there (QEMU parks the blob
 * after the initrd).  Same read, caller's address. */
static uint32_t fdt_magic_at(uint64_t phys);

static uint32_t fdt_magic_at_ram_base(void)
{
    const volatile uint8_t *p =
        (const volatile uint8_t *)p2v_a64(RAM_BASE);

    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t fdt_magic_at(uint64_t phys)
{
    const volatile uint8_t *p =
        (const volatile uint8_t *)p2v_a64(phys);

    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ---- boot_info consumption (the shape main_rv.c established) ----------- */

static int boot_info_check(void)
{
    if (boot_info.magic != BOOT_MAGIC) {
        pl011_puts("[boot] handoff magic BAD\n");
        return -1;
    }
    pl011_puts("[boot] handoff magic OK, path=PSCI, boot_info filled from DTB\n");
    return 0;
}

static void memmap_report(void)
{
    uint64_t usable = 0;

    for (uint32_t i = 0; i < boot_info.mmap_count; i++)
        if (boot_info.mmap[i].type == BOOT_MEM_USABLE)
            usable += boot_info.mmap[i].length;

    pl011_puts("[mm]   mmap entries: ");
    pl011_putdec64(boot_info.mmap_count);
    pl011_puts(", usable RAM: ");
    pl011_putdec64(usable / (1024 * 1024));
    pl011_puts(" MiB\n");

    if (boot_info.initrd_phys) {
        /* A5a: prove the bytes are REALLY there, not just advertised —
         * a USTAR archive carries "ustar" at offset 257 of the first
         * header block.  The A5c tenant walk will stand on this. */
        const volatile uint8_t *tar =
            (const volatile uint8_t *)p2v_a64(boot_info.initrd_phys);
        int ustar_ok = boot_info.initrd_size > 262 &&
                       tar[257] == 'u' && tar[258] == 's' &&
                       tar[259] == 't' && tar[260] == 'a' &&
                       tar[261] == 'r';
        pl011_puts(ustar_ok ? "[mm]   initrd magic: ustar OK\n"
                            : "[mm]   initrd magic: NOT a ustar archive\n");
        pl011_puts("[mm]   initrd: ");
        pl011_putdec64(boot_info.initrd_size);
        pl011_puts(" bytes at phys ");
        pl011_puthex64(boot_info.initrd_phys);
        pl011_puts("\n");
    } else {
        pl011_puts("[mm]   initrd: none\n");
    }
}

static void platform_report(void)
{
    pl011_puts("[hw]   cpus: ");
    pl011_putdec64(boot_info.cpu_count);
    pl011_puts(" (boot cpu ");
    pl011_putdec64(boot_info.bsp_lapic_id);
    pl011_puts(")\n[hw]   uart: ");
    pl011_puthex64(platform.uart_base);
    pl011_puts(" irq ");
    pl011_putdec64(platform.uart_irq);
    pl011_puts(" (INTID, normalised)\n[hw]   gicd: ");
    pl011_puthex64(platform.gicd_base);
    pl011_puts(" gicc: ");
    pl011_puthex64(platform.gicc_base);
    pl011_puts("\n[hw]   virtio-mmio windows: ");
    pl011_putdec64(platform.virtio_count);
    if (platform.virtio_count > 0) {
        pl011_puts(" (irq ");
        pl011_putdec64(platform.virtio_irq[0]);
        pl011_puts("..");
        pl011_putdec64(platform.virtio_irq[platform.virtio_count - 1]);
        pl011_puts(")");
    }
    pl011_puts("\n");
    if (platform.bootargs) {
        pl011_puts("[hw]   bootargs: ");
        pl011_puts(platform.bootargs);
        pl011_puts("\n");
    }

    /* D2's assert: the PSCI conduit this kernel hardcodes (hvc, in
     * psci.c) must be the one the tree declares.  An smc board would
     * need one instruction changed -- this line is where it gets
     * NAMED instead of hanging in psci_call. */
    if (platform.psci_method == FDT_PSCI_HVC) {
        pl011_puts("[hw]   psci: method hvc (matches psci.c's conduit)\n");
    } else if (platform.psci_method == FDT_PSCI_SMC) {
        pl011_puts("[hw]   psci: method smc -- MISMATCH, psci.c uses hvc; "
                   "power-off may hang\n");
    } else {
        pl011_puts("[hw]   psci: absent from the tree\n");
    }
}

/* ---- A2: vectors, timer, GIC --------------------------------------------- */

/* Spin until the virtual counter has advanced `cycles` -- pure
 * busy-wait on CNTVCT, usable while interrupts do the real work. */
static void spin_cycles(uint64_t cycles)
{
    uint64_t end = a64_cntvct() + cycles;
    while (a64_cntvct() < end)
        __asm__ volatile("yield");
}

static void a2_bringup(void)
{
    if (platform.gicd_base == 0 || platform.gicc_base == 0) {
        pl011_puts("[gic]  no GICv2 in the tree; A2 gates skipped\n");
        return;
    }

    /* HHDM view: the MMU is on from boot.S since A3, and A3's final
     * tables map this plateau Device-nGnRE (the transport-refuses-
     * Normal rule arrives with A7; the kernel's own drivers get the
     * right attributes by construction here). */
    gic_init((uint64_t)p2v_a64(platform.gicd_base),
             (uint64_t)p2v_a64(platform.gicc_base),
             (int)platform.gic_is_v3);

    /* PARITY P6: secondaries up via PSCI CPU_ON, counted, one SGI
     * round-trip (D5: receipts, not scheduling).  Same HHDM bases
     * the GIC driver just took. */
    smp_a64_bringup(&boot_info,
                    (uint64_t)p2v_a64(platform.gicd_base),
                    (uint64_t)p2v_a64(platform.gicc_base));
    pl011_puts("[gic]  GICv2 up: distributor + CPU interface, INTIDs pre-normalised\n");

    trap_init_a64();
    pl011_puts("[isr]  VBAR_EL1 installed (16 slots x 128 bytes), IRQ unmasked\n");

    /* Gate 1: the deliberate fault -- named, resumed past. */
    if (trap_selftest_a64() == 0)
        pl011_puts("[isr]  PASS: undefined instruction named and resumed\n");
    else
        pl011_puts("[isr]  FAIL: self-test fault did not arrive\n");

    /* Gate 1b: the alignment world model, MAPPED-DEVICE edition --
     * and a measured QEMU fact worth its own line.  The architecture
     * says an unaligned access to Device memory faults; A2 measured
     * exactly that with the MMU OFF.  With the MMU ON and the early
     * tables attributing RAM Device-nGnRnE, QEMU's TCG does NOT
     * model the fault (measured here: the same load that Data-
     * Aborted pre-MMU sails through on a mapped Device attribute).
     * Real hardware may fault where TCG did not -- which is exactly
     * why -mstrict-align STAYS ON for the kernel: the compiler never
     * emits the access class TCG under-models.  The gate pins the
     * TCG behaviour so a QEMU change announces itself. */
    if (trap_alignment_probe_a64() != 0)
        pl011_puts("[isr]  PASS: unaligned load on MAPPED Device RAM not faulted by TCG (pinned QEMU fact; hw may differ -- strict-align stays)\n");
    else
        pl011_puts("[isr]  PASS: unaligned load on mapped Device RAM faulted (TCG began modelling it -- update the A3 result note)\n");

    /* Gate 2: the tick.  CNTFRQ is 62.5 MHz (Fact 2.3); spin half a
     * guest second and expect ~50 ticks at 100 Hz.  The band is wide
     * (10..90) because the assertion is "the timer LIVES and re-arms",
     * not "QEMU's scheduler is fair" -- the rv smoke's reasoning. */
    uint64_t frq = read_cntfrq();
    uint64_t t0 = timer_ticks_a64();
    spin_cycles(frq / 2);
    uint64_t seen = timer_ticks_a64() - t0;

    pl011_puts("[timer] ");
    if (seen >= 10 && seen <= 90) {
        pl011_puts("PASS: ");
        pl011_putdec64(seen);
        pl011_puts(" ticks observed at 100 Hz (virtual timer, INTID 27)\n");
    } else {
        pl011_puts("FAIL: ");
        pl011_putdec64(seen);
        pl011_puts(" ticks in half a second -- timer dead or runaway\n");
    }

    /* Gate 3: the GIC bookkeeping those ticks imply. */
    pl011_puts("[gic]  ");
    if (gic_completions() >= seen && seen > 0) {
        pl011_puts("PASS: claim/complete round-trip (");
        pl011_putdec64(gic_completions());
        pl011_puts(" completions)\n");
    } else {
        pl011_puts("FAIL: completions did not track ticks\n");
    }

    /* Gate 4: the jitter pool fed from those traps (the N0 shape). */
    pl011_puts("[rng]  jitter events collected: ");
    pl011_putdec64(trap_jitter_events_a64());
    pl011_puts("\n");
}

/* ---- A3: memory ------------------------------------------------------------ */

static void a3_bringup(void)
{
    /* PMM first (paging's table_alloc feeds from it), then the final
     * tables, then the heap on top of both -- the V3 order. */
    pmm_a64_init(&boot_info);
    if (pmm_a64_selftest() == 0)
        pl011_puts("[pmm]  PASS: 64 distinct frames out and back, count restored\n");
    else
        pl011_puts("[pmm]  FAIL: frame allocator self-test\n");

    paging_a64_init();
    if (paging_a64_selftest() == 0)
        pl011_puts("[vmm]  PASS: positive map cycle + three fault probes\n");
    else
        pl011_puts("[vmm]  FAIL: paging self-test\n");

    /* The alignment probe's second world: the final tables attribute
     * RAM Normal WB, so the load that faulted in a2_bringup must now
     * SUCCEED -- the mirror gate.  (probe() returns -1 on survival,
     * which is the PASS here.) */
    if (trap_alignment_probe_a64() != 0)
        pl011_puts("[vmm]  PASS: the same unaligned load SUCCEEDS on Normal WB (both polarities measured)\n");
    else
        pl011_puts("[vmm]  FAIL: unaligned load still faults on Normal memory\n");

    kheap_a64_init();
    if (kheap_a64_selftest() == 0)
        pl011_puts("[heap] PASS: 64 alloc/free cycles, patterns intact, zero leak\n");
    else
        pl011_puts("[heap] FAIL: heap self-test\n");
}

/* ---- A4: threads, scheduler, EL0 -------------------------------------------

   The sched gate is thread32's / V4's: two workers that NEVER yield,
   each spinning until it has seen 3 preemptions' worth of progress
   from the other -- possible only if the timer trap forcibly
   switches between them.  The farewell-tail lesson transfers with
   the shape: each worker banks a surplus after its own wait, so the
   other's condition is payable regardless of who exits first (the
   deadlock the rv port measured on its first cut). */

static volatile uint64_t worker_count[2];

static void sched_worker(void *arg)
{
    int me = (int)(uint64_t)arg;
    int other = 1 - me;
    uint64_t seen_start = worker_count[other];
    while (worker_count[other] < seen_start + 3)
        worker_count[me]++;             /* no yield anywhere in here */
    for (int i = 0; i < 1000; i++)
        worker_count[me]++;             /* the farewell surplus */
}

/* The FPU gate (the M1 regression test, fourth edition): a worker
 * parks a distinctive pattern in q8/q9 (callee-saved by AAPCS64 --
 * exactly the registers a lazy switch would lose), yields through
 * many preemptions while the OTHER worker clobbers the same
 * registers, then checks the pattern survived.  Handwritten asm:
 * the kernel is -mgeneral-regs-only, so no compiler-generated FPU
 * code exists to do this accidentally -- which is the point. */

static volatile int fpu_result = -1;
static volatile int fpu_done;
static volatile int fpu_clobber_stop;

static void fpu_keeper(void *arg)
{
    (void)arg;
    uint64_t in0 = 0xA110CA7EDEADBEEFUL, in1 = 0x5EED5EED5EED5EEDUL;
    uint64_t out0 = 0, out1 = 0;

    /* .arch_extension: the file is compiled -mgeneral-regs-only (the
     * COMPILER must not touch q-registers), but this probe exists
     * precisely to touch them by hand -- the directive opens the
     * assembler's gate without loosening the compiler's. */
    __asm__ volatile(".arch_extension fp\n\t"
                     ".arch_extension simd\n\t"
                     "mov v8.d[0], %0\n\t"
                     "mov v8.d[1], %1\n\t"
                     "mov v9.d[0], %1\n\t"
                     "mov v9.d[1], %0" :: "r"(in0), "r"(in1));

    /* Sit through ~20 ticks; the clobberer runs meanwhile. */
    uint64_t until = a64_cntvct() + (62500000 / 5);
    while (a64_cntvct() < until)
        __asm__ volatile("yield");

    __asm__ volatile(".arch_extension fp\n\t"
                     ".arch_extension simd\n\t"
                     "mov %0, v8.d[0]\n\t"
                     "mov %1, v9.d[1]" : "=r"(out0), "=r"(out1));
    fpu_result = (out0 == in0 && out1 == in0) ? 0 : -1;
    fpu_clobber_stop = 1;
    fpu_done = 1;
}

static void fpu_clobberer(void *arg)
{
    (void)arg;
    while (!fpu_clobber_stop) {
        __asm__ volatile(".arch_extension fp\n\t"
                         ".arch_extension simd\n\t"
                         "movi v8.16b, #0x55\n\t"
                         "movi v9.16b, #0xAA");
        for (volatile int i = 0; i < 100; i++)
            ;
    }
}

static void a4_bringup(void)
{
    sched_a64_init();

    /* Gate 1: forced preemption. */
    worker_count[0] = worker_count[1] = 0;
    int t1 = thread_a64_create("worker-a", sched_worker, (void *)0);
    int t2 = thread_a64_create("worker-b", sched_worker, (void *)1);
    pl011_puts("[sched] self-test: two never-yielding workers...\n");

    uint64_t deadline = a64_cntvct() + 2 * read_cntfrq();
    while ((thread_a64_state(t1) != THREAD_A64_STATE_DONE ||
            thread_a64_state(t2) != THREAD_A64_STATE_DONE) &&
           a64_cntvct() < deadline)
        __asm__ volatile("wfi");

    if (thread_a64_state(t1) == THREAD_A64_STATE_DONE &&
        thread_a64_state(t2) == THREAD_A64_STATE_DONE)
        pl011_puts("[sched] PASS: two never-yielding workers both finished "
                   "(timer preemption is real)\n");
    else
        pl011_puts("[sched] FAIL: a worker starved -- preemption dead\n");
    thread_a64_reap();

    /* Gate 2: FPU state across preemptive switches (M1, 4th ed.). */
    fpu_result = -1; fpu_done = 0; fpu_clobber_stop = 0;
    int tk = thread_a64_create("fpu-keeper",  fpu_keeper, 0);
    int tc = thread_a64_create("fpu-clobber", fpu_clobberer, 0);
    pl011_puts("[fpu]  keeper parks q8/q9, clobberer overwrites, "
               "20 ticks of preemption...\n");
    deadline = a64_cntvct() + 2 * read_cntfrq();
    while (!fpu_done && a64_cntvct() < deadline)
        __asm__ volatile("wfi");
    if (fpu_done && fpu_result == 0)
        pl011_puts("[fpu]  PASS: q8/q9 survived preemptive clobbering "
                   "(eager save earns its 528 bytes)\n");
    else
        pl011_puts("[fpu]  FAIL: FPU state lost across switches\n");
    (void)tk; (void)tc;
    thread_a64_reap();

    /* Gate 3: EL0 round trip + the privileged negative control. */
    if (user_a64_selftest() == 0)
        pl011_puts("[user] PASS: EL0 entered, syscalls served, exit "
                   "round-tripped, privilege contained\n");
    else
        pl011_puts("[user] FAIL: EL0 self-test\n");
}

void kmain_a64(uint64_t x0_at_entry)
{
    pl011_puts("\nHello from AuraLite OS kernel (aarch64)!\n");
    pl011_puts("[kernel] AuraLite OS aarch64, ARM64_PLAN phase A1\n");

    pl011_puts("[boot] CurrentEL: EL");
    pl011_putdec64(read_currentel());
    pl011_putc('\n');

    /* A5a: two boot paths, one kernel.  Image boots deliver the DTB in
     * x0 (the promise A1 measured as Image-only is load-bearing now);
     * ELF boots deliver x0 = 0 and the DTB parked at the RAM base.
     * VERIFIED either way -- x0 is trusted only after its magic reads
     * back, and the ELF-path pins stay exactly as A0/A1 wrote them. */
    uint64_t dtb_phys;
    if (x0_at_entry != 0 && fdt_magic_at(x0_at_entry) == FDT_MAGIC_BE) {
        dtb_phys = x0_at_entry;
        pl011_puts("[boot] x0 at entry: ");
        pl011_puthex64(x0_at_entry);
        pl011_puts(" (DTB pointer, Image path -- magic verified)\n");
        pl011_puts("[boot] DTB source: x0 (Image boot protocol)\n");
    } else {
        dtb_phys = RAM_BASE;
        /* A0 Fact 2.2 echo, first half: x0 is NOT the DTB pointer for
         * ELF payloads.  Print what arrived; the smoke asserts 0. */
        pl011_puts("[boot] x0 at entry: ");
        pl011_puthex64(x0_at_entry);
        pl011_puts(" (not the DTB pointer for ELF payloads -- measured, plan Fact 2)\n");

        /* Second half: the DTB is parked at the RAM base. */
        uint32_t magic = fdt_magic_at_ram_base();

        pl011_puts("[boot] DTB probe at RAM base ");
        pl011_puthex64(RAM_BASE);
        pl011_puts(": magic ");
        pl011_puthex64(magic);
        if (magic == FDT_MAGIC_BE) {
            pl011_puts(" OK (big-endian read)\n");
        } else {
            pl011_puts(" MISMATCH; refusing to continue (expected 0xD00DFEED)\n");
            psci_system_off();
        }
    }

    /* A0 Fact 2.3 echo: the timer frequency is a register. */
    pl011_puts("[boot] CNTFRQ_EL0: ");
    pl011_putdec64(read_cntfrq());
    pl011_puts(" Hz\n");

    /* The A1 shim: DTB -> boot_info_t, through the SHARED walker.
     * Errors are named, not numbered -- the V0/V1 tradition. */
    int rc = fdt_parse(dtb_phys, 0 /* boot cpu; MPIDR affinity 0 */,
                       &boot_info, &platform);
    if (rc != 0) {
        pl011_puts("[boot] FDT parse FAILED: ");
        switch (rc) {
        case FDT_ERR_MAGIC:     pl011_puts("bad magic\n");            break;
        case FDT_ERR_VERSION:   pl011_puts("incompatible version\n"); break;
        case FDT_ERR_BOUNDS:    pl011_puts("offset out of bounds\n"); break;
        case FDT_ERR_TRUNCATED: pl011_puts("truncated stream\n");     break;
        default:                pl011_puts("unknown error\n");        break;
        }
        pl011_puts("[boot] cannot build boot_info; shutting down\n");
        psci_system_off();
    }

    if (boot_info_check() != 0)
        psci_system_off();

    memmap_report();
    platform_report();

    /* a2 first: the vector table must exist before a3's fault probes
     * arm (a W^X probe with VBAR unset is a hang, not a test --
     * measured on the first ordering attempt).  The timer keeps
     * ticking straight through the table switch: the HHDM never
     * moves, .text stays RX, and the GIC windows stay Device. */
    a2_bringup();
    a3_bringup();
    a4_bringup();

    /* ---- A7: the virt machine's real devices. ------------------------
     * Absence is a SKIP, not a FAIL: the boot smoke runs without
     * -drive/-netdev and must stay green; the driver gates get their
     * devices from a64_drivers_smoke's richer QEMU line (main_rv.c's
     * V7 stanza, fourth arch).  The console goes interrupt-fed both
     * ways first: RX through the GIC (INTID from A1's normalisation),
     * TX through the O3 ring core [AMEND-3]. */

    if (platform.uart_base && platform.uart_irq)
        pl011_rx_init(platform.uart_irq);
    pl011_tx_ring_enable();

    /* HW_PLAN H0: bench the LINKED string ops (kernel/lib/string.c,
     * [AMEND-2]'s adoptee) -- the byte-loop baseline H1 must beat,
     * on this tenant's own clock (CNTFRQ is a register, Fact 2.3). */
    membench_a64_run(read_cntfrq());

    if (vblk_a64_init(&platform) == 0) {
        /* Two kinds of media (the P2 dispatch, mirrored): the parity
         * smokes' pattern disk keeps its A7 selftest gate verbatim;
         * anything else is filesystem media and goes behind the
         * blkdev seam. */
        static uint8_t sec0[512];
        int is_pattern = (vblk_a64_read(0, sec0) == 0 &&
                          sec0[0] == 'A' && sec0[1] == 'u' &&
                          sec0[2] == 'r' && sec0[3] == 'a');
        if (is_pattern) {
            if (vblk_a64_selftest() != 0) {
                pl011_puts("[blk]  FAIL: self-test\n");
                psci_system_off();
            }
        } else {
            pl011_puts("[blk]  sector 0: no test pattern; filesystem media\n");
            a64fs_bringup();
        }
    }

    if (vnet_a64_init(&platform) == 0) {
        if (vnet_a64_selftest() != 0) {
            pl011_puts("[net]  FAIL: self-test\n");
            psci_system_off();
        }
    }

    /* ---- A5c: the initrd and real compiled userspace (the fourth
     * tenant).  Only Image boots carry an initrd on this board (the
     * A5a fact); an ELF boot reports the absence honestly and keeps
     * the A4 ending. ---- */
    if (initrd_a64_init(&boot_info) == 0) {
        pl011_puts("[boot] starting init (EL0, ELF64 from the initrd)\n");
        /* R5 (ledger RES-14): with secondaries online, init runs ON
         * ONE OF THEM.  Single-core boots keep the old path. */
        int r5_core = -1;
        int code = smp_a64_run_init_on_secondary(&r5_core);
        if (code == -1000) {
            code = user_a64_run_elf("bina64/init");
        } else if (code == -1001) {
            pl011_puts("[smp] R5 FAIL: secondary never took the job\n");
            code = user_a64_run_elf("bina64/init");
        } else {
            kprintf("[smp] init ran at EL0 ON CORE %d "
                    "(R5: user code off the boot core)\n", r5_core);
        }
        if (code == 7)
            pl011_puts("[init] PASS: inita64 ran and exited 7 as built\n");
        else {
            pl011_puts("[init] FAIL: inita64 exit=");
            pl011_putdec64((uint64_t)code);
            pl011_puts(" (want 7)\n");
        }

        /* The auralite# gate, fourth arch: the PROMOTED shell. */
        pl011_puts("[boot] starting shell (EL0, the shared smallsh)\n");
        int sh = user_a64_run_elf("bina64/smallsh");
        pl011_puts("[shell] exited ");
        pl011_putdec64((uint64_t)sh);
        pl011_puts("\n");

        /* A7's receipt: every keystroke the session typed arrived
         * through the GIC path, and this counter is the proof the
         * smoke test greps (a poll-fed session would leave it 0).
         * Lost-edge recoveries are printed beside it, not folded in
         * -- a lossy host stays visible. */
        pl011_puts("[uart] rx bytes via GIC irq: ");
        pl011_putdec64(pl011_rx_count());
        pl011_puts(" (+");
        pl011_putdec64(pl011_rx_polled_count());
        pl011_puts(" polled recoveries)\n");

        pl011_puts("[kernel] A7 complete; console+shell+blk+net online; "
                   "powering off via PSCI\n");
        psci_system_off();
    }

    pl011_puts("[kernel] A4 complete; powering off via PSCI "
               "(A5 adds libca64, init, the shared shell)\n");
    psci_system_off();
}
