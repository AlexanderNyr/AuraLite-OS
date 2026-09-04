/* thread.c — kernel thread creation, initial stack setup, exit and reaping.
 *
 * A new thread's stack is crafted so that context_switch's first "restore +
 * ret" sequence lands at thread_entry(), which calls fn(arg) and then
 * thread_exit().
 */

#include <stdint.h>
#include "kernel/proc/thread.h"
/* SELFHOST SH5c: the __sync_* spellings above are tcc macros (kernel/lib/
 * atomic_compat.h); clang keeps its builtins. */
#include "kernel/lib/atomic_compat.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/errno.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/slab.h"
#include "kernel/mm/vma.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/gui/gui.h"
#include "kernel/gpu/gpu_syscalls.h"
#include "kernel/ipc/sysvipc.h"
#include "kernel/net/socket.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/lapic.h"
#include "drivers/timer/pit.h"

/* Implemented in context.asm */
extern void context_switch(tcb_t *old, tcb_t *new);

static uint64_t next_tid = 1;

/* SELFHOST SH5c: the stack geometry (SLOT_SIZE / REGION_BASE / MAX_SLOTS /
 * REGION_END) lives in kernel/proc/thread_stack.h -- tss.c derives its IST
 * region base from REGION_END, so the old "hardcode 128 slots * 24 KiB"
 * arithmetic (which the 32-KiB stacks overran, wiping the IST1 stacks)
 * cannot come back. */

static uint8_t thread_stack_slots[THREAD_STACK_MAX_SLOTS];
static spinlock_t thread_stack_lock = SPINLOCK_UNLOCKED;

static tcb_t *all_threads[128];
static int all_threads_count = 0;

/* Guards all_threads[]/all_threads_count.  The pre-SMP code used a plain
 * cli section, which excludes only LOCAL interleavings -- with APs running
 * real threads (SMP step 3.2), register/deregister/lookup on two CPUs at
 * once would corrupt the array.  irqsave because lookups also happen from
 * IRQ context (e.g. signal_tick() on the BSP timer IRQ). */
static spinlock_t thread_registry_lock = SPINLOCK_UNLOCKED;

void thread_register_tcb(tcb_t *tcb) {
    uint64_t flags = spinlock_acquire_irqsave(&thread_registry_lock);
    if (all_threads_count < 128) {
        all_threads[all_threads_count++] = tcb;
    }
    spinlock_release_irqrestore(&thread_registry_lock, flags);
}

void thread_deregister_tcb(tcb_t *tcb) {
    uint64_t flags = spinlock_acquire_irqsave(&thread_registry_lock);
    for (int i = 0; i < all_threads_count; i++) {
        if (all_threads[i] == tcb) {
            all_threads[i] = all_threads[all_threads_count - 1];
            all_threads_count--;
            break;
        }
    }
    spinlock_release_irqrestore(&thread_registry_lock, flags);
}

tcb_t *thread_get_by_pid(uint64_t pid) {
    uint64_t flags = spinlock_acquire_irqsave(&thread_registry_lock);
    tcb_t *found = NULL;
    for (int i = 0; i < all_threads_count; i++) {
        if (all_threads[i]->id == pid) {
            found = all_threads[i];
            break;
        }
    }
    spinlock_release_irqrestore(&thread_registry_lock, flags);
    return found;
}

int thread_get_all(tcb_t *out_list[], int max) {
    uint64_t flags = spinlock_acquire_irqsave(&thread_registry_lock);
    int n = 0;
    for (int i = 0; i < all_threads_count && n < max; i++) {
        out_list[n++] = all_threads[i];
    }
    spinlock_release_irqrestore(&thread_registry_lock, flags);
    return n;
}

/* Dead threads cannot be freed by thread_exit() itself because it is still
 * running on the exiting thread's kernel stack.  They are linked here and
 * later freed by thread_reap_zombies() from another thread's context.
 *
 * A zombie stays on this list (state=THREAD_DEAD, waited=0) until its parent
 * collects it via wait4(); at that point waited flips to 1 and the next
 * reaper sweep frees the TCB, kernel stack and (for user processes) the
 * address-space frames.  Orphans (parent==NULL or parent already exited)
 * are marked waited=1 immediately so they don't linger forever. */
static tcb_t *zombie_head = NULL;
static volatile uint64_t zombies_queued = 0;
static volatile uint64_t zombies_reaped = 0;

/* Guards the zombie list (zombie_head link/unlink, ->waited transitions) and
 * the two counters.  Same SMP rationale as thread_registry_lock above:
 * thread_exit() on one CPU and do_waitpid()/thread_reap_zombies() on another
 * must not race -- a plain cli section no longer excludes the remote party. */
static spinlock_t zombie_lock = SPINLOCK_UNLOCKED;

/* RESIDUE2 T1: guards the parent/child process table (tcb_t::child_head /
 * tcb_t::sibling) and the n_children counters.  Deliberately separate from
 * zombie_lock: do_waitpid matches through the CHILDREN list without ever
 * taking zombie_lock, and the reaper takes children_lock only AFTER it has
 * released zombie_lock — no path holds both, so no lock order exists. */
static spinlock_t children_lock = SPINLOCK_UNLOCKED;

void proc_children_link(tcb_t *parent, tcb_t *child) {
    if (!parent || !child) return;
    uint64_t flags = spinlock_acquire_irqsave(&children_lock);
    child->sibling      = parent->child_head;
    child->parent       = parent;
    parent->child_head  = child;
    parent->n_children++;
    spinlock_release_irqrestore(&children_lock, flags);
}

void proc_children_unlink(tcb_t *parent, tcb_t *child) {
    if (!parent || !child) return;
    uint64_t flags = spinlock_acquire_irqsave(&children_lock);
    tcb_t **pp = &parent->child_head;
    while (*pp && *pp != child) pp = &(*pp)->sibling;
    if (*pp) {
        *pp = child->sibling;
        child->sibling = NULL;
        if (parent->n_children > 0) parent->n_children--;
    }
    spinlock_release_irqrestore(&children_lock, flags);
}

/* OPT_PLAN.md O7: waiters in do_waitpid() block here instead of
 * yield-polling; thread_exit's zombie_enqueue wakes them.  A child
 * STOPPING (WUNTRACED) has no waker hook yet — the deadline net in
 * do_waitpid bounds that path at ~50 ms, recorded in the plan's
 * residue table. */
static struct wait_queue child_exit_wq;

uint64_t thread_zombies_queued_total(void) { return zombies_queued; }
uint64_t thread_zombies_reaped_total(void) { return zombies_reaped; }

void thread_free_kernel_stack(tcb_t *tcb);

int thread_alloc_kernel_stack(tcb_t *tcb) {
    if (!tcb) return -1;
    uint64_t irqf = spinlock_acquire_irqsave(&thread_stack_lock);
    int slot = -1;
    for (int i = 0; i < THREAD_STACK_MAX_SLOTS; i++) {
        if (!thread_stack_slots[i]) {
            thread_stack_slots[i] = 1;
            slot = i;
            break;
        }
    }
    spinlock_release_irqrestore(&thread_stack_lock, irqf);
    if (slot < 0) return -1;

    uint64_t region = THREAD_STACK_REGION_BASE + (uint64_t)slot * THREAD_STACK_SLOT_SIZE;
    uint64_t usable = region + THREAD_STACK_GUARD_PAGES * 4096ULL;

    memset(tcb->kernel_stack_phys, 0, sizeof(tcb->kernel_stack_phys));
    for (int i = 0; i < THREAD_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            tcb->kernel_stack = (void *)(uintptr_t)usable;
            tcb->kernel_stack_region = (void *)(uintptr_t)region;
            tcb->kernel_stack_slot = slot;
            thread_free_kernel_stack(tcb);
            return -1;
        }
        tcb->kernel_stack_phys[i] = phys;
        paging_map(usable + (uint64_t)i * 4096ULL, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXEC);
    }

    tcb->kernel_stack = (void *)(uintptr_t)usable;
    tcb->kernel_stack_region = (void *)(uintptr_t)region;
    tcb->kernel_stack_slot = slot;
    memset(tcb->kernel_stack, 0, THREAD_STACK_SIZE);
    return 0;
}

void thread_free_kernel_stack(tcb_t *tcb) {
    if (!tcb) return;
    if (tcb->kernel_stack) {
        memset(tcb->kernel_stack, 0, THREAD_STACK_SIZE);
    }
    uint64_t usable = (uint64_t)(uintptr_t)tcb->kernel_stack;
    for (int i = 0; i < THREAD_STACK_PAGES; i++) {
        if (tcb->kernel_stack_phys[i]) {
            paging_unmap(usable + (uint64_t)i * 4096ULL);
            pmm_free_frame(tcb->kernel_stack_phys[i]);
            tcb->kernel_stack_phys[i] = 0;
        }
    }
    if (tcb->kernel_stack_slot >= 0 && tcb->kernel_stack_slot < THREAD_STACK_MAX_SLOTS) {
        uint64_t irqf = spinlock_acquire_irqsave(&thread_stack_lock);
        thread_stack_slots[tcb->kernel_stack_slot] = 0;
        spinlock_release_irqrestore(&thread_stack_lock, irqf);
    }
    tcb->kernel_stack = NULL;
    tcb->kernel_stack_region = NULL;
    tcb->kernel_stack_slot = -1;
}

/*
 * Trampoline: this is the "return address" planted on a new thread's stack.
 * context_switch's ret jumps here.  We read the current TCB (set by the
 * scheduler before the switch) and invoke the thread's function.
 * Non-static: the scheduler also uses it for the idle thread's initial frame.
 */
void thread_entry(void) {
    tcb_t *self = sched_current();
    self->entry(self->arg);
    thread_exit();   /* never returns */
}

/*
 * Build the initial stack frame for a newly-created thread so that the first
 * context_switch into it works: 6 callee-saved register slots + 1 return
 * address + 1 alignment padding = 8 qwords = 64 bytes, keeping the saved RSP
 * 16-byte aligned (required so that after pop×6 + ret the function entry has
 * RSP ≡ 8 mod 16, per the System V AMD64 ABI).
 */
static void setup_initial_stack(tcb_t *tcb, void (*fn)(void *), void *arg) {
    /* Record the function/arg for thread_entry to pick up. */
    tcb->entry = fn;
    tcb->arg   = arg;

    uint64_t *sp = (uint64_t *)((uint8_t *)tcb->kernel_stack + THREAD_STACK_SIZE);

    /* Layout (high address first, stack grows down):
     *   alignment padding
     *   return address (thread_entry)
     *   rbx, rbp, r12, r13, r14, r15  (callee-saved, all 0)
     *   RFLAGS (0x202 = IF set, so the new thread starts with interrupts on)
     * The saved RSP points at RFLAGS (lowest).
     * Total: 9 qwords = 0 mod 8, but must also be 0 mod 16 after the
     * eventual popfq+6 pops+ret. 9 qwords is odd, so add one more padding. */
    sp--; *sp = 0;                      /* extra alignment padding          */
    sp--; *sp = 0;                      /* alignment padding                */
    sp--; *sp = (uint64_t)thread_entry; /* return address for ret           */
    sp--; *sp = 0;                      /* rbx                              */
    sp--; *sp = 0;                      /* rbp                              */
    sp--; *sp = 0;                      /* r12                              */
    sp--; *sp = 0;                      /* r13                              */
    sp--; *sp = 0;                      /* r14                              */
    sp--; *sp = 0;                      /* r15                              */
    sp--; *sp = 0x202;                  /* RFLAGS (IF set)                  */

    tcb->rsp = (uint64_t)sp;
}

/* FIX_R3: kthread_create() is split into create and publish.  Callers that
 * must initialise TCB fields beyond the baked-in defaults (clone/fork/
 * spawn set pml4_phys, fork_user_*, tls_base, fd tables, credentials AFTER
 * creation) MUST use kthread_create_unstarted() + kthread_start() around
 * their initialisation: a plain kthread_create() enqueues the TCB onto a
 * run queue immediately, and on SMP a REMOTE cpu is not stopped by the
 * caller's local cli — it could steal and run the half-initialised thread.
 * (Observed during R3 bring-up: the first pthread child intermittently ran
 * with tls_base == 0 or under the kernel PML4, because a sibling cpu
 * dequeued it mid-do_clone; fork had the same latent window guarded only
 * by cli, the comment there even admits it.) */
tcb_t *kthread_create_unstarted(void (*fn)(void *), void *arg,
                                const char *name) {
    tcb_t *tcb = slab_alloc(tcb_cache);
    if (tcb == NULL) {
        kprintf("[thread] FATAL: slab_alloc failed for TCB\n");
        return NULL;
    }
    memset(tcb, 0, sizeof(tcb_t));

    tcb->kernel_stack = NULL;
    tcb->kernel_stack_region = NULL;
    tcb->kernel_stack_slot = -1;
    if (thread_alloc_kernel_stack(tcb) != 0) {
        kprintf("[thread] FATAL: could not allocate guarded kernel stack\n");
        slab_free(tcb_cache, tcb);
        return NULL;
    }

    /* Atomic tid allocation: kthread_create now runs concurrently on
     * several cpus (SMP 3.2); a plain next_tid++ handed out duplicate tids
     * (observed: 'kmain' and 'thread-A' both with tid 1). */
    tcb->id           = __sync_fetch_and_add(&next_tid, 1);
    tcb->state        = THREAD_READY;
    tcb->quantum      = SCHED_QUANTUM;
    /* Born parked: the first-run stack frame is fully crafted below, before
     * the thread becomes visible on any run queue. */
    tcb->switch_parked = 1;
    tcb->on_queue      = 0;   /* queue claim is untaken until the enqueue below */
    spinlock_init(&tcb->vma_lock);
    /* Default: each new task is its own process group and session leader.
     * fork()/spawn() override pgid/sid to inherit from the parent; setsid()/
     * setpgid() change them explicitly (P6). */
    tcb->pgid         = (int64_t)tcb->id;
    tcb->sid          = (int64_t)tcb->id;
    tcb->is_session_leader = 1;
    tcb->umask        = 0022;
    if (name != NULL) {
        strncpy(tcb->name, name, THREAD_NAME_MAX - 1);
        tcb->name[THREAD_NAME_MAX - 1] = 0;
    }

    setup_initial_stack(tcb, fn, arg);
    /* New threads start with cloexec cleared and the fd table zeroed
     * (kmalloc + memset above). */
    thread_register_tcb(tcb);
    /* NOT enqueued yet: the caller completes field initialisation and then
     * publishes the thread with kthread_start(). */
    return tcb;
}

/* Publish a freshly created, fully initialised thread on a run queue.
 * After this returns the thread may run on ANY cpu, so every TCB field
 * the entry path or the scheduler reads must already be set. */
void kthread_start(tcb_t *tcb) {
    /* Fresh thread: nobody else can hold the claim, so this always wins;
     * keep the claim discipline uniform with every other enqueue site. */
    if (__sync_lock_test_and_set(&tcb->on_queue, 1) == 0) {
        sched_add_thread(tcb);
    }
}

tcb_t *kthread_create(void (*fn)(void *), void *arg, const char *name) {
    tcb_t *tcb = kthread_create_unstarted(fn, arg, name);
    if (tcb) kthread_start(tcb);
    return tcb;
}

/* ---- Zombie / wait4 helpers ---- */

/* WNOHANG / WUNTRACED / WCONTINUED / WNOWAIT option bits (libc sys/wait.h). */
#define WAIT_WNOHANG    1
#define WAIT_WUNTRACED  2
#define WAIT_WEXITED    4
#define WAIT_WCONTINUED 8
#define WAIT_WNOWAIT    0x01000000
#define WAITPID_OPT_MASK (WAIT_WNOHANG | WAIT_WUNTRACED | WAIT_WCONTINUED)

/* Build a POSIX wait-status word from a reaped zombie. */
static int wait_status_of(tcb_t *z) {
    if (z->term_signal) return z->term_signal & 0x7f;       /* WIFSIGNALED */
    return (z->exit_code & 0xff) << 8;                       /* WIFEXITED */
}

/* P6a: waitpid child selector (RESIDUE2 T1: now scanned over the caller's
 * children list, so the parent identity is a given; the selector reduces
 * to the pid/pgid forms).
 *   pid > 0  : c->id == pid
 *   pid == 0 : c->pgid == caller's pgid
 *   pid == -1: any child
 *   pid < -1 : c->pgid == |pid|
 */
static int wait_child_matches(tcb_t *parent, tcb_t *child, int64_t pid,
                              int64_t parent_pgid) {
    if (!parent || !child) return 0;
    if (child->parent != parent) return 0;
    if (pid > 0)  return child->id == (uint64_t)pid;
    if (pid == -1) return 1;
    if (pid == 0)  return child->pgid == parent_pgid;
    /* pid < -1 */
    return child->pgid == -pid;
}

int64_t do_waitpid_ex(int64_t pid, int *status, int options,
                      uint64_t *cpu_ticks_out) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    if (options & ~WAITPID_OPT_MASK) return -EINVAL;
    int64_t parent_pgid = self->pgid;

    for (;;) {
        /* RESIDUE2 T1: match through the parent's CHILDREN list (the
         * parent/child process table), not the global registry.  The
         * list holds every live child plus every not-yet-collected
         * zombie, so one structure answers all three questions —
         * reapable exit, WCONTINUED/WUNTRACED report, ECHILD. */
        uint64_t cf = spinlock_acquire_irqsave(&children_lock);
        tcb_t *match = NULL;
        int st = 0;
        int kind = 0;   /* 1 = exit, 2 = continued, 3 = stopped */
        for (tcb_t *c = self->child_head; c; c = c->sibling) {
            if (!wait_child_matches(self, c, pid, parent_pgid)) continue;
            if (c->state == THREAD_DEAD && !c->waited) {
                st = wait_status_of(c);
                if (cpu_ticks_out) *cpu_ticks_out = c->cpu_ticks;
                kind = 1;
                match = c;
                break;
            }
            if ((options & WAIT_WCONTINUED) && c->continued_notified) {
                kind = 2;
                match = c;
                break;
            }
            if ((options & WAIT_WUNTRACED) && c->state == THREAD_STOPPED &&
                !c->stop_notified && c->stop_signal) {
                kind = 3;
                match = c;
                break;
            }
        }
        int64_t who = 0;
        if (match) {
            who = (int64_t)match->id;
            if (kind == 1) {
                match->waited = 1;         /* reaper releases it later */
                if (self->n_children > 0) self->n_children--;
            } else if (kind == 2) {
                match->continued_notified = 0;
                st = 0xFFFF;               /* glibc WIFCONTINUED */
            } else {
                match->stop_notified = 1;
                st = 0x7f | (match->stop_signal << 8);
            }
        }
        spinlock_release_irqrestore(&children_lock, cf);
        if (who > 0) {
            if (status) *status = st;
            return who;
        }

        /* No matching event: is there a matching child AT ALL (ECHILD)?
         * The children list answers in O(children) instead of scanning
         * every thread in the registry. */
        int have_matching_child = 0;
        cf = spinlock_acquire_irqsave(&children_lock);
        for (tcb_t *c = self->child_head; c; c = c->sibling) {
            if (!wait_child_matches(self, c, pid, parent_pgid)) continue;
            if (c->state == THREAD_DEAD && c->waited) continue; /* collected */
            have_matching_child = 1;
            break;
        }
        spinlock_release_irqrestore(&children_lock, cf);

        if (!have_matching_child) return -ECHILD;
        if (options & WAIT_WNOHANG) return 0;   /* matching child exists, none ready */
        /* O7: block until a child exits (zombie_enqueue wakes us) with a
         * 5-tick net — it bounds both the lost-wakeup window (a child
         * exiting between the scan above and the sleep) and WUNTRACED
         * stop events, which have no waker hook yet. */
        wq_wait_deadline(&child_exit_wq, NULL, timer_get_ticks() + 5);
        /* POSIX: a blocking wait interrupted by a signal whose handler is
         * installed (and which is not blocked) fails with -EINTR; the
         * signal stays pending for normal delivery. */
        if (signal_caught_pending(self)) return -EINTR;
    }
}

int64_t do_waitpid(int64_t pid, int *status, int options) {
    return do_waitpid_ex(pid, status, options, NULL);
}

/* RESIDUE2 T1: waitid.  Same event sources as do_waitpid_ex, plus WNOWAIT
 * (peek without collecting) and per-event info output.  Returns the child
 * pid, 0 (WNOHANG, none ready), or a negative errno. */
int64_t do_waitid(int idtype, int64_t id, siginfo_t *info, int options) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    if (idtype < WAITID_P_ALL || idtype > WAITID_P_PGID) return -EINVAL;
    const int want = options & (WAIT_WEXITED | WAIT_WUNTRACED | WAIT_WCONTINUED);
    if (want == 0) return -EINVAL;              /* POSIX: one event required */
    if (options & ~(WAIT_WEXITED | WAIT_WUNTRACED | WAIT_WCONTINUED |
                    WAIT_WNOHANG | WAIT_WNOWAIT)) return -EINVAL;
    if (idtype == WAITID_P_ALL && want != WAIT_WEXITED) {
        /* P_ALL + stopped/continued is legal POSIX but our selector is
         * pid/pgid-shaped; be honest instead of silently mis-matching. */
        return -EINVAL;
    }

    for (;;) {
        uint64_t cf = spinlock_acquire_irqsave(&children_lock);
        tcb_t *match = NULL;
        int kind = 0;   /* 1 = exit, 2 = stopped, 3 = continued */
        for (tcb_t *c = self->child_head; c; c = c->sibling) {
            /* Selector: P_PID -> exact id; P_PGID -> pgid; P_ALL -> any. */
            if (idtype == WAITID_P_PID   && c->id   != (uint64_t)id) continue;
            if (idtype == WAITID_P_PGID  && c->pgid != id) continue;
            if ((want & WAIT_WEXITED) && c->state == THREAD_DEAD && !c->waited) {
                kind = 1; match = c; break;
            }
            if ((want & WAIT_WUNTRACED) && c->state == THREAD_STOPPED &&
                !c->stop_notified && c->stop_signal) {
                kind = 2; match = c; break;
            }
            if ((want & WAIT_WCONTINUED) && c->continued_notified) {
                kind = 3; match = c; break;
            }
        }
        int64_t who = 0;
        if (match) {
            who = (int64_t)match->id;
            if (info) {
                memset(info, 0, sizeof(*info));
                info->si_signo = SIGCHLD;
                info->si_pid   = (int)match->id;
                info->si_uid   = match->uid;
                if (kind == 1) {
                    info->si_code  = match->term_signal ? 2 /* CLD_KILLED */
                                                        : 1 /* CLD_EXITED */;
                    info->si_status = match->term_signal
                                        ? (128 + match->term_signal)
                                        : (match->exit_code & 0xff);
                } else if (kind == 2) {
                    info->si_code   = 5;   /* CLD_STOPPED */
                    info->si_status = match->stop_signal;
                } else {
                    info->si_code   = 6;   /* CLD_CONTINUED */
                    info->si_status = SIGCONT;
                }
            }
            if (!(options & WAIT_WNOWAIT)) {
                if (kind == 1) {
                    match->waited = 1;
                    if (self->n_children > 0) self->n_children--;
                } else if (kind == 2) {
                    match->stop_notified = 1;
                } else {
                    match->continued_notified = 0;
                }
            }
        }
        spinlock_release_irqrestore(&children_lock, cf);
        if (who > 0) return who;

        /* ECHILD: no matching, still-waitable child on our list. */
        int have = 0;
        cf = spinlock_acquire_irqsave(&children_lock);
        for (tcb_t *c = self->child_head; c; c = c->sibling) {
            if (idtype == WAITID_P_PID  && c->id   != (uint64_t)id) continue;
            if (idtype == WAITID_P_PGID && c->pgid != id) continue;
            if (c->state == THREAD_DEAD && c->waited) continue;
            have = 1;
            break;
        }
        spinlock_release_irqrestore(&children_lock, cf);
        if (!have) return -ECHILD;
        if (options & WAIT_WNOHANG) return 0;
        wq_wait_deadline(&child_exit_wq, NULL, timer_get_ticks() + 5);
        if (signal_caught_pending(self)) return -EINTR;
    }
}

/* Legacy wrapper used by internal callers (process.c) — blocking, any child. */
int64_t do_wait4_pid(int64_t pid, int64_t *exit_code) {
    int st = 0;
    int64_t r = do_waitpid(pid, exit_code ? &st : 0, 0);
    if (exit_code) {
        /* Preserve the old "raw exit code" contract for internal callers:
         * decode the POSIX status back to the 0..255 / 128+signo form. */
        if ((st & 0x7f) && !(st & 0xff00)) *exit_code = 128 + (st & 0x7f);
        else *exit_code = (st >> 8) & 0xff;
    }
    return r;
}

static void close_process_fds(tcb_t *t) {
    if (!t) return;
    for (int fd = 0; fd < VFS_MAX_FDS; fd++) {
        if (t->fd_table[fd] != NULL) {
            vfs_close(fd);
        }
    }
}

static void zombie_enqueue(tcb_t *t) {
    uint64_t flags = spinlock_acquire_irqsave(&zombie_lock);
    t->next = zombie_head;
    zombie_head = t;
    zombies_queued++;
    spinlock_release_irqrestore(&zombie_lock, flags);
    /* O7: a parent may be blocked in do_waitpid — this IS its event. */
    wq_wake_all(&child_exit_wq);
}

void thread_reap_zombies(void) {
    /* Unlink the waited zombies under zombie_lock: on SMP the enqueue side
     * (thread_exit_with_code) and waiters (do_waitpid) may be on other CPUs;
     * a local cli section cannot exclude them. */
    uint64_t rz = spinlock_acquire_irqsave(&zombie_lock);

    tcb_t *current = sched_current();
    tcb_t *reap = NULL;
    tcb_t **pp = &zombie_head;
    while (*pp) {
        tcb_t *z = *pp;
        if (z == current || !z->waited || z->switch_parked == 0) {
            /* Leave in place:  still the running thread (impossible since
             * we are alive), a zombie nobody has wait4()ed yet — or, FIX_R3:
             * a zombie whose final switch-AWAY has not completed on its last
             * cpu.  switch_parked reads 0 on any thread that is currently ON
             * a cpu (established at pick time, set back to 1 only inside
             * context_switch after the frame save).  A DEAD thread enqueues
             * itself on the zombie list BEFORE its last switch-out, so with
             * an already-waiting parent and a reaper on another cpu the TCB
             * could be memset + its kernel stack freed while the owner was
             * still executing its exit tail on that same stack — observed
             * live as a stack-protector trip in spinlock_acquire_irqsave on
             * the dying thread's stack, with the thread's TCB already
             * zeroed (name "", kernel_stack NULL).  The picker protocol
             * makes the same guarantee for runnable threads; after parking,
             * nothing but a handful of never-again-read pops remains, so
             * reaping is safe. */
            pp = &z->next;
            continue;
        }
        *pp = z->next;
        z->next = reap;
        reap = z;
    }

    spinlock_release_irqrestore(&zombie_lock, rz);

    while (reap) {
        tcb_t *z = reap;
        reap = z->next;
        uint64_t reaped_frames = 0;
        if (z->pml4_phys) {
            /* Full user-half reaping.  A zombie is not running anywhere, but
             * with SMP the CPU doing the reaping may itself be executing on
             * the zombie's CR3 from kernel context (e.g. the thread exited on
             * this CPU and sched_yield() reaped it immediately): switch to the
             * kernel PML4 first so the walk/free never tears down the active
             * CR3.  The next user dispatch will load that thread's
             * pml4_phys again. */
            uint64_t cur_cr3 = read_cr3() & PAGE_ADDR_MASK;
            uint64_t kpml4   = paging_get_kernel_pml4();
            if (z->pml4_phys != kpml4) {
                if (cur_cr3 == z->pml4_phys) {
                    paging_switch_to(kpml4);
                }
                vma_free_all(&z->vma_list);
                reaped_frames = paging_free_address_space(z->pml4_phys);
                /* The freed frames go straight back to the PMM and can be
                 * re-allocated at once; other CPUs may still hold this dead
                 * space's VA->frame translations in their TLBs (no PCID
                 * tagging), where they would alias whatever the frames are
                 * reused for.  RESIDUE2 T1: use the precise O5 shootdown —
                 * the per-CPU CR3 filters it to exactly the CPUs that ran
                 * (or still run) this address space — instead of a raw
                 * broadcast IPI to every remote CPU. */
                if (smp_get_cpu_count() > 1) {
                    tlb_shootdown_range(z->pml4_phys, 0, 0);
                }
            }
            z->pml4_phys = 0;
        }
        /* RESIDUE2 T1: leave the parent/child process table before the TCB
         * is zeroed — after adoption the parent may be init, whose list
         * would otherwise keep a pointer to freed memory. */
        if (z->parent) {
            proc_children_unlink(z->parent, z);
        }
        kprintf("[thread] reaped '%s' (tid %llu, %llu frames)\n",
                z->name, (unsigned long long)z->id,
                (unsigned long long)reaped_frames);
        thread_deregister_tcb(z);
        thread_free_kernel_stack(z);
        memset(z, 0, sizeof(*z));
        slab_free(tcb_cache, z);
        zombies_reaped++;
    }
}

void thread_exit_with_signal(int signo) {
    tcb_t *self = sched_current();
    if (self) self->term_signal = signo;
    thread_exit_with_code(128 + signo);   /* legacy exit-code convention */
}

void thread_exit_with_code(int code) {
    /* P9 pthread teardown: if this thread was created with CLONE_CHILD_CLEARTID
     * (clear_tid_addr != 0), zero *clear_tid_addr in user memory and wake one
     * waiter on that futex so pthread_join() can observe completion.  Done in
     * normal context, before disabling interrupts. */
    {
        extern int copy_to_user(void *user_dst, const void *kernel_src, uint64_t len);
        extern int futex_wake(uint32_t *uaddr, int n);
        tcb_t *me = sched_current();
        if (me && me->clear_tid_addr) {
            uint32_t zero = 0;
            (void)copy_to_user((void *)(uintptr_t)me->clear_tid_addr, &zero, sizeof(zero));
            (void)futex_wake((uint32_t *)(uintptr_t)me->clear_tid_addr, 1);
            me->clear_tid_addr = 0;
        }
    }

    /* Disable interrupts: we're about to manipulate scheduler state. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *self = sched_current();
    self->state = THREAD_DEAD;
    self->exit_code = code;
    gui_cleanup_process(self->id);
    gpu_cleanup_process(self->id);
    socket_close_process(self->id);
    /* Q14: apply SEM_UNDO records and detach SysV shm segments. */
    sysvipc_cleanup_process(self);
    close_process_fds(self);
    kprintf("[thread] '%s' (tid %llu) exited (code=%d)\n",
            self->name, (unsigned long long)self->id, code);

    /*
     * P6a: orphan adoption — reparent all live and zombie children to init (PID 1).
     * Do NOT mark adopted zombies waited; init will reap them via waitpid().
     * This prevents live child->parent dangling pointers to a freed TCB.
     * Already-waited zombies are NOT adopted: they are already collected and
     * pending reap; do not count them toward init->n_children.
     */
    tcb_t *init_task = thread_get_by_pid(1);
    if (init_task && init_task != self) {
        /* RESIDUE2 T1: walk THIS thread's children list (the parent/child
         * process table) instead of the whole registry — O(children), and
         * children_lock keeps the sibling links stable against racing
         * forks/reapers on other CPUs. */
        uint64_t cf = spinlock_acquire_irqsave(&children_lock);
        int adopted = 0;
        tcb_t *c = self->child_head;
        while (c) {
            tcb_t *next = c->sibling;
            if (c->state == THREAD_DEAD && c->waited) {
                /* Already collected, pending reap: keep it off the table
                 * so no dangling parent pointer survives; the reaper will
                 * not need to unlink it. */
                c->parent = NULL;
                c->sibling = NULL;
            } else {
                c->parent = init_task;
                c->sibling = init_task->child_head;
                init_task->child_head = c;
                adopted++;
                /* Adopted zombies stay un-waited so init can collect them. */
            }
            c = next;
        }
        self->child_head = NULL;
        if (adopted) {
            init_task->n_children += adopted;
        }
        self->n_children = 0;
        spinlock_release_irqrestore(&children_lock, cf);
    } else {
        /* Fallback (no init yet, e.g. early boot): mark orphaned zombies
         * waited so they don't leak; live orphans get parent=NULL to avoid
         * use-after-free. */
        uint64_t cf = spinlock_acquire_irqsave(&children_lock);
        tcb_t *c = self->child_head;
        while (c) {
            tcb_t *next = c->sibling;
            c->parent = NULL;
            c->sibling = NULL;
            if (c->state == THREAD_DEAD && !c->waited) {
                c->waited = 1;
            }
            c = next;
        }
        self->child_head = NULL;
        self->n_children = 0;
        spinlock_release_irqrestore(&children_lock, cf);
    }

    /* If we have NO parent or it's a kernel thread (parent NULL), mark
     * ourselves as immediately collectable — no one will wait4 for us. */
    if (self->parent == NULL) {
        self->waited = 1;
    } else if (self->parent->state != THREAD_DEAD) {
        /* POSIX: a child's termination posts SIGCHLD to its parent. */
        signal_send(self->parent, SIGCHLD);
    }

    zombie_enqueue(self);

    /* schedule() picks the next thread and switches; since we're DEAD, we are
       not re-added to the queue.  We never return here. */
    schedule();

    /* Unreachable, but keep the compiler happy with noreturn. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
    (void)rflags;
}

void thread_exit(void) {
    thread_exit_with_code(0);
}
