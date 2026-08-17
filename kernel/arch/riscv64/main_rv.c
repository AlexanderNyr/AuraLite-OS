/* kernel/arch/riscv64/main_rv.c -- rv64 kernel entry (RISCV_PLAN V3).
 *
 * V0 proved the chain (clang -> lld -> OpenSBI -> _start -> SBI
 * console); V1 makes this kernel the third CONSUMER of boot_info_t.
 * On x86 a loader fills the struct before the kernel runs; here the
 * kernel's own FDT shim fills it from the DTB in a1, and kmain_rv
 * then reads it back through the same contract kmain and kmain32
 * use -- magic first, trust nothing before it checks.
 *
 * The boot log deliberately rhymes with main32.c's: "handoff magic
 * OK", an mmap summary, an initrd line.  Three kernels, one shape --
 * a person reading any serial log knows where they are.
 */

#include <stdint.h>

#include "boot/shared/boot_info.h"
#include "kernel/arch/riscv64/fdt.h"
#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/plic.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"

/* The struct the FDT shim fills.  Static in .bss (boot.S zeroed it):
 * ~9 KiB is too big for the V0 stack and there is no allocator yet. */
static boot_info_t    boot_info;
static fdt_platform_t platform;

/* ---- tiny formatting (kprintf32's opening subset; the shared
 * kprintf arrives with the V6 sweep's console work) ---- */

static void put_hex64(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

static void put_udec(uint64_t v)
{
    char buf[20];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i--)
        sbi_putc(buf[i]);
}

/* ---- boot_info consumption (main32.c's shape) --------------------------- */

static int boot_info_check(void)
{
    if (boot_info.magic != BOOT_MAGIC) {
        sbi_puts("[boot] boot_info magic BAD; halting\n");
        return -1;
    }
    sbi_puts("[boot] handoff magic OK, path=SBI, boot_info filled from DTB\n");

    sbi_puts("[mm]   HHDM offset: ");
    put_hex64(boot_info.hhdm_offset);
    sbi_puts(" (Sv39 direct map -- live since boot.S's early tables)\n");
    return 0;
}

static void memmap_report(void)
{
    uint64_t usable = 0;

    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const boot_mmap_entry_t *e = &boot_info.mmap[i];
        const char *tag = "other   ";
        switch (e->type) {
        case BOOT_MEM_USABLE:     tag = "usable  "; usable += e->length; break;
        case BOOT_MEM_RESERVED:   tag = "reserved"; break;
        case BOOT_MEM_KERNEL:     tag = "kernel  "; break;
        case BOOT_MEM_BOOTLOADER: tag = "loader  "; break;
        }
        sbi_puts("[mm]   ");
        put_hex64(e->base);
        sbi_puts(" + ");
        put_hex64(e->length);
        sbi_puts("  ");
        sbi_puts(tag);
        sbi_puts("\n");
    }

    sbi_puts("[mm]   mmap entries: ");
    put_udec(boot_info.mmap_count);
    sbi_puts(", usable RAM: ");
    put_udec(usable / (1024 * 1024));
    sbi_puts(" MiB\n");

    if (boot_info.initrd_phys) {
        sbi_puts("[mm]   initrd: ");
        put_udec(boot_info.initrd_size);
        sbi_puts(" bytes at phys ");
        put_hex64(boot_info.initrd_phys);
        sbi_puts("\n");
    } else {
        sbi_puts("[mm]   initrd: none\n");
    }
}

static void platform_report(void)
{
    sbi_puts("[hw]   harts: ");
    put_udec(boot_info.cpu_count);
    sbi_puts(" (boot hart ");
    put_udec(boot_info.bsp_lapic_id);
    sbi_puts(")\n[hw]   uart: ");
    put_hex64(platform.uart_base);
    sbi_puts("\n[hw]   plic: ");
    put_hex64(platform.plic_base);
    sbi_puts("\n[hw]   virtio-mmio windows: ");
    put_udec(platform.virtio_count);
    sbi_puts("\n");
    if (platform.bootargs) {
        sbi_puts("[hw]   bootargs: ");
        sbi_puts(platform.bootargs);
        sbi_puts("\n");
    }
}

/* ---- V2: traps, timer, PLIC --------------------------------------------- */

/* The UART line's first proof.  V7 owns the real 16550 driver; V2
 * only needs the PLIC path shown live with a genuine device
 * interrupt.  The 16550's THRE interrupt obliges: IER bit 1 set while
 * the transmit holding register is empty -> the line fires
 * immediately, no one has to type.  The handler reads IIR (clears the
 * THRE cause -- a level-triggered line left uncleared would claim
 * forever) and counts. */
#define UART_IER 1   /* interrupt enable */
#define UART_IIR 2   /* interrupt identification (read clears THRE) */

static volatile uint8_t *uart_mmio;
static volatile uint64_t uart_irqs;

static void uart_irq_probe(uint32_t irq)
{
    (void)irq;
    if (uart_mmio) {
        (void)uart_mmio[UART_IIR];      /* acknowledge THRE */
        uart_mmio[UART_IER] = 0;        /* one proof is enough */
    }
    uart_irqs++;
}

static void v2_bringup(void)
{
    trap_set_hartid(boot_info.bsp_lapic_id);
    trap_init(platform.timebase_freq);
    sbi_puts("[isr]  stvec installed, SIE.STIE+SEIE on, timer armed (100 Hz)\n");

    /* [isr] gate: a deliberate illegal instruction, named + resumed. */
    if (trap_selftest() == 0) {
        sbi_puts("[isr]  PASS: illegal instruction named and resumed past\n");
    } else {
        sbi_puts("[isr]  FAIL: self-test fault did not round-trip\n");
        sbi_shutdown();
    }

    /* [timer] gate: watch ticks advance.  100 Hz means 5 ticks in
     * 50 ms; spin on rdtime so the bound is wall-clock, not luck. */
    {
        uint64_t start_ticks = timer_ticks();
        uint64_t tb = platform.timebase_freq ? platform.timebase_freq
                                             : 10000000;
        uint64_t deadline = rv_rdtime() + tb / 4;      /* 250 ms cap */
        while (timer_ticks() < start_ticks + 5 &&
               rv_rdtime() < deadline)
            __asm__ volatile("wfi");

        if (timer_ticks() >= start_ticks + 5) {
            sbi_puts("[timer] PASS: ");
            put_udec(timer_ticks() - start_ticks);
            sbi_puts(" ticks observed at 100 Hz\n");
        } else {
            sbi_puts("[timer] FAIL: no ticks within 250 ms\n");
            sbi_shutdown();
        }
    }

    /* Jitter pool: collection side of the N0 fallback path, fed by
     * the timer trap's rdtime deltas.  Consumed by the DRBG when the
     * shared rng joins the build (V8) -- counted, not oversold. */
    sbi_puts("[rng]  jitter events collected: ");
    put_udec(trap_jitter_events());
    sbi_puts(" (pool feeds from timer traps; DRBG consumes in V8)\n");

    /* PLIC: threshold + enable for the UART line from the DTB. */
    if (platform.plic_base) {
        /* MMIO through the HHDM: physical bases from the DTB, plus
         * the direct-map offset (V3 -- there is no identity map). */
        plic_init((uint64_t)p2v_rv(platform.plic_base),
                  boot_info.bsp_lapic_id);
        plic_enable(platform.uart_irq, uart_irq_probe);
        sbi_puts("[plic] S-context enabled, threshold 0, uart irq ");
        put_udec(platform.uart_irq);
        sbi_puts(" wired\n");

        /* Claim/complete round-trip with a REAL device interrupt:
         * the 16550's transmitter is idle, so enabling the THRE
         * interrupt (IER bit 1) raises the line right now.  Wait a
         * few ticks; the handler acks and disables itself. */
        if (platform.uart_base) {
            uart_mmio = (volatile uint8_t *)p2v_rv(platform.uart_base);
            uart_mmio[UART_IER] = 0x02;             /* THRE on */
            uint64_t tb = platform.timebase_freq ? platform.timebase_freq
                                                 : 10000000;
            uint64_t deadline = rv_rdtime() + tb / 10;  /* 100 ms cap */
            while (plic_completions() == 0 && rv_rdtime() < deadline)
                __asm__ volatile("wfi");
        }

        if (plic_completions() > 0) {
            sbi_puts("[plic] PASS: claim/complete round-trip, ");
            put_udec(plic_completions());
            sbi_puts(" completion(s), uart line fired ");
            put_udec(uart_irqs);
            sbi_puts(" time(s)\n");
        } else {
            sbi_puts("[plic] FAIL: no claim within 100 ms of THRE enable\n");
            sbi_shutdown();
        }
    } else {
        sbi_puts("[plic] not found in DTB -- external interrupts unavailable\n");
    }
}

/* ---- entry -------------------------------------------------------------- */

void kmain_rv(uint64_t hartid, uint64_t dtb_phys)
{
    sbi_console_init();

    sbi_puts("\n==============================================\n"
             " Hello from AuraLite OS kernel (riscv64)!\n"
             "  rv64gc S-mode, booted via OpenSBI\n"
             "==============================================\n\n");
    sbi_puts("[kernel] AuraLite OS riscv64, RISCV_PLAN phase V3\n");

    sbi_puts("[boot] boot hart: ");
    put_udec(hartid);
    sbi_puts("\n[boot] DTB at phys ");
    put_hex64(dtb_phys);
    sbi_puts("\n");

    /* The V1 shim: DTB -> boot_info_t.  Errors are named, not
     * numbered -- a silent boot was this port's V0 failure mode and
     * once was enough. */
    int rc = dtb_phys ? fdt_parse(dtb_phys, hartid, &boot_info, &platform)
                      : FDT_ERR_MAGIC;
    if (rc != 0) {
        sbi_puts("[boot] FDT parse FAILED: ");
        switch (rc) {
        case FDT_ERR_MAGIC:     sbi_puts("bad magic\n");            break;
        case FDT_ERR_VERSION:   sbi_puts("incompatible version\n"); break;
        case FDT_ERR_BOUNDS:    sbi_puts("offset out of bounds\n"); break;
        case FDT_ERR_TRUNCATED: sbi_puts("truncated stream\n");     break;
        default:                sbi_puts("unknown error\n");        break;
        }
        sbi_puts("[boot] cannot build boot_info; shutting down\n");
        sbi_shutdown();
    }
    sbi_puts("[boot] DTB magic OK (0xD00DFEED, big-endian read)\n");

    /* From here on: only boot_info, never the raw DTB pointer -- the
     * same discipline the x86 kernels keep toward their loaders. */
    if (boot_info_check() != 0)
        sbi_shutdown();
    memmap_report();
    platform_report();

    v2_bringup();

    /* ---- V3: PMM -> final Sv39 tables -> heap, each gated. ---------- */

    pmm_rv_init(&boot_info);
    if (pmm_rv_selftest() == 0) {
        sbi_puts("[pmm]  PASS: 64 frames out and back, count restored\n");
    } else {
        sbi_puts("[pmm]  FAIL: self-test\n");
        sbi_shutdown();
    }

    paging_rv_init();
    if (paging_rv_selftest() == 0) {
        sbi_puts("[vmm]  PASS: positive path + 3 fault probes "
                 "(W^X write, W^X exec, identity drop)\n");
    } else {
        sbi_puts("[vmm]  FAIL: self-test\n");
        sbi_shutdown();
    }

    kheap_rv_init();
    if (kheap_rv_selftest() == 0) {
        sbi_puts("[heap] PASS: 64 cycles, no corruption, no leak\n");
    } else {
        sbi_puts("[heap] FAIL: self-test\n");
        sbi_shutdown();
    }

    sbi_puts("[kernel] V3 complete; shutting down "
             "(V4 adds threads, the scheduler, U-mode and ecall)\n");
    sbi_shutdown();
}
