/* syscall.c — system call dispatch.
 *
 * Minimal Phase 8 set: just SYS_WRITE (1) so the user test can print, and
 * SYS_EXIT (60) so it can terminate. A full POSIX subset arrives in Phase 9.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"

#define SYS_WRITE 1
#define SYS_EXIT  60

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd (only stdout=1 supported), a2 = buffer, a3 = length. */
        if (a1 != 1) {
            return (uint64_t)-1;
        }
        const char *buf = (const char *)a2;
        for (uint64_t i = 0; i < a3; i++) {
            kputchar(buf[i]);
        }
        return a3;
    }
    case SYS_EXIT:
        kprintf("[syscall] user process exited (code %llu)\n",
                (unsigned long long)a1);
        thread_exit();
        return 0;   /* unreachable */
    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-1;
    }
}
