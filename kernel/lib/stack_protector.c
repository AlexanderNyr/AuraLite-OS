#include <stdint.h>
#include "kernel/lib/stack_protector.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/bsod.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/boot_info.h"

extern void kernel_halt(void);

/* Compile-time bootstrap canary, reseeded during early boot. */
uintptr_t __stack_chk_guard = 0xA84B9C2DF13E0471ULL;

/* FIX_R2: how many times the protector has tripped.  >1 means a halted
 * machine previously kept running long enough to trip again; useful to
 * tell independent trips from one cascading corruption. */
static volatile uint32_t stack_chk_trip_count = 0;

uint32_t stack_protector_trip_count(void) {
    return stack_chk_trip_count;
}

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

/* FIX_R2 instrument: emit one labelled field line, lock-free. */
static void diag_field_str(const char *name, const char *val) {
    diag_early_puts("[diag]   ");
    diag_early_puts(name);
    diag_early_puts("=");
    diag_early_puts(val ? val : "(null)");
    diag_early_puts("\n");
}

static void diag_field_hex(const char *name, uint64_t v) {
    diag_early_puts("[diag]   ");
    diag_early_puts(name);
    diag_early_puts("=");
    diag_early_puthex(v);
    diag_early_puts("\n");
}

/* R2 needs this function never to be on the protected-side itself: a
 * protected __stack_chk_fail could recurse through its own epilogue. */
__attribute__((no_stack_protector, noreturn))
void __stack_chk_fail(void) {
    stack_chk_trip_count++;

    /* FIX_R0: the lock-free serial dump comes first — with a corrupted or
     * overflowed stack the kprintf path (console lock, framebuffer) is
     * exactly what may not work anymore.
     *
     * FIX_R2: a bare "STACK CORRUPTION DETECTED" cannot distinguish the two
     * failure shapes this message conflates, and they have different causes:
     *
     *   (a) GENUINE OVERFLOW — the stack grew past its end and clobbered the
     *       frame cookie from below.  With guarded stack pages the guard
     *       should fault first (or, post-FIX_R1, escalate to an IST #DF), so
     *       a trip that arrives HERE with an in-bounds stack pointer is
     *       unlikely to be this.
     *
     *   (b) CANARY VALUE MISMATCH — the in-frame cookie was overwritten by
     *       somebody with a pointer: an adjacent-buffer overrun, a stale
     *       pointer, or a racing writer on another CPU.  The stack pointer
     *       stays inside the stack.  This is the scary SMP case, and the
     *       classification below exists to recognise it.
     *
     * So the dump captures: which CPU, which thread, where (return address
     * of the protected function's epilogue call site), current rsp/rbp vs
     * the thread's kernel-stack bounds, and (heuristically) the frame's
     * stored cookie vs __stack_chk_guard.  Everything is read through
     * sched_current()-style safe accessors and bounds-checked before any
     * dereference — a trip on a truly-wild stack must not fault again
     * inside the report (that is also why all output here is lock-free). */
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    uintptr_t ret_addr  = (uintptr_t)__builtin_return_address(0);
    uintptr_t our_rbp   = (uintptr_t)__builtin_frame_address(0);

    diag_early_puts("\n[diag] === KERNEL STACK CORRUPTION DETECTED on cpu#");
    diag_early_putdec(diag_cpu_id());
    diag_early_puts(" (stack protector; trip #");
    diag_early_putdec(stack_chk_trip_count);
    diag_early_puts(") ===\n");

    diag_field_hex("fault-detected-at-rip", (uint64_t)ret_addr);
    diag_field_hex("rsp", rsp);
    diag_field_hex("rbp", (uint64_t)our_rbp);
    diag_field_hex("expected-canary(__stack_chk_guard)",
                   (uint64_t)__stack_chk_guard);

    /* Which thread, and which stack, is involved. */
    tcb_t *cur = sched_current();
    const char *class;
    if (cur) {
        diag_field_str("thread", cur->name);
        diag_field_hex("tid", cur->id);
        diag_field_hex("thread-pml4-phys", cur->pml4_phys);

        uint64_t usable = (uint64_t)(uintptr_t)cur->kernel_stack;
        uint64_t top    = usable + (uint64_t)THREAD_STACK_SIZE;
        int have_bounds = (cur->kernel_stack != 0);

        if (have_bounds) {
            diag_field_hex("kstack-usable-bottom", usable);
            diag_field_hex("kstack-top", top);

            /* Heuristic frame-cookie readback, bounded to the mapped stack:
             * with -fno-omit-frame-pointer our caller's rbp sits at
             * [our_rbp], and -fstack-protector-strong lays the cookie just
             * below the caller's saved rbp.  Both dereferences stay inside
             * [usable, top), so a corrupt chain cannot fault here. */
            if (our_rbp >= usable && our_rbp + 8 < top) {
                uint64_t caller_rbp = *(volatile uint64_t *)our_rbp;
                diag_field_hex("caller-rbp", caller_rbp);
                if (caller_rbp >= usable && caller_rbp + 8 <= top) {
                    uint64_t cookie =
                        *(volatile uint64_t *)(caller_rbp - 8);
                    diag_field_hex("frame-cookie(heuristic caller_rbp-8)",
                                   cookie);
                }
            }

            if (rsp < usable || rsp >= top) {
                class = "GENUINE-OVERFLOW -- stack pointer is OUT of the "
                        "current kernel stack; something walked off the end. "
                        "A guard #PF (or IST #DF) normally fires first; this "
                        "shape means the guard was bypassed or the fault "
                        "cascaded before delivery.";
            } else {
                class = "CANARY-VALUE-MISMATCH -- stack pointer is IN "
                        "bounds, so the frame's cookie was overwritten by "
                        "another writer (adjacent overrun, stale pointer, or "
                        "a racing CPU), not by the stack growing past its "
                        "end.";
            }
        } else {
            class = "UNKNOWN -- current thread has no recorded kernel stack "
                    "(very early in thread bring-up).";
        }
    } else {
        class = "UNKNOWN -- no current thread (scheduler not initialised on "
                "this CPU yet, or running on the boot stack).";
    }

    diag_early_puts("[diag]   class=");
    diag_early_puts(class);
    diag_early_puts("\n");

    kprintf("[security] STACK CORRUPTION DETECTED in kernel (cpu%u)\n",
            diag_cpu_id());
    bsod_show(BSOD_KCANARY, class, diag_cpu_id(), ret_addr, rsp);
    kernel_halt();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* abort() — SELFHOST_PLAN.md SH5c.
 *
 * The kernel has no libc, but the tcc runtime does call abort(): libtcc1's
 * va_list helper (linked since SH5c: tcc lowers va_arg through __va_arg)
 * ends in abort() on an unreachable argument-class default.  Freestanding
 * clang never references the symbol, so the clang build keeps its exact
 * previous content (gc-sections drops this); the tcc build needs it.
 * A kernel abort is a fatal bug report, same lane as __stack_chk_fail. */
__attribute__((noreturn, no_stack_protector))
void abort(void) {
    diag_early_puts("[diag] abort() called by the compiler runtime\n");
    bsod_show(BSOD_KEXPLICIT, "abort() in compiler runtime", diag_cpu_id(),
              (uintptr_t)__builtin_return_address(0), 0);
    kernel_halt();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
