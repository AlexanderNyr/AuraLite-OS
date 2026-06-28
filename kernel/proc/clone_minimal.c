/* Minimal clone implementation - no duplicates */
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/errno.h"
#include <stdint.h>

int64_t do_clone(uint64_t f, uint64_t s, uint64_t p, uint64_t c, uint64_t t) {
    (void)f; (void)s; (void)p; (void)c; (void)t;
    return -ENOSYS;
}

int64_t do_arch_prctl(int code, uint64_t addr) {
    (void)code; (void)addr;
    return -ENOSYS;
}

int64_t do_futex(uint64_t u, int o, uint32_t v, uint64_t tm, uint32_t *u2, uint32_t v3) {
    (void)u; (void)o; (void)v; (void)tm; (void)u2; (void)v3;
    return -ENOSYS;
}

int64_t do_tkill(int64_t tid, int sig) {
    (void)tid; (void)sig;
    return -ENOSYS;
}
