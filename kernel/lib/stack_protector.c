#include <stdint.h>
#include "kernel/lib/stack_protector.h"
#include "kernel/lib/kprintf.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/limine_requests.h"

extern void kernel_halt(void);

/* Compile-time bootstrap canary, reseeded during early boot. */
uintptr_t __stack_chk_guard = 0xA84B9C2DF13E0471ULL;

void stack_protector_init(void) {
    volatile uint64_t local = 0;
    uint64_t seed = read_tsc() ^ read_cr3() ^ limine_get_hhdm_offset() ^
                    (uint64_t)(uintptr_t)&local ^ (uint64_t)(uintptr_t)&__stack_chk_guard;
    if (seed == 0 || seed == 0x00000A0DFFULL) {
        seed ^= 0xD1B54A32D192ED03ULL;
    }
    __stack_chk_guard = (uintptr_t)seed;
}

__attribute__((noreturn)) void __stack_chk_fail(void) {
    kprintf("[security] STACK CORRUPTION DETECTED in kernel\n");
    kernel_halt();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
