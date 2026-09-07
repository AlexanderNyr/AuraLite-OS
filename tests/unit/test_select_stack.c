#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/fs/vfs.h"
#include "../../kernel/proc/thread.h"
#include "../../kernel/time.h"

static tcb_t fake_cur;
static int copy_from_user_calls;
static int copy_to_user_calls;
static int kernel_block_calls;   /* Q16: do_select blocks via kernel_block_current() */
static int wq_add_calls;
static int wq_remove_calls;
static size_t kmalloc_calls;
static size_t kfree_calls;

void *kmalloc(size_t n) { kmalloc_calls++; return calloc(1, n); }
void kfree(void *p) { if (p) kfree_calls++; free(p); }
void schedule(void) { /* no-op: do_select never blocks in these tests */ }
int copy_from_user(void *dst, const void *src, uint64_t len) { memcpy(dst, src, (size_t)len); copy_from_user_calls++; return 0; }
int copy_to_user(void *dst, const void *src, uint64_t len) { memcpy(dst, src, (size_t)len); copy_to_user_calls++; return 0; }
tcb_t *sched_current(void) { return &fake_cur; }
void kernel_block_current(void) { kernel_block_calls++; fake_cur.state = 0; }
struct wait_queue *vfs_get_read_wq(struct ofd *o) { return &o->read_wq; }
struct wait_queue *vfs_get_write_wq(struct ofd *o) { return &o->write_wq; }

/* RESIDUE2 CI fix (run 92482275773): select.c's EINTR gate now asks the
 * signal subsystem whether any pending unmasked signal is ACTIONABLE
 * (would run a handler / stop / kill) instead of testing raw
 * sig_pending & ~sig_mask -- a default-ignore signal (SIGCHLD with no
 * handler) must not turn a timeout into EINTR.  Stubbed with a knob so
 * both sides of the gate stay unit-testable. */
static int fake_actionable_pending = 0;
int signal_actionable_pending(tcb_t *t) { (void)t; return fake_actionable_pending; }

/* Stubs for the pipe-aware readiness helpers used by select.c (BUG-28).
 * The real implementations live in kernel/fs/vfs.c. */
static int force_readable = 0;
static int force_writable = 0;
int vfs_ofd_is_readable(struct ofd *o) { return force_readable || (o->pos < o->vn->size || o->nonblock); }
int vfs_ofd_is_writable(struct ofd *o) { return force_writable || (o->access_mode != O_RDONLY); }

void wq_add_entry(struct wait_queue *q, struct wq_entry *e) { (void)q; (void)e; wq_add_calls++; }
void wq_remove_entry(struct wait_queue *q, struct wq_entry *e) { (void)q; (void)e; wq_remove_calls++; }
uint32_t ktime_hz(void) { return 100; }
uint64_t ktime_ticks(void) { return 1000; }

#include "../../kernel/fs/select.c"

static void reset_state(void) {
    memset(&fake_cur, 0, sizeof(fake_cur));
    copy_from_user_calls = 0;
    copy_to_user_calls = 0;
    kernel_block_calls = 0;
    wq_add_calls = 0;
    wq_remove_calls = 0;
    kmalloc_calls = 0;
    kfree_calls = 0;
    fake_actionable_pending = 0;
}

static void test_zero_timeout_no_alloc(void) {
    reset_state();
    fd_set rfds;
    struct kernel_timeval tv = {0, 0};
    FD_ZERO(&rfds);
    assert(do_select(64, &rfds, NULL, NULL, &tv) == 0);
    assert(kmalloc_calls == 0);
    assert(kfree_calls == 0);
}

static void test_blocking_path_heap_allocates_per_nfds(void) {
    reset_state();
    struct vnode vnode;
    struct ofd ofd;
    memset(&vnode, 0, sizeof(vnode));
    memset(&ofd, 0, sizeof(ofd));
    vnode.size = 0;
    ofd.vn = &vnode;
    ofd.access_mode = O_RDONLY;
    fake_cur.fd_table[0] = &ofd;

    fd_set rfds;
    struct kernel_timeval tv = {1, 0};
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);

    int ready = do_select(1, &rfds, NULL, NULL, &tv);
    assert(ready == 0);
    assert(kmalloc_calls == 4);
    assert(kfree_calls == 4);
    assert(wq_add_calls == 1);
    assert(wq_remove_calls == 1);
    assert(kernel_block_calls == 1);   /* Q16: blocks once, no spin */
}

/* BUG-28: a pipe fd whose helper reports ready must be returned immediately
 * without entering the blocking path. */
static void test_pipe_ready_returns_immediately(void) {
    reset_state();
    struct vnode vnode;
    struct ofd ofd;
    memset(&vnode, 0, sizeof(vnode));
    memset(&ofd, 0, sizeof(ofd));
    vnode.size = 0;              /* pipe-like: vn->size is always 0 */
    ofd.vn = &vnode;
    ofd.access_mode = O_RDONLY;
    fake_cur.fd_table[0] = &ofd;

    force_readable = 1;
    fd_set rfds;
    struct kernel_timeval tv = {0, 0};   /* zero timeout */
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);

    int ready = do_select(1, &rfds, NULL, NULL, &tv);
    assert(ready == 1);
    assert(kmalloc_calls == 0); /* no blocking path allocations */
    assert(kfree_calls == 0);
    assert(kernel_block_calls == 0);
    force_readable = 0;
}

/* RESIDUE2 CI fix (run 92482275773): the EINTR gate must distinguish
 * actionable pending signals (handler/stop/kill -> -EINTR) from pending
 * default-ignore ones (e.g. SIGCHLD with no handler -> plain timeout 0).
 * The kernel-side predicate is stubbed with a knob; both sides covered. */
static void test_eintr_gate_requires_actionable_signal(void) {
    struct vnode vnode;
    struct ofd ofd;
    fd_set rfds;
    struct kernel_timeval tv = {1, 0};

    reset_state();
    memset(&vnode, 0, sizeof(vnode));
    memset(&ofd, 0, sizeof(ofd));
    ofd.vn = &vnode;
    ofd.access_mode = O_RDONLY;
    fake_cur.fd_table[0] = &ofd;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);

    /* A pending default-ignore signal (not actionable): timeout, not EINTR
     * -- this is the exact shape of the CI flake the fix closes. */
    fake_actionable_pending = 0;
    assert(do_select(1, &rfds, NULL, NULL, &tv) == 0);
    assert(kernel_block_calls == 1);

    /* A pending caught signal: -EINTR. */
    fake_actionable_pending = 1;
    assert(do_select(1, &rfds, NULL, NULL, &tv) == -EINTR);
    assert(kernel_block_calls == 2);
    fake_actionable_pending = 0;
}

int main(void) {
    test_zero_timeout_no_alloc();
    test_blocking_path_heap_allocates_per_nfds();
    test_pipe_ready_returns_immediately();
    test_eintr_gate_requires_actionable_signal();
    return 0;
}
