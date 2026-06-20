/* kernel.c — C entry point (kmain), reached from boot.asm. */

#include <stdint.h>
#include "kernel/kernel.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/irq.h"
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

/*
 * Phase 2 gate test: a real divide-by-zero. Volatile operands prevent the
 * compiler from folding the division to a constant, so a genuine idiv executes
 * and raises #DE (vector 0). The IDT handler prints the register dump and the
 * kernel halts. (This self-test is removed once a scheduler exists.)
 */
static void test_exception_handling(void) {
    kprintf("[kernel] testing exception handling: dividing by zero...\n");
    /* A real divide-by-zero. Volatile operands stop the compiler folding this
       to a constant, so a genuine idiv executes and raises #DE (vector 0).
       The IDT handler prints the register dump and the kernel then halts.
       (This self-test is removed once a scheduler exists.) */
    volatile int divisor  = 0;
    volatile int dividend = 1;
    int quotient = dividend / divisor;
    kprintf("[kernel] UNREACHABLE: quotient = %d\n", quotient);   /* never runs */
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
    kprintf(" Hello from NovOS kernel!                     \n");
    kprintf("  x86_64 long mode, booted via Limine         \n");
    kprintf("==============================================\n");
    kprintf("\n");

    kprintf("[kernel] %s version %s\n", NOVOS_NAME, NOVOS_VERSION);
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

    test_exception_handling();

    kernel_halt();
}
