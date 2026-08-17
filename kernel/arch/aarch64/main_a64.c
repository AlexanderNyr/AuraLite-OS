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
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/psci.h"
#include "kernel/arch/aarch64/trap_a64.h"

#define RAM_BASE      0x40000000UL
#define FDT_MAGIC_BE  0xD00DFEEDUL

/* The structs the FDT walk fills.  Static in .bss (boot.S zeroed
 * it): ~9 KiB is too big for the boot stack -- the same note as
 * main_rv.c, because it is the same struct. */
static boot_info_t    boot_info;
static fdt_platform_t platform;

/* Contract 1 of the shared walker (kernel/dt/fdt.h): how a physical
 * DTB address becomes a pointer.  A1 runs with the MMU off, so
 * physical IS virtual; A3 re-points this at the HHDM the same way
 * riscv64's version does. */
const void *dt_phys_to_virt(uint64_t phys)
{
    return (const void *)phys;
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

/* The DTB magic, read big-endian byte-wise from the RAM base.  With
 * the MMU off this region is Device memory and the compiler must not
 * merge or widen the accesses -- byte loads are both the byte-order
 * answer and the alignment answer (A0/A1 are compiled -mstrict-align,
 * plan Fact 5.1, but byte loads owe nothing to the flag). */
static uint32_t fdt_magic_at_ram_base(void)
{
    const volatile uint8_t *p = (const volatile uint8_t *)RAM_BASE;

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

    gic_init(platform.gicd_base, platform.gicc_base);
    pl011_puts("[gic]  GICv2 up: distributor + CPU interface, INTIDs pre-normalised\n");

    trap_init_a64();
    pl011_puts("[isr]  VBAR_EL1 installed (16 slots x 128 bytes), IRQ unmasked\n");

    /* Gate 1: the deliberate fault -- named, resumed past. */
    if (trap_selftest_a64() == 0)
        pl011_puts("[isr]  PASS: undefined instruction named and resumed\n");
    else
        pl011_puts("[isr]  FAIL: self-test fault did not arrive\n");

    /* Gate 1b: the alignment world model.  MMU off => Device memory
     * => an unaligned load faults (Fact 5.1 -- the reason the whole
     * kernel is compiled -mstrict-align).  Probe it, don't trust it. */
    if (trap_alignment_probe_a64() == 0)
        pl011_puts("[isr]  PASS: unaligned load faulted (Device memory, pre-MMU -- Fact 5.1 measured)\n");
    else
        pl011_puts("[isr]  FAIL: unaligned load did NOT fault -- the strict-align premise is wrong\n");

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

void kmain_a64(uint64_t x0_at_entry)
{
    pl011_puts("\nHello from AuraLite OS kernel (aarch64)!\n");
    pl011_puts("[kernel] AuraLite OS aarch64, ARM64_PLAN phase A1\n");

    pl011_puts("[boot] CurrentEL: EL");
    pl011_putdec64(read_currentel());
    pl011_putc('\n');

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

    /* A0 Fact 2.3 echo: the timer frequency is a register. */
    pl011_puts("[boot] CNTFRQ_EL0: ");
    pl011_putdec64(read_cntfrq());
    pl011_puts(" Hz\n");

    /* The A1 shim: DTB -> boot_info_t, through the SHARED walker.
     * Errors are named, not numbered -- the V0/V1 tradition. */
    int rc = fdt_parse(RAM_BASE, 0 /* boot cpu; MPIDR affinity 0 */,
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

    a2_bringup();

    pl011_puts("[kernel] A2 complete; powering off via PSCI "
               "(A3 adds TTBR1 paging, PMM, the heap)\n");
    psci_system_off();
}
