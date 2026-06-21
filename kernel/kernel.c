/* kernel.c — C entry point (kmain), reached from boot.asm. */

#include <stdint.h>
#include "kernel/kernel.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"
#include "drivers/uart/uart.h"
#include "drivers/framebuffer/fb.h"

/* Halt the (only) CPU indefinitely with interrupts off. */
void kernel_halt(void) {
    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void kmain(void) {
    /* Order matters for diagnosability: each subsystem prints its own status,
       so if the boot stalls or triple-faults we can see exactly how far it got. */
    uart_init();
    kprintf("[boot] UART (COM1) initialised @ 115200 baud\n");

    fb_init();
    kprintf("[boot] framebuffer console initialised\n");

    gdt_init();
    kprintf("[boot] GDT loaded (flat 64-bit segments)\n");

    idt_init();
    kprintf("[boot] IDT installed: 256 gates\n");

    pic_init();
    kprintf("[boot] PIC remapped (IRQs -> vectors 32-47), all masked\n");

    __asm__ volatile ("sti");   /* interrupts on; exceptions fire regardless */

    kprintf("\n");
    kprintf("==============================================\n");
    kprintf(" Hello from AuraLite OS kernel!                     \n");
    kprintf("  x86_64 long mode, booted via Limine         \n");
    kprintf("==============================================\n");
    kprintf("\n");

    kprintf("[kernel] %s version %s\n", AURALITE_NAME, AURALITE_VERSION);
    kprintf("[kernel] build: %s %s\n", __DATE__, __TIME__);

    if (limine_base_revision_supported()) {
        kprintf("[limine] requested base revision supported\n");
    } else {
        kprintf("[limine] WARNING: requested base revision NOT supported\n");
    }

    uint64_t mem = limine_get_usable_memory();
    kprintf("[mm]    usable memory: %llu bytes (%llu KiB / %llu MiB)\n",
            (unsigned long long)mem,
            (unsigned long long)(mem / 1024ULL),
            (unsigned long long)(mem / (1024ULL * 1024ULL)));
    kprintf("[mm]    HHDM offset: 0x%016llx\n",
            (unsigned long long)limine_get_hhdm_offset());

    kprintf("\n[kernel] interrupts enabled, exception handling online.\n");

    kprintf("[boot] initialising physical memory manager...\n");
    pmm_init();
    pmm_self_test();

    kprintf("[boot] initialising virtual memory manager...\n");
    paging_init();
    paging_self_test();

    kprintf("[boot] initialising kernel heap...\n");
    kheap_init();
    kheap_self_test();

    kprintf("\n[kernel] reached end of kmain; halting.\n");
    kernel_halt();
}
