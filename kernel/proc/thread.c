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

tcb_t *thread_find_zombie(uint64_t parent_pid, int64_t match_pid) {
    /* Takes zombie_lock itself: with SMP, racing thread_exit()/reapers run
     * on OTHER CPUs where a local cli buys nothing. */
    uint64_t flags = spinlock_acquire_irqsave(&zombie_lock);
    tcb_t *found = NULL;
    for (tcb_t *z = zombie_head; z; z = z->next) {
        if (z->state != THREAD_DEAD || z->waited) continue;
        uint64_t z_parent_id = z->parent ? z->parent->id : 0;
        if (z_parent_id != parent_pid) continue;
        if (match_pid >= 0 && z->id != (uint64_t)match_pid) continue;
        found = z;
        break;
    }
    spinlock_release_irqrestore(&zombie_lock, flags);
    return found;
}

/* WNOHANG / WUNTRACED option bits (match libc sys/wait.h). */
#define WAIT_WNOHANG  1
#define WAIT_WUNTRACED 2

/* Does zombie @z match the wait selector @pid for parent @parent_id?
 *   pid > 0  : z->id == pid
 *   pid == 0 : z->pgid == parent's pgid
 *   pid == -1: any child
 *   pid < -1 : z->pgid == |pid|
 */
static int wait_zombie_matches(tcb_t *z, int64_t pid, uint64_t parent_id,
                               int64_t parent_pgid) {
    uint64_t zp = z->parent ? z->parent->id : 0;
    if (zp != parent_id) return 0;
    if (pid > 0)        return z->id == (uint64_t)pid;
    if (pid == 0)       return z->pgid == parent_pgid;
    if (pid == -1)      return 1;
    return z->pgid == -pid;   /* pid < -1: group |pid| */
}

/* Build a POSIX wait-status word from a reaped zombie. */
static int wait_status_of(tcb_t *z) {
    if (z->term_signal) return z->term_signal & 0x7f;       /* WIFSIGNALED */
    return (z->exit_code & 0xff) << 8;                       /* WIFEXITED */
}

/* P6a: waitpid child selector */
static int wait_child_matches(tcb_t *parent, tcb_t *child, int64_t pid) {
    if (!parent || !child) return 0;
    if (child->parent != parent) return 0;
    if (pid > 0)  return child->id == (uint64_t)pid;
    if (pid == -1) return 1;
    if (pid == 0)  return child->pgid == parent->pgid;
    /* pid < -1 */
    return child->pgid == -pid;
}

int64_t do_waitpid(int64_t pid, int *status, int options) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    if (options & ~(WAIT_WNOHANG | WAIT_WUNTRACED)) return -EINVAL;
    uint64_t parent_id = self->id;
    int64_t parent_pgid = self->pgid;

    for (;;) {
        /* Find a matching, not-yet-collected zombie (zombie_lock: a child's
         * thread_exit() may be running on another CPU right now). */
        uint64_t zf = spinlock_acquire_irqsave(&zombie_lock);
        tcb_t *match = NULL;
        for (tcb_t *z = zombie_head; z; z = z->next) {
            if (z->state != THREAD_DEAD || z->waited) continue;
            if (wait_zombie_matches(z, pid, parent_id, parent_pgid)) { match = z; break; }
        }
        if (match) {
            int st = wait_status_of(match);
            uint64_t reaped = match->id;
            match->waited = 1;                 /* reaper releases it later */
            if (self->n_children > 0) self->n_children--;
            spinlock_release_irqrestore(&zombie_lock, zf);
            if (status) *status = st;
            return (int64_t)reaped;
        }
        spinlock_release_irqrestore(&zombie_lock, zf);

        /* WUNTRACED (FIX_R6): report a matching child that has STOPPED and
         * whose stop has not been reported yet — once per stop
         * (stop_notified is reset when it stops again).  Status encoding:
         * 0x7f | (stopsig << 8), matching libc's WIFSTOPPED/WSTOPSIG. */
        if (options & WAIT_WUNTRACED) {
            uint64_t sf = spinlock_acquire_irqsave(&thread_registry_lock);
            for (int i = 0; i < all_threads_count; i++) {
                tcb_t *c = all_threads[i];
                if (!wait_child_matches(self, c, pid)) continue;
                if (c->state != THREAD_STOPPED || c->stop_notified ||
                    !c->stop_signal) continue;
                c->stop_notified = 1;
                int st = 0x7f | (c->stop_signal << 8);
                uint64_t who = c->id;
                spinlock_release_irqrestore(&thread_registry_lock, sf);
                if (status) *status = st;
                return (int64_t)who;
            }
            spinlock_release_irqrestore(&thread_registry_lock, sf);
        }

        /* No matching zombie: scan all TCBs to see if a matching child exists.
         * Skip already-waited zombies — they do not count as children. */
        uint64_t rf = spinlock_acquire_irqsave(&thread_registry_lock);
        int have_matching_child = 0;
        for (int i = 0; i < all_threads_count; i++) {
            tcb_t *c = all_threads[i];
            if (!wait_child_matches(self, c, pid)) continue;
            if (c->state == THREAD_DEAD && c->waited) continue; /* already reaped/collected */
            have_matching_child = 1;
            break;
        }
        spinlock_release_irqrestore(&thread_registry_lock, rf);

        if (!have_matching_child) return -ECHILD;
        if (options & WAIT_WNOHANG) return 0;   /* matching child exists, none ready */
        /* O7: block until a child exits (zombie_enqueue wakes us) with a
         * 5-tick net — it bounds both the lost-wakeup window (a child
         * exiting between the scan above and the sleep) and WUNTRACED
         * stop events, which have no waker hook yet. */
        wq_wait_deadline(&child_exit_wq, NULL, timer_get_ticks() + 5);
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
                 * reused for.  Shoot down every remote TLB (handler reloads
                 * CR3 = full flush) before returning them to circulation. */
                if (smp_get_cpu_count() > 1) {
                    lapic_send_ipi_all_excluding_self(IPI_TLB_SHOOTDOWN_VECTOR);
                }
            }
            z->pml4_phys = 0;
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
        /* Registry lock: with APs scheduling real threads, another CPU may
         * be inside thread_register_tcb/thread_reap_zombies right now; the
         * cli above only fences THIS cpu. */
        uint64_t rf = spinlock_acquire_irqsave(&thread_registry_lock);
        int adopted = 0;
        for (int i = 0; i < all_threads_count; i++) {
            tcb_t *c = all_threads[i];
            if (c->parent == self) {
                /* Skip already-waited zombies: they are already collected,
                 * pending thread_reap_zombies(), do not adopt or count. */
                if (c->state == THREAD_DEAD && c->waited) {
                    c->parent = NULL;  /* avoid dangling pointer to freed TCB */
                    continue;
                }
                c->parent = init_task;
                adopted++;
                /* Adopted zombies stay un-waited so init can collect them. */
            }
        }
        if (adopted) {
            init_task->n_children += adopted;
            self->n_children -= adopted;
            if (self->n_children < 0) self->n_children = 0;
        }
        /* Any remaining n_children should be already-waited zombies that we
         * deliberately did not adopt; clear the counter to maintain invariant. */
        self->n_children = 0;
        spinlock_release_irqrestore(&thread_registry_lock, rf);
    } else {
        /* Fallback (no init yet, e.g. early boot): mark orphaned zombies
         * waited so they don't leak; live orphans get parent=NULL to avoid
         * use-after-free. */
        uint64_t rf = spinlock_acquire_irqsave(&thread_registry_lock);
        for (int i = 0; i < all_threads_count; i++) {
            tcb_t *c = all_threads[i];
            if (c->parent == self) {
                c->parent = NULL;
                if (c->state == THREAD_DEAD && !c->waited) {
                    c->waited = 1;
                }
            }
        }
        spinlock_release_irqrestore(&thread_registry_lock, rf);
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
