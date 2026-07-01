/* kernel/fs/select.c — select / poll (P10) */

#include "kernel/fs/vfs.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/usercopy.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/time.h"
#include "drivers/timer/pit.h"
#include "kernel/mm/kheap.h"
#include <stdint.h>

#define FD_SETSIZE 64

typedef struct {
    uint64_t fds_bits[FD_SETSIZE / 64];
} fd_set;

static inline void FD_ZERO(fd_set *set) { memset(set, 0, sizeof(fd_set)); }
static inline void FD_SET(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->fds_bits[fd / 64] |= (1ULL << (fd % 64));
}
static inline int FD_ISSET(int fd, fd_set *set) {
    return (fd >= 0 && fd < FD_SETSIZE) && (set->fds_bits[fd / 64] & (1ULL << (fd % 64)));
}

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct kernel_timeval *timeout) {
    tcb_t *cur = sched_current();
    if (!cur || nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;

    int ready = 0;
    fd_set r, w, e, r_out, w_out, e_out;
    FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&e);
    FD_ZERO(&r_out); FD_ZERO(&w_out); FD_ZERO(&e_out);
    if (readfds && copy_from_user(&r, readfds, sizeof(fd_set)) != 0) return -EFAULT;
    if (writefds && copy_from_user(&w, writefds, sizeof(fd_set)) != 0) return -EFAULT;
    if (exceptfds && copy_from_user(&e, exceptfds, sizeof(fd_set)) != 0) return -EFAULT;
    (void)e; /* exception readiness is not supported yet; output is cleared. */

    for (int fd = 0; fd < nfds; fd++) {
        struct ofd *o = cur->fd_table[fd];
        if (!o) continue;

        int can_read  = (readfds  && FD_ISSET(fd, &r))  && vfs_ofd_is_readable(o);
        int can_write = (writefds && FD_ISSET(fd, &w)) && vfs_ofd_is_writable(o);

        if (can_read)  { FD_SET(fd, &r_out); ready++; }
        if (can_write) { FD_SET(fd, &w_out); ready++; }
    }

    if (ready == 0 && timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0)
        return 0;   /* timeout immediately */

    /* True blocking wait via wait_queue (H4) */
    if (ready == 0) {
        struct wq_entry *rentries = NULL;
        struct wq_entry *wentries = NULL;
        struct wait_queue **rwqs = NULL;
        struct wait_queue **wwqs = NULL;

        if (nfds > 0) {
            rentries = kmalloc(sizeof(*rentries) * (uint64_t)nfds);
            wentries = kmalloc(sizeof(*wentries) * (uint64_t)nfds);
            rwqs = kmalloc(sizeof(*rwqs) * (uint64_t)nfds);
            wwqs = kmalloc(sizeof(*wwqs) * (uint64_t)nfds);
            if (!rentries || !wentries || !rwqs || !wwqs) {
                kfree(rentries);
                kfree(wentries);
                kfree(rwqs);
                kfree(wwqs);
                return -ENOMEM;
            }
            memset(rentries, 0, sizeof(*rentries) * (uint64_t)nfds);
            memset(wentries, 0, sizeof(*wentries) * (uint64_t)nfds);
            memset(rwqs, 0, sizeof(*rwqs) * (uint64_t)nfds);
            memset(wwqs, 0, sizeof(*wwqs) * (uint64_t)nfds);
        }

        for (int fd = 0; fd < nfds; fd++) {
            struct ofd *o = cur->fd_table[fd];
            if (!o) continue;
            if (readfds && FD_ISSET(fd, &r)) {
                rwqs[fd] = vfs_get_read_wq(o);
                if (rwqs[fd]) {
                    rentries[fd].tcb = cur;
                    wq_add_entry(rwqs[fd], &rentries[fd]);
                }
            }
            if (writefds && FD_ISSET(fd, &w)) {
                wwqs[fd] = vfs_get_write_wq(o);
                if (wwqs[fd]) {
                    wentries[fd].tcb = cur;
                    wq_add_entry(wwqs[fd], &wentries[fd]);
                }
            }
        }

        uint64_t old_sleep = cur->sleep_deadline;
        if (timeout) {
            uint64_t freq = timer_get_frequency();
            if (freq == 0) freq = 100;
            uint64_t total_ticks = (uint64_t)timeout->tv_sec * freq +
                                   (uint64_t)timeout->tv_usec * freq / 1000000ULL;
            if (total_ticks > 0) {
                cur->sleep_deadline = timer_get_ticks() + total_ticks;
            }
        }

        cur->state = THREAD_BLOCKED;
        sched_yield();

        cur->sleep_deadline = old_sleep;

        /* Remove from all wait queues */
        for (int fd = 0; fd < nfds; fd++) {
            if (rwqs[fd]) wq_remove_entry(rwqs[fd], &rentries[fd]);
            if (wwqs[fd]) wq_remove_entry(wwqs[fd], &wentries[fd]);
        }

        /* Re-scan for readiness after waking up */
        FD_ZERO(&r_out); FD_ZERO(&w_out);
        ready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            struct ofd *o = cur->fd_table[fd];
            if (!o) continue;

            int can_read  = (readfds  && FD_ISSET(fd, &r))  && vfs_ofd_is_readable(o);
            int can_write = (writefds && FD_ISSET(fd, &w)) && vfs_ofd_is_writable(o);

            if (can_read)  { FD_SET(fd, &r_out); ready++; }
            if (can_write) { FD_SET(fd, &w_out); ready++; }
        }

        kfree(rentries);
        kfree(wentries);
        kfree(rwqs);
        kfree(wwqs);
    }

    if (readfds && copy_to_user(readfds, &r_out, sizeof(fd_set)) != 0) return -EFAULT;
    if (writefds && copy_to_user(writefds, &w_out, sizeof(fd_set)) != 0) return -EFAULT;
    if (exceptfds && copy_to_user(exceptfds, &e_out, sizeof(fd_set)) != 0) return -EFAULT;

    return ready;
}
