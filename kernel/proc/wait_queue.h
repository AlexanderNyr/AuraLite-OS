#ifndef AURALITE_PROC_WAIT_QUEUE_H
#define AURALITE_PROC_WAIT_QUEUE_H

#include "kernel/lib/spinlock.h"

struct tcb;

struct wq_entry {
    struct tcb      *tcb;
    struct wq_entry *next;
};

struct wait_queue {
    spinlock_t       lock;
    struct wq_entry *head;
};

void wq_init(struct wait_queue *wq);
void wq_wait(struct wait_queue *wq, spinlock_t *lock);

/* OPT_PLAN.md O7: wq_wait with a PIT-tick deadline as the wake-up net.
 * The lost-wakeup window of a flag-check-then-sleep caller, and event
 * sources that have no waker hook yet (a child STOPPING, for wait4),
 * are both bounded by the deadline instead of being hangs.  deadline 0
 * means no net (plain wq_wait). */
void wq_wait_deadline(struct wait_queue *wq, spinlock_t *lock,
                      uint64_t deadline_ticks);
void wq_wake_one(struct wait_queue *wq);
void wq_wake_all(struct wait_queue *wq);
int  wq_wake_n(struct wait_queue *wq, int n);

/* Helpers for select/poll multi-queue waiting */
void wq_add_entry(struct wait_queue *wq, struct wq_entry *entry);
void wq_remove_entry(struct wait_queue *wq, struct wq_entry *entry);

#endif /* AURALITE_PROC_WAIT_QUEUE_H */