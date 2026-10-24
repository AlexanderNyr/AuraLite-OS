/* kernel/proc/wait_queue.c — True blocking wait queues (H4).
 *
 * OPT_PLAN.md O4: the queue lock is taken irqsave now.  Before, every
 * acquire ran with interrupts as-found, which made waking from IRQ
 * context a latent deadlock: an IRQ landing on a CPU that already held
 * wq->lock (any wq_wait/wq_add caller) would spin on its own lock
 * forever.  The GUI compositor's pokes come from the keyboard/mouse/PIT
 * handlers, so the primitive has to be honest about IRQ callers — and
 * O7's wait4/getrandom conversions will lean on the same guarantee. */

#include "kernel/proc/wait_queue.h"
/* SELFHOST SH5c: the __sync_* spellings above are tcc macros (kernel/lib/
 * atomic_compat.h); clang keeps its builtins. */
#include "kernel/lib/atomic_compat.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/arch/arch.h"      /* arch_irq_save/restore for wq_wait */
#include <stddef.h>

void wq_init(struct wait_queue *wq) {
    if (!wq) return;
    spinlock_init(&wq->lock);
    wq->head = NULL;
}

void wq_add_entry(struct wait_queue *wq, struct wq_entry *entry) {
    if (!wq || !entry) return;
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    entry->next = wq->head;
    wq->head = entry;
    spinlock_release_irqrestore(&wq->lock, wqfl);
}

void wq_remove_entry(struct wait_queue *wq, struct wq_entry *entry) {
    if (!wq || !entry) return;
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    struct wq_entry **pp = &wq->head;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spinlock_release_irqrestore(&wq->lock, wqfl);
}

void wq_wait(struct wait_queue *wq, spinlock_t *lock) {
    if (!wq) return;
    tcb_t *cur = sched_current();
    if (!cur) return;

    struct wq_entry entry = { cur, NULL };

    /* Publish the entry AND mark the thread blocked as one atomic step
     * under the queue lock (the futex.c model), with IRQs already off so
     * the run-up to schedule() is indivisible against a local tick.  The
     * waker side (wq_wake_*) takes this same lock and only wakes
     * THREAD_BLOCKED tcbs: the old split — plain wq_add_entry(), then a
     * bare state store — let a wake landing in between find the entry
     * with the state still ready, clear the list with head = NULL and
     * leave this thread blocked with nobody left to wake it.
     *
     * The old sched_yield() was a second bug in the same wait: it resets
     * THREAD_READY unconditionally, so the BLOCKED set here never stuck
     * and every "wait" actually spun through a full scheduler rotation
     * per retry.  Under the smpstress O_APPEND contention (-smp 4, six
     * writers) that thrash is what pushed the case over its CI budget —
     * blocked threads now really block (the syscall.c note: they account
     * as idle and the scheduler can reach hlt). */
    arch_irqflags_t rflags = arch_irq_save();
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    entry.next = wq->head;
    wq->head = &entry;
    cur->state = THREAD_BLOCKED;
    spinlock_release_irqrestore(&wq->lock, wqfl);

    if (lock) spinlock_release(lock);

    schedule();          /* IRQs off, like time.c: nanosleep — sleep until
                          * woken; wq_wait_deadline additionally relies on
                          * signal_tick honouring sleep_deadline. */

    arch_irq_restore(rflags);

    if (lock) spinlock_acquire(lock);

    wq_remove_entry(wq, &entry);
}

void wq_wait_deadline(struct wait_queue *wq, spinlock_t *lock,
                      uint64_t deadline_ticks) {
    tcb_t *cur = sched_current();
    if (!cur) return;
    if (deadline_ticks) cur->sleep_deadline = deadline_ticks;
    wq_wait(wq, lock);
    cur->sleep_deadline = 0;
}

/* SMP 3.2: every wake enqueue must first claim tcb.on_queue exactly once
 * (see thread.h); the waker may run on a different cpu than the woken
 * thread, which could still be mid context-switch-out. */
void wq_wake_one(struct wait_queue *wq) {
    if (!wq) return;
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    struct wq_entry *w = wq->head;
    if (w) {
        wq->head = w->next;
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            if (__sync_lock_test_and_set(&t->on_queue, 1) == 0) {
                sched_add_thread(t);
            }
        }
    }
    spinlock_release_irqrestore(&wq->lock, wqfl);
}

void wq_wake_all(struct wait_queue *wq) {
    if (!wq) return;
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    struct wq_entry *w = wq->head;
    wq->head = NULL;
    while (w) {
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            if (__sync_lock_test_and_set(&t->on_queue, 1) == 0) {
                sched_add_thread(t);
            }
        }
        w = w->next;
    }
    spinlock_release_irqrestore(&wq->lock, wqfl);
}

int wq_wake_n(struct wait_queue *wq, int n) {
    if (!wq || n <= 0) return 0;
    uint64_t wqfl = spinlock_acquire_irqsave(&wq->lock);
    int woken = 0;
    struct wq_entry *w = wq->head;
    struct wq_entry *prev = NULL;
    while (w && woken < n) {
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            if (__sync_lock_test_and_set(&t->on_queue, 1) == 0) {
                sched_add_thread(t);
            }
            if (prev) prev->next = w->next;
            else wq->head = w->next;
            woken++;
            w = w->next;
        } else {
            prev = w;
            w = w->next;
        }
    }
    spinlock_release_irqrestore(&wq->lock, wqfl);
    return woken;
}
