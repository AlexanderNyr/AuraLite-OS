#ifndef AURALITE_LIB_ASSERT_H
#define AURALITE_LIB_ASSERT_H

/*
 * Kernel panic / assertion helpers. A full register dump + stack trace arrives
 * with the exception handler in Phase 2; for now PANIC prints the site and
 * halts the CPU so the failure is observable rather than a silent reboot.
 *
 * FIX_R0: every panic names the CPU it happened on (diag_cpu_id()) — under
 * SMP an unattributed panic cannot be told apart from an AP misbehaving.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/lib/bsod.h"

void kernel_halt(void) __attribute__((noreturn));

#ifndef NDEBUG
#  define ASSERT(cond)                                                       \
       do {                                                                  \
           if (!(cond)) {                                                    \
               kprintf("\n[PANIC cpu%u] %s:%d: assertion failed: %s\n",      \
                       diag_cpu_id(), __FILE__, __LINE__, #cond);            \
               bsod_show(BSOD_KASSERT, #cond, diag_cpu_id(),                 \
                         (uintptr_t)__builtin_return_address(0), 0);         \
               kernel_halt();                                                \
           }                                                                 \
       } while (0)
#else
#  define ASSERT(cond) ((void)0)
#endif

#define PANIC(fmt, ...)                                                      \
    do {                                                                     \
        kprintf("\n[PANIC cpu%u] %s:%d: " fmt "\n",                          \
                diag_cpu_id(), __FILE__, __LINE__, ##__VA_ARGS__);           \
        bsod_show(BSOD_KEXPLICIT, fmt, diag_cpu_id(),                        \
                  (uintptr_t)__builtin_return_address(0), 0);                \
        kernel_halt();                                                       \
    } while (0)

#endif /* AURALITE_LIB_ASSERT_H */
