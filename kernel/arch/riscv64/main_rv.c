/* kernel/arch/riscv64/main_rv.c -- rv64 kernel entry (RISCV_PLAN V7).
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
#include "kernel/dt/fdt.h"
#include "kernel/arch/riscv64/initrd_rv.h"
#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/plic.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/thread_rv.h"
#include "kernel/arch/riscv64/trap.h"
#include "kernel/arch/riscv64/uart_rv.h"
#include "kernel/arch/riscv64/membench_rv.h"
#include "kernel/arch/riscv64/user_rv.h"
#include "kernel/arch/riscv64/vblk_rv.h"
#include "kernel/arch/riscv64/pci_rv.h"
#include "kernel/arch/riscv64/fsglue_rv.h"
#include "kernel/arch/riscv64/smp_rv.h"
#include "kernel/arch/riscv64/vnet_rv.h"

/* The struct the FDT shim fills.  Static in .bss (boot.S zeroed it):
 * ~9 KiB is too big for the V0 stack and there is no allocator yet. */
static boot_info_t    boot_info;
static fdt_platform_t platform;

/* Contract 1 of the shared walker (kernel/dt/fdt.h, promoted in
 * ARM64_PLAN A1): how a physical DTB address becomes a pointer is
 * this arch's business.  Here: the HHDM -- boot.S turned Sv39 on
 * before any C ran, so physical pointers no longer dereference bare.
 * (This is the exact comment that sat inside fdt_parse when the
 * walker was arch-private; the promotion moved the POLICY out of the
 * shared file and left the MECHANISM here, where satp lives.) */
const void *dt_phys_to_virt(uint64_t phys)
{
    return p2v_rv(phys);
}

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

/* ---- V4: the sched gate ---------------------------------------------------
 *
 * Two workers that NEVER yield: each spins bumping its counter until
 * it has seen 3 timer preemptions' worth of progress from the OTHER
 * one -- possible only if the timer trap forcibly switches between
 * them.  Cooperative scheduling cannot pass this; that is the point
 * (thread32's gate, LP64 spelling). */

static volatile uint64_t worker_count[2];

static void sched_worker(void *arg)
{
    int me = (int)(uint64_t)arg;
    int other = 1 - me;
    uint64_t seen_start = worker_count[other];
    while (worker_count[other] < seen_start + 3)
        worker_count[me]++;             /* no yield anywhere in here */
    /* The farewell tail -- and it is load-bearing: the first cut
     * DEADLOCKED (measured: worker-a DONE, worker-b spinning forever).
     * A exits the moment it has seen B's +3, contributing ZERO further
     * increments; if B sampled its baseline near A's final value, B's
     * +3 never arrives.  Each worker therefore banks a surplus after
     * its own wait is satisfied, so the other's condition is payable
     * regardless of who exits first. */
    for (int i = 0; i < 1000; i++)
        worker_count[me]++;
}

static int sched_selftest(void)
{
    worker_count[0] = worker_count[1] = 0;
    int t1 = thread_rv_create("worker-a", sched_worker, (void *)0);
    int t2 = thread_rv_create("worker-b", sched_worker, (void *)1);
    if (t1 < 0 || t2 < 0)
        return -1;

    sbi_puts("[sched] self-test: two never-yielding workers...\n");

    /* Wait for both to finish, bounded by wall clock (2 s at the
     * timebase).  thread 0 spins on wfi -- every tick preempts it
     * too, which is itself part of the proof. */
    uint64_t tb = platform.timebase_freq ? platform.timebase_freq
                                         : 10000000;
    uint64_t deadline = rv_rdtime() + 2 * tb;
    while ((thread_rv_state(t1) != THREAD_RV_STATE_DONE ||
            thread_rv_state(t2) != THREAD_RV_STATE_DONE) &&
           rv_rdtime() < deadline)
        __asm__ volatile("wfi");

    if (thread_rv_state(t1) != THREAD_RV_STATE_DONE ||
        thread_rv_state(t2) != THREAD_RV_STATE_DONE)
        return -1;

    sbi_puts("[sched] worker-a count ");
    put_udec(worker_count[0]);
    sbi_puts(", worker-b count ");
    put_udec(worker_count[1]);
    sbi_puts(" -- both advanced under forced preemption\n");
    return (worker_count[0] > 0 && worker_count[1] > 0) ? 0 : -1;
}

/* ---- entry -------------------------------------------------------------- */

void kmain_rv(uint64_t hartid, uint64_t dtb_phys)
{
    sbi_console_init();

    sbi_puts("\n==============================================\n"
             " Hello from AuraLite OS kernel (riscv64)!\n"
             "  rv64gc S-mode, booted via OpenSBI\n"
             "==============================================\n\n");
    sbi_puts("[kernel] AuraLite OS riscv64, RISCV_PLAN phase V7\n");

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

    /* ---- V4: scheduler, then U-mode, each gated. -------------------- */

    sched_rv_init();
    if (sched_selftest() == 0) {
        sbi_puts("[sched] PASS: two never-yielding workers both ran "
                 "(preemption is real)\n");
    } else {
        sbi_puts("[sched] FAIL: self-test\n");
        sbi_shutdown();
    }
    thread_rv_reap();

    if (user_rv_selftest() == 0) {
        sbi_puts("[user] PASS: U-mode round trip + negative control\n");
    } else {
        sbi_puts("[user] FAIL: self-test\n");
        sbi_shutdown();
    }

    /* ---- V7: the virt machine's real devices. ------------------------
     * Absence is a SKIP, not a FAIL: the boot smoke runs without
     * -drive/-netdev and must stay green; the driver gates get their
     * devices from rv_shell_smoke's richer QEMU line.  V2's THRE
     * proof already ran inside v2_bringup; the RX driver now owns
     * the line for good. */

    if (platform.uart_base && platform.plic_base) {
        uart_rv_init((uint64_t)p2v_rv(platform.uart_base),
                     platform.uart_irq);
    }

    /* HW_PLAN H0: bench the LINKED string ops (kernel/lib/string.c,
     * adopted by this kernel in the same phase -- the OPT §7 residue
     * line paid).  Numbers before changes: this table is the byte-
     * loop baseline H1's word loops must beat, on this tenant's own
     * clock. */
    membench_rv_run(platform.timebase_freq ? platform.timebase_freq
                                           : 10000000);

    /* PARITY P5: secondaries up, counted, one IPI round-trip (D5:
     * receipts, not scheduling). */
    smp_rv_bringup(hartid, &boot_info);

    if (vblk_rv_init(&platform) == 0) {
        /* Two kinds of media arrive here: the parity smoke's pattern
         * disk (sector 0 starts with "Aura" -- V7's known-bytes gate)
         * and P2's filesystem images.  Sniff sector 0 and dispatch:
         * the pattern disk keeps its full selftest gate verbatim, a
         * filesystem disk goes behind the blkdev seam instead (its
         * write/readback proof is ext2's own self-test). */
        static uint8_t sec0[512];
        int is_pattern = (vblk_rv_read(0, sec0) == 0 &&
                          sec0[0] == 'A' && sec0[1] == 'u' &&
                          sec0[2] == 'r' && sec0[3] == 'a');
        if (is_pattern) {
            if (vblk_rv_selftest() != 0) {
                sbi_puts("[blk]  FAIL: self-test\n");
                sbi_shutdown();
            }
        } else {
            sbi_puts("[blk]  sector 0: no test pattern; filesystem media\n");
            /* PARITY P2: put the disk behind the blkdev seam and
             * mount the shared ext2 on it. */
            rvfs_bringup();
        }
    }

    if (vnet_rv_init(&platform) == 0) {
        if (vnet_rv_selftest() != 0) {
            sbi_puts("[net]  FAIL: self-test\n");
            sbi_shutdown();
        }
    }

    /* RESIDUE R7: the ECAM walk's receipt on every boot -- AFTER the
     * mmio probes (their legacy vrings demand contiguous frames; the
     * walk's page tables would move the cursor under them), and a
     * no-op when the vblk PCI fallback already walked. */
    pci_rv_init(&platform);

    /* ---- V5: the initrd and real compiled userspace. ---------------- */

    if (initrd_rv_init(&boot_info) == 0) {
        sbi_puts("[boot] starting init (U-mode, ELF64 from the initrd)\n");
        /* R5 (ledger RES-14): with secondaries online, init runs ON
         * ONE OF THEM -- U-mode entry, syscalls and exit off the
         * boot hart, receipt named below.  Single-hart boots keep
         * the old path bit-for-bit. */
        int r5_hart = -1;
        int code = smp_rv_run_init_on_secondary(&r5_hart);
        if (code == -1000) {
            code = user_rv_run_elf("binrv/init");
        } else if (code == -1001) {
            sbi_puts("[smp] R5 FAIL: secondary never took the job\n");
            code = user_rv_run_elf("binrv/init");
        } else {
            sbi_puts("[smp] init ran at U-mode ON HART ");
            put_udec((uint64_t)r5_hart);
            sbi_puts(" (R5: user code off the boot hart)\n");
        }
        if (code == 7)
            sbi_puts("[init] PASS: initrv ran and exited 7 as built\n");
        else {
            sbi_puts("[init] FAIL: initrv exit=");
            put_udec((uint64_t)code);
            sbi_puts(" (want 7)\n");
        }

        /* The auralite# gate, third arch: the PROMOTED shell. */
        sbi_puts("[boot] starting shell (U-mode, the shared smallsh)\n");
        int sh = user_rv_run_elf("binrv/smallsh");
        sbi_puts("[shell] exited ");
        put_udec((uint64_t)sh);
        sbi_puts("\n");

        /* V7's receipt: every keystroke the session typed arrived
         * through the PLIC path, and this counter is the proof the
         * smoke test greps (a poll-fed session would leave it 0). */
        sbi_puts("[uart] rx bytes via PLIC irq: ");
        put_udec(uart_rv_rx_count());
        sbi_puts("\n");
    } else {
        sbi_puts("[init] SKIP: no initrd (pass -initrd build/initrd.tar)\n");
    }

    sbi_puts("[kernel] V7 complete; console+shell+blk+net online; idle. "
             "Shutting down (V8 is parity)\n");
    sbi_shutdown();
}
