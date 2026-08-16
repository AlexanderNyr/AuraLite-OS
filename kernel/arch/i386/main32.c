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

    kprintf32("[kernel] I3 memory online; idle (I4 adds threads + Ring 3)\n");

halt:
    for (;;)
        __asm__ volatile("hlt");
}
