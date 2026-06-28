/* kernel/sync/futex.c — fast userspace mutex (P9) */

#include "kernel/sync/futex.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/errno.h"
#include "kernel/proc/usercopy.h"
#include <stdint.h>

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

struct futex_waiter {
    tcb_t               *tcb;
    uint32_t             expected_val;
    struct futex_waiter *next;
};

struct futex_bucket {
    spinlock_t           lock;
    struct futex_waiter *head;
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

    spinlock_acquire(&b->lock);

    struct futex_waiter w = { sched_current(), val, b->head };
    b->head = &w;

    tcb_t *cur = sched_current();
    if (cur) cur->state = THREAD_BLOCKED;

    spinlock_release(&b->lock);

    sched_yield();   /* sleep until woken */

    /* Remove from list (best effort) */
    spinlock_acquire(&b->lock);
    struct futex_waiter **pp = &b->head;
    while (*pp) {
        if (*pp == &w) {
            *pp = w.next;
            break;
        }
        pp = &(*pp)->next;
    }
    spinlock_release(&b->lock);

    return 0;
}

int futex_wake(uint32_t *uaddr, int n) {
    if (n <= 0) return 0;
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;

    uint32_t idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    spinlock_acquire(&b->lock);

    int woken = 0;
    struct futex_waiter *w = b->head;
    struct futex_waiter *prev = NULL;

    while (w && woken < n) {
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            sched_add_thread(t);   /* scheduler must expose this or use ready queue */

            /* unlink */
            if (prev) prev->next = w->next;
            else b->head = w->next;

            woken++;
        }
        prev = w;
        w = w->next;
    }

    spinlock_release(&b->lock);
    return woken;
}