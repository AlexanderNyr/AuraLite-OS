/* kernel/arch/i386/main32.c -- i386 kernel entry (I386_PLAN I2 + I3).
 *
 * I2 brought up the CPU (GDT/TSS, IDT, PIC, PIT, named exceptions).
 * I3 adds memory: boot32.asm now enables PSE paging before this code
 * runs (the kernel is higher-half at 0xC0100000), and kmain32 owns the
 * rest -- dropping the boot identity window, the PMM over the E820
 * map, 4 KiB page mappings, and the on-demand heap.  The init order
 * tracks kernel/kernel.c's:
 *   uart -> banner -> boot_info -> gdt -> idt -> pic -> sti -> pit ->
 *   vmm -> pmm -> heap -> self-tests -> idle.
 *
 * Still compiled -malign-double: this file reads boot_info_t, and the
 * I1 canary (mmap_count) keeps watching the cross-width layout.
 */

#include <stdint.h>

#include "boot/shared/boot_info.h"
#include "kernel/arch/i386/gdt.h"
#include "kernel/arch/i386/idt.h"
#include "kernel/arch/i386/irq32.h"
#include "kernel/arch/i386/isr.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kheap32.h"
#include "kernel/arch/i386/thread32.h"
#include "kernel/arch/i386/user32.h"
#include "kernel/arch/i386/initrd32.h"
#include "kernel/arch/i386/vga32.h"
#include "kernel/arch/i386/kbd32.h"
#include "kernel/arch/i386/ata32.h"
#include "kernel/arch/i386/fsglue32.h"
#include "kernel/arch/i386/net32.h"
#include "kernel/arch/i386/netglue32.h"

void thread32_reap(void);

/* isr32.c's deliberate-fault hooks. */
void isr32_expect_breakpoint(void);
int  isr32_breakpoint_seen(void);

static const boot_info_t *boot_info;

/* ---- boot_info -------------------------------------------------------- */

static int boot_info_check(uint32_t phys)
{
    /* Paging is ON since boot32.asm (I3): reach the struct through the
     * direct map, not the physical pointer -- the identity window is
     * about to be dropped. */
    boot_info = (const boot_info_t *)p2v_32(phys);

    if (boot_info->magic != BOOT_MAGIC) {
        kprintf32("[boot]  boot_info magic BAD (hi=%x lo=%x); halting\n",
                  (uint32_t)(boot_info->magic >> 32),
                  (uint32_t)boot_info->magic);
        return -1;
    }

    kprintf32("[boot]  handoff magic OK, path=%s, boot_info at phys %x\n",
              boot_info->boot_from_uefi ? "UEFI" : "BIOS", phys);

    /* The loader owns hhdm_offset; the kernel checks rather than
     * assumes (the 64-bit path prints its 0xffff800000000000 the same
     * way).  Stage 2's 32-bit branch writes 0xC0000000. */
    if (boot_info->hhdm_offset != KERNEL_VBASE_32) {
        kprintf32("[boot]  hhdm_offset=%x%x does not match the direct map "
                  "(%x); halting\n",
                  (uint32_t)(boot_info->hhdm_offset >> 32),
                  (uint32_t)boot_info->hhdm_offset, KERNEL_VBASE_32);
        return -1;
    }
    kprintf32("[mm]    HHDM offset: %x (direct map, %u MiB)\n",
              KERNEL_VBASE_32, DIRECT_MAP_BYTES / (1024 * 1024));
    return 0;
}

static void memmap_report(void)
{
    uint32_t usable_kib = 0;

    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        const boot_mmap_entry_t *e = &boot_info->mmap[i];
        if (e->type == BOOT_MEM_USABLE) {
            uint64_t end = e->base + e->length;
            if (e->base >= 0x100000000ULL)
                continue;
            if (end > 0x100000000ULL)
                end = 0x100000000ULL;
            usable_kib += (uint32_t)((end - e->base) / 1024);
        }
    }

    kprintf32("[mm]    E820 entries: %u, usable below 4 GiB: %u KiB (%u MiB)\n",
              boot_info->mmap_count, usable_kib, usable_kib / 1024);
    kprintf32("[mm]    initrd: %x bytes at phys %x\n",
              (uint32_t)boot_info->initrd_size,
              (uint32_t)boot_info->initrd_phys);
}

/* ---- boot self-tests --------------------------------------------------- */

static int selftest_breakpoint(void)
{
    isr32_expect_breakpoint();
    __asm__ volatile("int3");
    return isr32_breakpoint_seen() ? 0 : -1;
}

static int selftest_pit(void)
{
    uint32_t start = pit32_ticks();
    for (volatile uint32_t spin = 0; spin < 50000000u; spin++) {
        if (pit32_ticks() - start >= 3)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* ---- I4: scheduler interleave self-test --------------------------------
 * Same contract as the x86_64 [sched] PASS: two threads increment their
 * counters concurrently under PIT preemption; both must make progress
 * WITHOUT either ever calling yield -- that is what distinguishes
 * preemption from cooperative interleaving. */
static volatile uint32_t st_count[2];

static void sched_worker(void *arg)
{
    volatile uint32_t *ctr = (volatile uint32_t *)arg;
    /* Run for ~15 ticks of wall time, pure computation. */
    uint32_t start = pit32_ticks();
    while (pit32_ticks() - start < 15)
        (*ctr)++;
}

static int selftest_sched(void)
{
    st_count[0] = st_count[1] = 0;

    int t1 = thread32_create("st-worker-1", sched_worker, (void *)&st_count[0]);
    int t2 = thread32_create("st-worker-2", sched_worker, (void *)&st_count[1]);
    if (t1 < 0 || t2 < 0)
        return -1;

    /* Wait for both to finish; the boot thread hlt-waits, so all
     * progress the workers make is preemption-driven. */
    uint32_t deadline = pit32_ticks() + 400;
    while ((thread32_state(t1) == THREAD32_STATE_READY ||
            thread32_state(t2) == THREAD32_STATE_READY)) {
        if (pit32_ticks() > deadline)
            return -1;
        __asm__ volatile("hlt");
    }
    thread32_reap();

    kprintf32("[sched] worker counts: %u / %u\n", st_count[0], st_count[1]);
    /* Both made real progress -> the PIT actually preempted. */
    if (st_count[0] < 1000 || st_count[1] < 1000)
        return -1;
    return 0;
}

/* ---- entry ------------------------------------------------------------- */

void kmain32(uint32_t boot_info_phys)
{
    uart32_init();
    kprintf32("\n[boot] UART (COM1) initialised @ 115200 baud\n");

    kputs32("\n==============================================\n"
            " Hello from AuraLite OS kernel (i386)!\n"
            "  x86 protected mode, higher half at 0xC0100000\n"
            "==============================================\n\n");
    kprintf32("[kernel] AuraLite OS i386, I386_PLAN phase I3\n");

    if (boot_info_check(boot_info_phys) != 0)
        goto halt;
    memmap_report();

    gdt_init();
    kprintf32("[boot] GDT loaded (kernel + user segments + 32-bit TSS)\n");

    idt_init();
    kprintf32("[boot] IDT installed: 256 gates\n");

    pic32_init();
    kprintf32("[boot] PIC remapped (IRQs -> vectors 32-47), all masked\n");

    __asm__ volatile("sti");
    kprintf32("[kernel] interrupts enabled, exception handling online.\n");

    pit32_init(100);

    /* ---- I3: memory ---- */
    paging32_init();
    paging32_drop_identity();

    kprintf32("[boot] initialising physical memory manager...\n");
    pmm32_init(boot_info);
    if (pmm32_selftest() != 0) {
        kprintf32("[pmm] FAIL: self-test\n");
        goto halt;
    }

    if (paging32_selftest() != 0) {
        kprintf32("[vmm] FAIL: self-test\n");
        goto halt;
    }

    kprintf32("[boot] initialising kernel heap...\n");
    kheap32_init();
    if (kheap32_selftest() != 0) {
        kprintf32("[heap] FAIL: self-test\n");
        goto halt;
    }

    /* ---- I7: the console grows a screen and a keyboard ---- */
    vga32_init();
    kprintf32("[boot] VGA text console online (80x25 at 0xB8000); "
              "kprintf fans out to UART + VGA\n");
    kbd32_init();

    /* ---- I8: storage + network (the gates that moved from I7) ---- */
    uint32_t disk_sectors = 0;
    if (ata32_init(&disk_sectors) == 0) {
        if (ata32_selftest() != 0) {
            kprintf32("[ata] FAIL: self-test\n");
            goto halt;
        }
        /* PARITY P7: drives behind the blkdev seam; the shared ext2
         * mounts on the slave when one is attached. */
        fs32_bringup();
    } else {
        kprintf32("[ata] no primary-master ATA device; skipping "
                  "(hardware without IDE: I8 residue, see plan)\n");
    }

    if (net32_init() == 0) {
        if (net32_selftest() != 0)
            kprintf32("[net] FAIL: self-test (continuing; network is "
                      "not boot-critical)\n");
        else
            /* R3: the shared TCP behind the netdev seam. */
            net32_tcp_bringup();
    }

    /* ---- I2 self-tests (still gating every boot) ---- */
    if (selftest_breakpoint() == 0)
        kprintf32("[isr] PASS: deliberate #BP named, dumped, resumed\n");
    else
        kprintf32("[isr] FAIL: #BP self-test did not round-trip\n");

    if (selftest_pit() == 0)
        kprintf32("[timer] PASS: PIT ticking (%u ticks observed)\n",
                  pit32_ticks());
    else
        kprintf32("[timer] FAIL: no PIT ticks; check PIC mask/EOI path\n");

    /* ---- I4: threads, preemption, Ring 3 ---- */
    kprintf32("[boot] initialising scheduler...\n");
    sched32_init();
    if (selftest_sched() == 0)
        kprintf32("[sched] PASS: 2 workers preempted, both progressed\n");
    else {
        kprintf32("[sched] FAIL: interleave self-test\n");
        goto halt;
    }

    syscall32_init();
    if (user32_selftest() != 0) {
        kprintf32("[user] FAIL: Ring 3 self-test\n");
        goto halt;
    }

    /* ---- I5: the initrd and a real compiled init ---- */
    if (initrd32_init(boot_info) == 0) {
        kprintf32("[boot] starting init (Ring 3, ELF32 from the initrd)\n");
        int code = user32_run_elf("bin32/init32");
        if (code == 7)
            kprintf32("[init] PASS: init32 ran and exited %d as built\n", code);
        else
            kprintf32("[init] FAIL: init32 exit=%d (want 7)\n", code);

        /* ---- I7: the interactive shell (the auralite# gate) ---- */
        kprintf32("[boot] starting shell (Ring 3)\n");
        int sh = user32_run_elf("bin32/shell32");
        kprintf32("[shell] exited %d\n", sh);
    } else {
        kprintf32("[init] SKIP: no initrd\n");
    }

    kprintf32("[kernel] I7 console+shell online; idle (I8 adds fs/net "
              "parity)\n");

halt:
    for (;;) {
        thread32_reap();
        __asm__ volatile("hlt");
    }
}
