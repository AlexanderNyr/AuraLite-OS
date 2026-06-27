/* thread.c — kernel thread creation, initial stack setup, exit and reaping.
 *
 * A new thread's stack is crafted so that context_switch's first "restore +
 * ret" sequence lands at thread_entry(), which calls fn(arg) and then
 * thread_exit().
 */

#include <stdint.h>
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/errno.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/gui/gui.h"
#include "kernel/net/socket.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "drivers/timer/pit.h"

/* Implemented in context.asm */
extern void context_switch(tcb_t *old, tcb_t *new);

static uint64_t next_tid = 1;

#define THREAD_STACK_SLOT_SIZE  ((THREAD_STACK_PAGES + 2 * THREAD_STACK_GUARD_PAGES) * 4096ULL)
#define THREAD_STACK_REGION_BASE 0xFFFFFFFF8A000000ULL
#define THREAD_STACK_MAX_SLOTS   128

static uint8_t thread_stack_slots[THREAD_STACK_MAX_SLOTS];
static spinlock_t thread_stack_lock = SPINLOCK_UNLOCKED;

static tcb_t *all_threads[128];
static int all_threads_count = 0;

void thread_register_tcb(tcb_t *tcb) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    if (all_threads_count < 128) {
        all_threads[all_threads_count++] = tcb;
    }
    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
}

void thread_deregister_tcb(tcb_t *tcb) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    for (int i = 0; i < all_threads_count; i++) {
        if (all_threads[i] == tcb) {
            all_threads[i] = all_threads[all_threads_count - 1];
            all_threads_count--;
            break;
        }
    }
    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
}

tcb_t *thread_get_by_pid(uint64_t pid) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    tcb_t *found = NULL;
    for (int i = 0; i < all_threads_count; i++) {
        if (all_threads[i]->id == pid) {
            found = all_threads[i];
            break;
        }
    }
    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
    return found;
}

int thread_get_all(tcb_t *out_list[], int max) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    int n = 0;
    for (int i = 0; i < all_threads_count && n < max; i++) {
        out_list[n++] = all_threads[i];
    }
    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
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

tcb_t *kthread_create(void (*fn)(void *), void *arg, const char *name) {
    tcb_t *tcb = kmalloc(sizeof(tcb_t));
    if (tcb == NULL) {
        kprintf("[thread] FATAL: kmalloc failed for TCB\n");
        return NULL;
    }
    memset(tcb, 0, sizeof(tcb_t));

    tcb->kernel_stack = NULL;
    tcb->kernel_stack_region = NULL;
    tcb->kernel_stack_slot = -1;
    if (thread_alloc_kernel_stack(tcb) != 0) {
        kprintf("[thread] FATAL: could not allocate guarded kernel stack\n");
        kfree(tcb);
        return NULL;
    }

    tcb->id           = next_tid++;
    tcb->state        = THREAD_READY;
    tcb->quantum      = SCHED_QUANTUM;
    /* Default: each new task is its own process group and session leader.
     * fork()/spawn() override pgid/sid to inherit from the parent; setsid()/
     * setpgid() change them explicitly (P6). */
    tcb->pgid         = (int64_t)tcb->id;
    tcb->sid          = (int64_t)tcb->id;
    if (name != NULL) {
        strncpy(tcb->name, name, THREAD_NAME_MAX - 1);
        tcb->name[THREAD_NAME_MAX - 1] = 0;
    }

    setup_initial_stack(tcb, fn, arg);
    /* New threads start with cloexec cleared and the fd table zeroed
     * (kmalloc + memset above). */
    thread_register_tcb(tcb);
    sched_add_thread(tcb);
    return tcb;
}

/* ---- Zombie / wait4 helpers ---- */

tcb_t *thread_find_zombie(uint64_t parent_pid, int64_t match_pid) {
    /* Caller must hold IF=0 if it cares about racing with thread_exit. */
    for (tcb_t *z = zombie_head; z; z = z->next) {
        if (z->state != THREAD_DEAD || z->waited) continue;
        uint64_t z_parent_id = z->parent ? z->parent->id : 0;
        if (z_parent_id != parent_pid) continue;
        if (match_pid >= 0 && z->id != (uint64_t)match_pid) continue;
        return z;
    }
    return NULL;
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

int64_t do_waitpid(int64_t pid, int *status, int options) {
    tcb_t *self = sched_current();
    if (!self) return -EINVAL;
    uint64_t parent_id = self->id;
    int64_t parent_pgid = self->pgid;

    for (;;) {
        uint64_t rflags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

        /* Find a matching, not-yet-collected zombie. */
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
            if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
            if (status) *status = st;
            return (int64_t)reaped;
        }

        /* No ready child.  ECHILD if we have no (live or zombie) children that
         * could match; otherwise either return 0 (WNOHANG) or yield + retry. */
        int have_candidate = (self->n_children > 0);
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");

        if (!have_candidate) return -ECHILD;
        if (options & WAIT_WNOHANG) return 0;   /* none ready right now */
        sched_yield();
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
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        if (t->fd_table[fd] != NULL) {
            vfs_close(fd);
        }
    }
}

static void zombie_enqueue(tcb_t *t) {
    t->next = zombie_head;
    zombie_head = t;
    zombies_queued++;
}

void thread_reap_zombies(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *current = sched_current();
    tcb_t *reap = NULL;
    tcb_t **pp = &zombie_head;
    while (*pp) {
        tcb_t *z = *pp;
        if (z == current || !z->waited) {
            /* Either still the running thread (impossible since we are alive)
             * or a zombie nobody has wait4()ed yet — leave it in place. */
            pp = &z->next;
            continue;
        }
        *pp = z->next;
        z->next = reap;
        reap = z;
    }

    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");

    while (reap) {
        tcb_t *z = reap;
        reap = z->next;
        uint64_t reaped_frames = 0;
        if (z->pml4_phys) {
            /* Full user-half reaping.  This kernel's scheduler is still
             * single-CPU, and user address spaces are not shared between live
             * TCBs, so no remote TLB shootdown is required here.  If the CPU is
             * currently running on the zombie's CR3 from kernel context, switch
             * to the kernel PML4 first and then free the unreachable user half. */
            uint64_t cur_cr3 = read_cr3() & PAGE_ADDR_MASK;
            uint64_t kpml4   = paging_get_kernel_pml4();
            if (z->pml4_phys != kpml4) {
                /* Single-CPU scheduler: if a kernel path is still running on
                 * the dead process' CR3, first move to the kernel PML4 so the
                 * page-table walk/free never tears down the active CR3.  The
                 * next user dispatch will load that thread's pml4_phys again. */
                if (cur_cr3 == z->pml4_phys) {
                    paging_switch_to(kpml4);
                }
                reaped_frames = paging_free_address_space(z->pml4_phys);
            }
            z->pml4_phys = 0;
        }
        kprintf("[thread] reaped '%s' (tid %llu, %llu frames)\n",
                z->name, (unsigned long long)z->id,
                (unsigned long long)reaped_frames);
        thread_deregister_tcb(z);
        thread_free_kernel_stack(z);
        memset(z, 0, sizeof(*z));
        kfree(z);
        zombies_reaped++;
    }
}

void thread_exit_with_signal(int signo) {
    tcb_t *self = sched_current();
    if (self) self->term_signal = signo;
    thread_exit_with_code(128 + signo);   /* legacy exit-code convention */
}

void thread_exit_with_code(int code) {
    /* Disable interrupts: we're about to manipulate scheduler state. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *self = sched_current();
    self->state = THREAD_DEAD;
    self->exit_code = code;
    gui_cleanup_process(self->id);
    socket_close_process(self->id);
    close_process_fds(self);
    kprintf("[thread] '%s' (tid %llu) exited (code=%d)\n",
            self->name, (unsigned long long)self->id, code);

    /*
     * Adopt our own children: any zombie whose parent is `self` would
     * otherwise leak forever (their parent will never wait4).  Mark them
     * `waited` so the next reaper sweep cleans them up.  Also rewrite the
     * parent pointer of any LIVING children to NULL so future exits don't
     * dereference a freed TCB.
     */
    for (tcb_t *z = zombie_head; z; z = z->next) {
        if (z->parent == self) {
            z->parent = NULL;
            if (!z->waited) z->waited = 1;
        }
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
