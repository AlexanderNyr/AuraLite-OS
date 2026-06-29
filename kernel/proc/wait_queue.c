/* kernel/proc/wait_queue.c — True blocking wait queues (H4) */

#include "kernel/proc/wait_queue.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include <stddef.h>

void wq_init(struct wait_queue *wq) {
    if (!wq) return;
    spinlock_init(&wq->lock);
    wq->head = NULL;
}

void wq_add_entry(struct wait_queue *wq, struct wq_entry *entry) {
    if (!wq || !entry) return;
    spinlock_acquire(&wq->lock);
    entry->next = wq->head;
    wq->head = entry;
    spinlock_release(&wq->lock);
}

void wq_remove_entry(struct wait_queue *wq, struct wq_entry *entry) {
    if (!wq || !entry) return;
    spinlock_acquire(&wq->lock);
    struct wq_entry **pp = &wq->head;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spinlock_release(&wq->lock);
}

void wq_wait(struct wait_queue *wq, spinlock_t *lock) {
    if (!wq) return;
    tcb_t *cur = sched_current();
    if (!cur) return;

    struct wq_entry entry = { cur, NULL };
    wq_add_entry(wq, &entry);

    cur->state = THREAD_BLOCKED;
    if (lock) spinlock_release(lock);

    sched_yield();

    if (lock) spinlock_acquire(lock);

    wq_remove_entry(wq, &entry);
}

void wq_wake_one(struct wait_queue *wq) {
    if (!wq) return;
    spinlock_acquire(&wq->lock);
    struct wq_entry *w = wq->head;
    if (w) {
        wq->head = w->next;
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            sched_add_thread(t);
        }
    }
    spinlock_release(&wq->lock);
}

void wq_wake_all(struct wait_queue *wq) {
    if (!wq) return;
    spinlock_acquire(&wq->lock);
    struct wq_entry *w = wq->head;
    wq->head = NULL;
    while (w) {
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            sched_add_thread(t);
        }
        w = w->next;
    }
    spinlock_release(&wq->lock);
}

int wq_wake_n(struct wait_queue *wq, int n) {
    if (!wq || n <= 0) return 0;
    spinlock_acquire(&wq->lock);
    int woken = 0;
    struct wq_entry *w = wq->head;
    struct wq_entry *prev = NULL;
    while (w && woken < n) {
        tcb_t *t = w->tcb;
        if (t && t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            sched_add_thread(t);
            if (prev) prev->next = w->next;
            else wq->head = w->next;
            woken++;
            w = w->next;
        } else {
            prev = w;
            w = w->next;
        }
    }
    spinlock_release(&wq->lock);
    return woken;
}
