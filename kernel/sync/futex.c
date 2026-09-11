/* kernel/sync/futex.c — fast userspace mutex (P9).
 *
 * LX_COMPAT L4: extended from WAIT/WAKE to the full set glibc's lock paths
 * use — WAIT_BITSET / WAKE_BITSET / REQUEUE / CMP_REQUEUE.  A waiter records
 * the bitset it blocked with; WAKE_BITSET wakes only waiters whose bitset
 * intersects, REQUEUE moves a bucket's sleeping waiters to a second uaddr's
 * bucket (the pthread-condvar broadcast shape).  Everything rides the same
 * per-bucket queue the L1 WAIT/WAKE used.
 */

#include "kernel/sync/futex.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/atomic_compat.h"   /* __sync_* spellings (tcc) */
#include "kernel/lib/errno.h"
#include "kernel/proc/usercopy.h"
#include <stdint.h>

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

/* A waiter node.  It is a STACK object of the blocked thread, so it stays
 * live for exactly as long as that thread sleeps; REQUEUE re-links it into
 * a different bucket without ever waking its owner.  `bucket` always names
 * the list the node is currently on, which is what the owner consults when
 * it resumes and unlinks itself. */
struct futex_waiter {
    struct futex_waiter *next;
    struct tcb         *tcb;
    uint32_t            bitset;
    struct futex_bucket *bucket;
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

/* Wait on @uaddr == @val, blocking with @bitset.  Returns:
 *    0            woken (or, L1-compat, the value no longer matched)
 *   -EAGAIN       *uaddr != val at entry (Linux's futex contract)
 *   -EFAULT       bad user pointer
 */
int futex_wait_bitset(uint32_t *uaddr, uint32_t val, uint32_t bitset) {
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;

    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;

    struct futex_bucket *b = &futex_table[futex_hash(uaddr)];
    struct futex_waiter w = { NULL, cur, bitset, b };

    /* The value re-check and the enqueue happen under the bucket lock, so a
     * wake that lands in between sees the waiter in the queue (no lost
     * wakeup). */
    uint64_t fl = spinlock_acquire_irqsave(&b->lock);
    uint32_t current;
    if (copy_from_user(&current, uaddr, sizeof(uint32_t)) != 0) {
        spinlock_release_irqrestore(&b->lock, fl);
        return -EFAULT;
    }
    if (current != val) {
        spinlock_release_irqrestore(&b->lock, fl);
        return -EAGAIN;
    }
    w.next = b->head;
    b->head = &w;
    cur->state = THREAD_BLOCKED;
    spinlock_release_irqrestore(&b->lock, fl);

    sched_yield();

    /* Woken (or requeued-and-then-woken): the waker has already unlinked us,
     * so this self-unlink is the safety net for any path that left the node
     * in place.  `w.bucket` may have been changed by a requeue while we
     * slept; consult the CURRENT bucket. */
    struct futex_bucket *cb = w.bucket;
    uint64_t fl2 = spinlock_acquire_irqsave(&cb->lock);
    struct futex_waiter **pp = &cb->head;
    while (*pp) {
        if (*pp == &w) { *pp = w.next; break; }
        pp = &(*pp)->next;
    }
    spinlock_release_irqrestore(&cb->lock, fl2);
    return 0;
}

int futex_wait(uint32_t *uaddr, uint32_t val) {
    return futex_wait_bitset(uaddr, val, FUTEX_BITSET_MATCH_ANY);
}

/* Wake up to @n blocked waiters whose bitset intersects @bitset.
 * Returns the number actually woken. */
int futex_wake_bitset(uint32_t *uaddr, int n, uint32_t bitset) {
    if (n <= 0) return 0;
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;

    struct futex_bucket *b = &futex_table[futex_hash(uaddr)];
    uint64_t fl = spinlock_acquire_irqsave(&b->lock);
    int woken = 0;
    struct futex_waiter **pp = &b->head;
    while (*pp && woken < n) {
        struct futex_waiter *w = *pp;
        if (w->tcb && w->tcb->state == THREAD_BLOCKED &&
            (w->bitset & bitset) != 0) {
            *pp = w->next;                 /* unlink */
            w->tcb->state = THREAD_READY;
            if (__sync_lock_test_and_set(&w->tcb->on_queue, 1) == 0)
                sched_add_thread(w->tcb);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }
    spinlock_release_irqrestore(&b->lock, fl);
    return woken;
}

int futex_wake(uint32_t *uaddr, int n) {
    return futex_wake_bitset(uaddr, n, FUTEX_BITSET_MATCH_ANY);
}

/* REQUEUE / CMP_REQUEUE.  Wake up to @nr_wake waiters on @uaddr, then move
 * every remaining blocked waiter to @uaddr2's bucket (the condvar broadcast
 * shape).  With a non-zero @cmpval this is CMP_REQUEUE and first checks
 * *uaddr == cmpval under the lock, answering -EAGAIN on mismatch.  Returns
 * the number woken (Linux's contract). */
int futex_requeue(uint32_t *uaddr, int nr_wake, uint32_t *uaddr2,
                  uint32_t cmpval) {
    if (!validate_user_range(uaddr, sizeof(uint32_t), 0))
        return -EFAULT;
    if (uaddr2 && !validate_user_range(uaddr2, sizeof(uint32_t), 0))
        return -EFAULT;

    struct futex_bucket *a = &futex_table[futex_hash(uaddr)];
    struct futex_bucket *bb = uaddr2 ? &futex_table[futex_hash(uaddr2)] : a;

    uint64_t fl = spinlock_acquire_irqsave(&a->lock);
    if (cmpval != 0) {
        uint32_t current;
        if (copy_from_user(&current, uaddr, sizeof(uint32_t)) != 0) {
            spinlock_release_irqrestore(&a->lock, fl);
            return -EFAULT;
        }
        if (current != cmpval) {
            spinlock_release_irqrestore(&a->lock, fl);
            return -EAGAIN;
        }
    }

    /* 1) Wake up to nr_wake (any bitset). */
    int woken = 0;
    struct futex_waiter **pp = &a->head;
    while (*pp && woken < nr_wake) {
        struct futex_waiter *w = *pp;
        if (w->tcb && w->tcb->state == THREAD_BLOCKED) {
            *pp = w->next;
            w->tcb->state = THREAD_READY;
            if (__sync_lock_test_and_set(&w->tcb->on_queue, 1) == 0)
                sched_add_thread(w->tcb);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    /* 2) Move the REST of the sleeping waiters to uaddr2's bucket.  The
     *    nodes stay BLOCKED, so their owners never observe the move until
     *    they are (later) woken there — at which point the self-unlink in
     *    futex_wait_bitset consults the node's updated bucket pointer. */
    if (bb != a && a->head) {
        struct futex_waiter *tail = a->head;
        while (tail->next) tail = tail->next;
        uint64_t fl2 = spinlock_acquire_irqsave(&bb->lock);
        tail->next = bb->head;
        bb->head = a->head;
        for (struct futex_waiter *w = a->head; w; w = w->next)
            w->bucket = bb;
        a->head = NULL;
        spinlock_release_irqrestore(&bb->lock, fl2);
    }
    spinlock_release_irqrestore(&a->lock, fl);
    return woken;
}
