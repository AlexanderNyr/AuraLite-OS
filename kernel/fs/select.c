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

/* Kernel-buffer variant used by do_select (user copies) and do_ppoll.
 * readfds/writefds/exceptfds are KERNEL pointers; on return they hold the
 * ready sets.  Returns the number of ready fds, 0 (timeout / nothing), or
 * a negative errno (-EINTR when a pending unmasked signal woke the wait —
 * Q16). */
static int do_select_kernel(int nfds, fd_set *r, fd_set *w, fd_set *e,
                            struct kernel_timeval *timeout) {
    tcb_t *cur = sched_current();
    if (!cur || nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;

    int ready = 0;
    fd_set r_out, w_out, e_out;
    FD_ZERO(&r_out); FD_ZERO(&w_out); FD_ZERO(&e_out);
    (void)e; /* exception readiness is not supported yet; output is cleared. */

    for (int fd = 0; fd < nfds; fd++) {
        struct ofd *o = cur->fd_table[fd];
        if (!o) continue;

        int can_read  = (r && FD_ISSET(fd, r))  && vfs_ofd_is_readable(o);
        int can_write = (w && FD_ISSET(fd, w)) && vfs_ofd_is_writable(o);

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
            if (r && FD_ISSET(fd, r)) {
                rwqs[fd] = vfs_get_read_wq(o);
                if (rwqs[fd]) {
                    rentries[fd].tcb = cur;
                    wq_add_entry(rwqs[fd], &rentries[fd]);
                }
            }
            if (w && FD_ISSET(fd, w)) {
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

        /* Block for real: schedule() directly (NOT sched_yield, which
         * rewrites state to THREAD_READY and would make this a spin), with
         * IRQs off exactly like kernel_nanosleep.  signal_send() and the
         * wait-queue wakers flip state to READY and enqueue us; the timer
         * wakes us via sleep_deadline (signal_tick). */
        {
            uint64_t rflags;
            __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
            cur->state = THREAD_BLOCKED;
            schedule();
            if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        }

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

            int can_read  = (r && FD_ISSET(fd, r))  && vfs_ofd_is_readable(o);
            int can_write = (w && FD_ISSET(fd, w)) && vfs_ofd_is_writable(o);

            if (can_read)  { FD_SET(fd, &r_out); ready++; }
            if (can_write) { FD_SET(fd, &w_out); ready++; }
        }

        /* Q16 (pselect/ppoll): a wake with nothing ready must be a signal
         * interrupt (signal_send now wakes blocked threads for unmasked
         * signals).  Return -EINTR so pselect/select can restart or run the
         * handler, exactly like kernel_nanosleep. */
        if (ready == 0 && cur && (cur->sig_pending & ~cur->sig_mask)) {
            kfree(rentries);
            kfree(wentries);
            kfree(rwqs);
            kfree(wwqs);
            return -EINTR;
        }

        kfree(rentries);
        kfree(wentries);
        kfree(rwqs);
        kfree(wwqs);
    }

    if (r) *r = r_out;
    if (w) *w = w_out;
    if (e) *e = e_out;

    return ready;
}

/* User-pointer wrapper (SYS_SELECT): copies fd_sets in and out. */
int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
              struct kernel_timeval *timeout) {
    fd_set r, w, e;
    fd_set *rp = NULL, *wp = NULL, *ep = NULL;
    if (readfds) {
        if (copy_from_user(&r, readfds, sizeof(fd_set)) != 0) return -EFAULT;
        rp = &r;
    }
    if (writefds) {
        if (copy_from_user(&w, writefds, sizeof(fd_set)) != 0) return -EFAULT;
        wp = &w;
    }
    if (exceptfds) {
        if (copy_from_user(&e, exceptfds, sizeof(fd_set)) != 0) return -EFAULT;
        ep = &e;
    }

    int ready = do_select_kernel(nfds, rp, wp, ep, timeout);
    if (ready < 0) return ready;

    if (readfds && copy_to_user(readfds, &r, sizeof(fd_set)) != 0) return -EFAULT;
    if (writefds && copy_to_user(writefds, &w, sizeof(fd_set)) != 0) return -EFAULT;
    if (exceptfds && copy_to_user(exceptfds, &e, sizeof(fd_set)) != 0) return -EFAULT;
    return ready;
}

/* ---- Q16: ppoll ---- */

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct kernel_pollfd {
    int   fd;
    short events;
    short revents;
};

/* ppoll: pollfds + relative timeout + atomically-installed signal mask.
 * Converts the pollfd array to fd_sets, delegates the wait to do_select
 * (which handles blocking, timeouts and the Q16 signal-wake/-EINTR path),
 * then converts the readiness back to revents.  The mask is applied for
 * the duration of the block only, exactly like pselect. */
int do_ppoll(struct kernel_pollfd *ufds, uint64_t nfds,
             struct kernel_timespec *timeout, const sigset_t *sigmask) {
    tcb_t *cur = sched_current();
    if (!cur || nfds == 0 || nfds > 64) return -EINVAL;

    struct kernel_pollfd *kfds = kmalloc(nfds * sizeof(*kfds));
    if (!kfds) return -ENOMEM;
    if (copy_from_user(kfds, ufds, nfds * sizeof(*kfds)) != 0) {
        kfree(kfds);
        return -EFAULT;
    }

    fd_set rfds, wfds;
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    int maxfd = -1;
    for (uint64_t i = 0; i < nfds; i++) {
        kfds[i].revents = 0;
        int fd = kfds[i].fd;
        if (fd < 0) continue;
        if (fd >= FD_SETSIZE) { kfds[i].revents |= POLLNVAL; continue; }
        if (kfds[i].events & POLLIN)  FD_SET(fd, &rfds);
        if (kfds[i].events & POLLOUT) FD_SET(fd, &wfds);
        if (fd > maxfd) maxfd = fd;
    }

    struct kernel_timeval tv, *ptv = NULL;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L) {
            kfree(kfds);
            return -EINVAL;
        }
        tv.tv_sec  = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000;
        ptv = &tv;
    }

    uint32_t old_mask = cur->sig_mask;
    if (sigmask) cur->sig_mask = *sigmask;
    int r = do_select_kernel(maxfd + 1, &rfds, &wfds, NULL, ptv);
    if (sigmask) cur->sig_mask = old_mask;

    if (r < 0) {
        kfree(kfds);
        return r;   /* -EINTR etc. */
    }

    /* Convert readiness back to revents.  Re-check each fd directly: the
     * fd_sets are NOT a reliable result (do_select_kernel only writes them
     * on the normal path, not on the timeout-0 / -EINTR early returns). */
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        int fd = kfds[i].fd;
        if (fd < 0) continue;
        if (fd >= FD_SETSIZE) { kfds[i].revents |= POLLNVAL; continue; }
        struct ofd *o = cur->fd_table[fd];
        short rev = 0;
        if (o && (kfds[i].events & POLLIN) && vfs_ofd_is_readable(o))
            rev |= POLLIN;
        if (o && (kfds[i].events & POLLOUT) && vfs_ofd_is_writable(o))
            rev |= POLLOUT;
        if (rev) ready++;
        kfds[i].revents = rev;
    }

    if (copy_to_user(ufds, kfds, nfds * sizeof(*kfds)) != 0) {
        kfree(kfds);
        return -EFAULT;
    }
    kfree(kfds);
    return ready;
}
