/* syscall.c — system call dispatch.
 *
 * Phase 9: expanded set. SYS_WRITE and SYS_EXIT from Phase 8, plus SYS_READ
 * (stub — no input device yet) and SYS_GETPID. The dispatch signature matches
 * the Linux ABI: rax=num, then up to 6 arguments.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_EXIT  60

/* Alternative getpid numbers used by different libcs. */
#define SYS_GETPID  39

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd, a2 = buffer, a3 = length. Only fd=1 (stdout) supported. */
        if (a1 != 1) {
            return (uint64_t)-1;
        }
        const char *buf = (const char *)a2;
        for (uint64_t i = 0; i < a3; i++) {
            kputchar(buf[i]);
        }
        return a3;
    }
    case SYS_READ:
        /* No input device yet; return 0 (EOF). */
        return 0;
    case SYS_EXIT:
        thread_exit();
        return 0;   /* unreachable */
    case SYS_GETPID: {
        tcb_t *cur = sched_current();
        return cur ? cur->id : 0;
    }
    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-1;
    }
}
