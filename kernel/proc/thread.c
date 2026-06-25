/* thread.c — kernel thread creation, initial stack setup, exit and reaping.
 *
 * A new thread's stack is crafted so that context_switch's first "restore +
 * ret" sequence lands at thread_entry(), which calls fn(arg) and then
 * thread_exit().
 */

#include <stdint.h>
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/gui/gui.h"
#include "kernel/net/socket.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "drivers/timer/pit.h"

/* Implemented in context.asm */
extern void context_switch(tcb_t *old, tcb_t *new);

static uint64_t next_tid = 1;

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

    void *stack = kmalloc(THREAD_STACK_SIZE);
    if (stack == NULL) {
        kprintf("[thread] FATAL: kmalloc failed for stack\n");
        kfree(tcb);
        return NULL;
    }

    tcb->kernel_stack = stack;
    tcb->id           = next_tid++;
    tcb->state        = THREAD_READY;
    tcb->quantum      = SCHED_QUANTUM;
    if (name != NULL) {
        strncpy(tcb->name, name, THREAD_NAME_MAX - 1);
        tcb->name[THREAD_NAME_MAX - 1] = 0;
    }

    setup_initial_stack(tcb, fn, arg);
    /* New threads start with cloexec cleared and the fd table zeroed
     * (kmalloc + memset above). */
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

int64_t do_wait4_pid(int64_t pid, int64_t *exit_code) {
    tcb_t *self = sched_current();
    if (!self) return -1;
    uint64_t parent_id = self->id;

    for (;;) {
        uint64_t rflags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
        tcb_t *z = thread_find_zombie(parent_id, pid);

        if (z) {
            int code = z->exit_code;
            uint64_t reaped_pid = z->id;
            /* Mark the zombie as collected; the next sched_yield() in any
             * thread will release the TCB + stack + (eventually) address
             * space.  We deliberately do NOT call thread_reap_zombies()
             * synchronously here: doing so from inside the syscall path of
             * the wait4()er has historically raced with other context
             * switches that still reference the dying thread's metadata. */
            z->waited = 1;
            if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
            if (exit_code) *exit_code = (int64_t)code;
            return (int64_t)reaped_pid;
        }

        /* No matching child has exited.  Decide whether one ever could. */
        int has_living_child = 0;
        /* We don't keep a per-parent child list; for now we scan the run queue
         * implicitly through sched (no public API).  Treat "no zombies and the
         * caller is asking for a specific PID that isn't queued" as a
         * permanent miss to avoid spinning forever in the wrong place.  The
         * common path (pid<0) yields and retries until something dies. */
        if (pid >= 0) {
            /* If the target PID is still on the zombie list but already
             * waited, return -1.  Otherwise we have to keep yielding. */
            for (tcb_t *zw = zombie_head; zw; zw = zw->next) {
                if (zw->id == (uint64_t)pid && zw->waited) {
                    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
                    return -1;
                }
            }
        }
        (void)has_living_child;
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        sched_yield();
    }
}

static void close_process_fds(tcb_t *t) {
    if (!t) return;
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        t->fd_table[fd].in_use = 0;
        t->fd_table[fd].vn = NULL;
        t->fd_table[fd].pos = 0;
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
            /* Full user-half reaping is wired up via
             * paging_free_address_space(), but a still-active page-walk by
             * another thread (notably the shell while a child runs under a
             * sibling syscall) makes it unsafe to free aggressively until
             * we add proper TLB shootdown + cross-PML4 reference counting.
             *
             * For now we only free zombies whose address space is currently
             * unreachable from any other context: the PML4 must differ from
             * the live CR3 AND from the kernel PML4.  Other zombies leak
             * their user frames (matching previous behaviour) until that
             * infrastructure lands.
             */
            uint64_t cur_cr3 = read_cr3() & PAGE_ADDR_MASK;
            uint64_t kpml4   = paging_get_kernel_pml4();
            if (cur_cr3 != z->pml4_phys && z->pml4_phys != kpml4) {
                /* TEMPORARILY DISABLED — see comment above.  Re-enable once
                 * the full reaping path is proven race-free in tests. */
                /* reaped_frames = paging_free_address_space(z->pml4_phys); */
                (void)kpml4;
            }
            z->pml4_phys = 0;
        }
        kprintf("[thread] reaped '%s' (tid %llu, %llu frames)\n",
                z->name, (unsigned long long)z->id,
                (unsigned long long)reaped_frames);
        if (z->kernel_stack) kfree(z->kernel_stack);
        kfree(z);
        zombies_reaped++;
    }
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
