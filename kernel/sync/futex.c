/* kernel/sync/futex.c — fast userspace mutex (P9) */

#include "kernel/sync/futex.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/errno.h"
#include "kernel/proc/usercopy.h"
#include <stdint.h>

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

struct futex_bucket {
    struct wait_queue wq;
};

static struct futex_bucket futex_table[FUTEX_HASH_SIZE];

static inline uint32_t futex_hash(uint32_t *uaddr) {
    uintptr_t p = (uintptr_t)uaddr;
    return (uint32_t)((p >> 3) ^ (p >> 13)) & (FUTEX_HASH_SIZE - 1);
}

int futex_wait(uint32_t *uaddr, uint32_t val) {
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;

    uint32_t current;
    if (copy_from_user(&current, uaddr, sizeof(uint32_t)) != 0)
        return -EFAULT;

    if (current != val)
        return 0;   /* value changed, do not sleep */

    uint32_t idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    wq_wait(&b->wq, NULL);

    return 0;
}

int futex_wake(uint32_t *uaddr, int n) {
    if (n <= 0) return 0;
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;

    uint32_t idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    return wq_wake_n(&b->wq, n);
}
