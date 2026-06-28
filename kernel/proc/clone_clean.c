/* Clean P9 clone implementation - single definitions */
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/errno.h"
#include <stdint.h>

#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

int64_t do_clone(uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    (void)flags; (void)stack; (void)ptid; (void)ctid; (void)tls;
    return -ENOSYS;
}

int64_t do_arch_prctl(int code, uint64_t addr) {
    (void)code; (void)addr;
    return -ENOSYS;
}

int64_t do_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)uaddr; (void)op; (void)val; (void)timeout; (void)uaddr2; (void)val3;
    return -ENOSYS;
}

int64_t do_tkill(int64_t tid, int sig) {
    (void)tid; (void)sig;
    return -ENOSYS;
}
