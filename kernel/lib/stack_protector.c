#include <stdint.h>
#include "kernel/lib/stack_protector.h"
#include "kernel/lib/kprintf.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/boot_info.h"

extern void kernel_halt(void);

/* Compile-time bootstrap canary, reseeded during early boot. */
uintptr_t __stack_chk_guard = 0xA84B9C2DF13E0471ULL;

/* Prevent stack protector from corrupting its own canary: this function
 * must NOT be stack-protected, otherwise the epilogue will compare the
 * stack cookie (saved before this function runs) against the now-updated
 * __stack_chk_guard and always fail. */
__attribute__((no_stack_protector))
void stack_protector_init(void) {
    volatile uint64_t local = 0;
    uint64_t seed = read_tsc() ^ read_cr3() ^ boot_get_hhdm_offset() ^
                    (uint64_t)(uintptr_t)&local ^ (uint64_t)(uintptr_t)&__stack_chk_guard;
    if (seed == 0 || seed == 0x00000A0DFFULL) {
        seed ^= 0xD1B54A32D192ED03ULL;
    }
    __stack_chk_guard = (uintptr_t)seed;
}

__attribute__((noreturn)) void __stack_chk_fail(void) {
    /* FIX_R0: the lock-free serial line comes first — with a corrupted or
     * overflowed stack the kprintf path (console lock, framebuffer, rbp
     * unwinding for other reporters) is exactly what may not work anymore.
     * Which CPU tripped the protector is also the one fact the FIX_R2
     * investigation cannot do without, so it is part of the message. */
    diag_early_puts("\n[diag] KERNEL STACK CORRUPTION DETECTED on cpu#");
    diag_early_putdec(diag_cpu_id());
    diag_early_puts(" (stack protector)\n");
    kprintf("[security] STACK CORRUPTION DETECTED in kernel (cpu%u)\n",
            diag_cpu_id());
    kernel_halt();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
