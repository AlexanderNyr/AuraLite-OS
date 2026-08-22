/* scheduler.c — SMP-safe round-robin preemptive scheduler (H8).
 *
 * Ready queue: per-CPU singly-linked list (tail append, head dequeue = FIFO fairness),
 * protected by per-CPU rq_lock spinlock.
 * Each CPU tracks its own current and idle threads via MSR_GS_BASE (cpu_local).
 */

#include <stdint.h>
#include "kernel/proc/scheduler.h"
#include "kernel/arch/arch.h"
#include "kernel/proc/thread.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/slab.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

extern void context_switch(tcb_t *old, tcb_t *new);
extern void thread_entry(void);   /* trampoline, defined in thread.c */

extern int cpu_local_ready;
extern void sched_add_thread(tcb_t *tcb);
extern tcb_t *sched_steal_work(void);

static int    scheduler_ready = 0;
static uint64_t tid_counter   = 0;
spinlock_t sched_lock = SPINLOCK_UNLOCKED;

/* CPU usage accounting, per online CPU (see scheduler.h).  Every cpu ticks
 * from its own timer source -- the BSP from the PIT (IRQ0 via LINT0 ExtINT),
 * each AP from its calibrated Local APIC timer -- and sched_tick() charges
 * the slot indexed by the calling cpu's cpu_id.  The getters SUM all slots
 * so readers (procfs /proc/stat, sysmon) keep the same "cumulative ticks
 * system-wide" semantics the single-CPU version had.  32 == MAX_CPUS (kept
 * in sync with kernel/arch/x86_64/cpu_local.c's ap_cpu_locals[]). */
#define SCHED_MAX_CPUS 32
static volatile uint64_t cpu_total_ticks[SCHED_MAX_CPUS];
static volatile uint64_t cpu_idle_ticks[SCHED_MAX_CPUS];

uint64_t sched_get_total_ticks(void) {
    uint64_t sum = 0;
    for (int i = 0; i < SCHED_MAX_CPUS; i++) sum += cpu_total_ticks[i];
    return sum;
}
uint64_t sched_get_idle_ticks(void) {
    uint64_t sum = 0;
    for (int i = 0; i < SCHED_MAX_CPUS; i++) sum += cpu_idle_ticks[i];
    return sum;
}

int sched_is_ready(void) { return scheduler_ready; }

/* ---- Idle loop ---- */

/* The idle thread runs when no other thread is ready. It enables interrupts
 * and halts until the next IRQ (typically the PIT timer). */
static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        arch_wait_for_interrupt();
    }
}

/* ---- Core scheduler ---- */

void schedule(void) {
    /* Must be called with interrupts disabled. */
    if (!cpu_local_ready) return;
    struct cpu_local *local = get_cpu_local();
    if (!local) return;

    /* SMP step 3.2: real parallel scheduling on every online CPU.  The
     * per-CPU prerequisites that made this safe landed in step 3.1:
     *   - SYSCALL entry state lives in struct cpu_local (no shared slots),
     *   - tss_set_rsp0_for_cpu()/set_syscall_stack() publish this CPU's
     *     RSP0 / syscall stack instead of a BSP-global one,
     *   - the VMM's "current tables" pointer is per-CPU (paging.c),
     *   - the TLB shootdown IPI rides the normal ISR/IPI dispatch.
     * Below, everything selections are strictly per-CPU: local->current,
     * the local run queue (rq_lock), then cross-CPU work stealing. */
    tcb_t *old = local->current;
    tcb_t *next = NULL;
    {
        uint64_t flags = spinlock_acquire_irqsave(&local->rq_lock);
        next = local->rq_head;
        if (next) {
            local->rq_head = next->next;
            if (!local->rq_head) local->rq_tail = NULL;
            local->rq_len--;
            next->next = NULL;
        }
        spinlock_release_irqrestore(&local->rq_lock, flags);
        /* Dequeuing releases the queue-membership claim (tcb.on_queue). */
        if (next) __sync_lock_release(&next->on_queue);
    }

    if (!next) {
        next = sched_steal_work();
    }
    if (!next) {
        next = local->idle;
    }

    if (next && next != old) {
        /* Parking protocol (SMP 3.2, see thread.h): the picked thread may
         * have been published on a queue by a cpu that is STILL finishing
         * its context save.  Spinning here is safe against deadlock BECAUSE
         * of the order of the steps below: this cpu has ALREADY secured its
         * own next thread and does not touch any contended lock before
         * context_switch(), while the cpu we are waiting for only needs a
         * few plain stores to finish saving (it publishes `old` below and
         * then immediately saves).  An earlier draft spun BEFORE picking,
         * which provably deadlocked at -smp 4: two cpus each published an
         * un-parked thread onto the other cpu's queue and then blocked
         * forever waiting for each other. */
        while (next->switch_parked == 0) {
            arch_cpu_relax();
        }
        /* Establish the on-cpu side of the parking invariant: from now
         * until our context save completes, parked reads 0.  Remote wakers
         * that race to re-enqueue this thread while it later blocks rely on
         * this to make pickers spin until the save actually lands. */
        next->switch_parked = 0;
    }

    local->current = next;
    if (next) next->state = THREAD_RUNNING;

    if (next && next->kernel_stack) {
        uint64_t kstack_top = (uint64_t)next->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0_for_cpu((int)local->cpu_id, kstack_top);
        /* Per-CPU syscall entry stack slot (struct cpu_local), safe to
         * publish from every cpu now. */
        set_syscall_stack(kstack_top);
    }

    if (next && next->pml4_phys != 0) {
        /* R5 (ledger RES-15): the one-time receipt that a USER
         * thread runs off the BSP.  The runqueues, least-loaded
         * placement and stealing landed at SMP 3.2; the status row
         * saying "BSP-only" outlived the code -- this line is the
         * measured answer, printed once so no smoke output drifts. */
        static volatile int ap_user_receipt_done;
        struct cpu_local *cl = get_cpu_local();
        if (cl->cpu_id != 0 && !ap_user_receipt_done &&
            !__sync_lock_test_and_set(&ap_user_receipt_done, 1)) {
            kprintf("[sched] R5 receipt: user thread pid=%d on AP cpu=%u\n",
                    (int)next->id, (unsigned)cl->cpu_id);
        }
        paging_switch_to(next->pml4_phys);
    } else if (next) {
        /* Idle/kernel threads have no address space of their own.  Do NOT
         * simply keep the previous user process' CR3 (the pre-SMP trick):
         * on SMP that process can exit and have its whole address space --
         * including the PML4 frame itself -- reaped by ANOTHER CPU (its
         * "is cur_cr3 == zombie pml4?" guard only protects the CPU doing
         * the reaping).  This CPU would keep translating through a freed,
         * reused frame and triple-fault at a random later instruction
         * (observed: #PF fetching kernel text under a stale CR3 right
         * after /hello exited while this cpu sat in its idle loop).
         * Idle therefore always runs on the KERNEL address space. */
        uint64_t kpml4 = paging_get_kernel_pml4();
        uint64_t cur = read_cr3() & PAGE_ADDR_MASK;
        if (cur != kpml4 && kpml4 != 0) {
            paging_switch_to(kpml4);
        }
    }

    /* Publish the still-READY old thread LAST, right before switching away:
     * the enqueue makes it visible to every cpu (including remote ones via
     * the load balancer) while its context is still unsaved -- the parking
     * flag covers exactly this window.  Doing it after the pick (rather
     * than before) is what keeps the picker-side spin deadlock-free. */
    if (old != NULL && old != local->idle && old->state == THREAD_READY) {
        old->switch_parked = 0;
        /* Claim the queue slot atomically (tcb.on_queue): a remote waker
         * (signal_tick sleep wakeup / wait-queue wake on another cpu) may
         * have flipped this thread BLOCKED->READY and already enqueued it
         * in the window between the thread's BLOCKED store and reaching
         * this schedule().  Exactly one claimer may perform the enqueue,
         * otherwise the same thread lands on two run queues and two cpus
         * tear its saved context apart with interleaved save/restore. */
        if (__sync_lock_test_and_set(&old->on_queue, 1) == 0) {
            sched_add_thread(old);
        }
    }

    if (old != next && old != NULL && next != NULL) {
        context_switch(old, next);
    }
}

void sched_yield(void) {
    /* Free dead threads from a safe stack before voluntarily switching away. */
    thread_reap_zombies();
    arch_irqflags_t rflags = arch_irq_save();
    if (cpu_local_ready) {
        struct cpu_local *local = get_cpu_local();
        if (local && local->current != NULL && local->current != local->idle) {
            local->current->state = THREAD_READY;
        }
    }
    schedule();
    arch_irq_restore(rflags);
}

void kernel_block_current(void) {
    arch_irqflags_t rflags = arch_irq_save();
    tcb_t *cur = sched_current();
    if (cur) cur->state = THREAD_BLOCKED;
    schedule();
    arch_irq_restore(rflags);
}

void sched_tick(void) {
    if (!scheduler_ready || !cpu_local_ready) {
        return;
    }
    struct cpu_local *local = get_cpu_local();
    if (!local || local->current == NULL) {
        return;
    }
    /* Per-CPU usage accounting: this cpu's own timer source ticked us (PIT
     * on the BSP, the calibrated LAPIC timer on each AP), so charge this
     * cpu's slot.  cpu_id < 32 by construction (cpu_local.c). */
    if (local->cpu_id < SCHED_MAX_CPUS) {
        cpu_total_ticks[local->cpu_id]++;
        if (local->current == local->idle) {
            cpu_idle_ticks[local->cpu_id]++;
        }
    }
    /* We are inside the timer IRQ handler, so IF is already clear. */
    if (local->current == local->idle) {
        /* Always try to switch away from idle. */
        schedule();
        return;
    }
    local->current->quantum--;
    if (local->current->quantum == 0) {
        local->current->quantum = SCHED_QUANTUM;
        local->current->state = THREAD_READY;
        schedule();
    }
}

tcb_t *sched_current(void) {
    if (!cpu_local_ready) return NULL;
    struct cpu_local *local = get_cpu_local();
    return local ? local->current : NULL;
}

/* ---- AP Idle entry point ---- */

static void setup_stack(tcb_t *tcb, void (*fn)(void *), void *arg);

void sched_idle(void) {
    struct cpu_local *local = get_cpu_local();
    if (!local) return;

    tcb_t *idle = slab_alloc(tcb_cache);
    if (!idle) return;
    memset(idle, 0, sizeof(tcb_t));
    idle->kernel_stack = NULL;
    idle->kernel_stack_region = NULL;
    idle->kernel_stack_slot = -1;
    if (thread_alloc_kernel_stack(idle) != 0) {
        slab_free(tcb_cache, idle);
        return;
    }
    idle->id = __sync_fetch_and_add(&tid_counter, 1);
    idle->state = THREAD_RUNNING;
    idle->quantum = 1;
    idle->umask = 0022;
    idle->switch_parked = 1;   /* first-run frame is fully crafted below */
    strncpy(idle->name, "ap-idle", THREAD_NAME_MAX - 1);
    setup_stack(idle, idle_loop, NULL);
    thread_register_tcb(idle);

    local->idle = idle;
    local->current = idle;

    for (;;) {
        /* SMP step 3.2: sti (not cli) so this AP's own LAPIC timer (started
         * in ap_entry() before we got here) can preempt us into sched_tick()
         * -> schedule(), which is how real threads reach this CPU.  Between
         * ticks the core simply sleeps on hlt with interrupts on -- the idle
         * TCB above keeps the accounting/stealing story consistent. */
        arch_wait_for_interrupt();
    }
}

/* ---- Gate self-test: two interleaving threads ---- */

static volatile int test_threads_done = 0;

static void test_thread_fn(void *arg) {
    const char *name = (const char *)arg;
    for (int i = 0; i < 4; i++) {
        kprintf("[sched] %s: message %d (tid %llu, tick %llu)\n",
                name, i,
                (unsigned long long)sched_current()->id,
                (unsigned long long)timer_get_ticks());
        sched_yield();
    }
    kprintf("[sched] %s: finished\n", name);
    test_threads_done++;
}

void scheduler_self_test(void) {
    kprintf("[sched] self-test: creating two threads...\n");
    kthread_create(test_thread_fn, "thread-A", "thread-A");
    kthread_create(test_thread_fn, "thread-B", "thread-B");

    /* Each thread prints 4 messages and yields 4 times.  20 kmain yields
       gives them ample scheduling slots to complete. */
    for (int i = 0; i < 20; i++) {
        sched_yield();
    }

    kprintf("[sched] test complete: %d/2 threads finished\n", test_threads_done);
    if (test_threads_done >= 2) {
        kprintf("[sched] PASS: two threads interleaved correctly\n");
    } else {
        kprintf("[sched] FAIL: not all threads completed\n");
    }
}

/* ---- Initialisation ---- */

static void setup_stack(tcb_t *tcb, void (*fn)(void *), void *arg) {
    tcb->entry = fn;
    tcb->arg   = arg;
    uint64_t *sp = (uint64_t *)((uint8_t *)tcb->kernel_stack + THREAD_STACK_SIZE);
    sp--; *sp = 0;                      /* extra alignment padding        */
    sp--; *sp = 0;                      /* alignment padding              */
    sp--; *sp = (uint64_t)thread_entry; /* ret target (trampoline)        */
    sp--; *sp = 0;  /* rbx */
    sp--; *sp = 0;  /* rbp */
    sp--; *sp = 0;  /* r12 */
    sp--; *sp = 0;  /* r13 */
    sp--; *sp = 0;  /* r14 */
    sp--; *sp = 0;  /* r15 */
    sp--; *sp = 0x202;  /* RFLAGS (IF set) */
    tcb->rsp = (uint64_t)sp;
}

void sched_init(void) {
    spinlock_init(&sched_lock);
    struct cpu_local *local = get_cpu_local();
    if (!local) return;

    /* 1) Create the "kmain" TCB representing the currently-running context. */
    tcb_t *kmain_thread = slab_alloc(tcb_cache);
    if (kmain_thread == NULL) {
        kprintf("[sched] FATAL: cannot allocate the kmain TCB; "
                "scheduler disabled\n");
        return;
    }
    memset(kmain_thread, 0, sizeof(tcb_t));
    kmain_thread->id      = __sync_fetch_and_add(&tid_counter, 1);
    kmain_thread->state   = THREAD_RUNNING;
    kmain_thread->quantum = SCHED_QUANTUM;
    kmain_thread->umask   = 0022;
    /* kmain is RUNNING right now, so by the SMP 3.2 parking invariant
     * ("0 while on-cpu, 1 once saved") it starts un-parked; the first real
     * context_switch away from it will set the flag. */
    kmain_thread->switch_parked = 0;
    strncpy(kmain_thread->name, "kmain", THREAD_NAME_MAX - 1);

    /* 2) Create the idle thread. */
    tcb_t *idle_thread = slab_alloc(tcb_cache);
    if (idle_thread == NULL) return;
    memset(idle_thread, 0, sizeof(tcb_t));
    idle_thread->kernel_stack = NULL;
    idle_thread->kernel_stack_region = NULL;
    idle_thread->kernel_stack_slot = -1;
    if (thread_alloc_kernel_stack(idle_thread) != 0) {
        slab_free(tcb_cache, idle_thread);
        return;
    }
    idle_thread->id      = __sync_fetch_and_add(&tid_counter, 1);
    idle_thread->state   = THREAD_READY;
    idle_thread->quantum = 1;
    idle_thread->umask   = 0022;
    idle_thread->switch_parked = 1;   /* first-run frame crafted below */
    strncpy(idle_thread->name, "idle", THREAD_NAME_MAX - 1);
    setup_stack(idle_thread, idle_loop, NULL);

    local->current = kmain_thread;
    local->idle = idle_thread;

    thread_register_tcb(kmain_thread);
    thread_register_tcb(idle_thread);

    scheduler_ready = 1;

    kprintf("[sched] scheduler initialised: kmain (tid %llu) + idle (tid %llu)\n",
            (unsigned long long)kmain_thread->id,
            (unsigned long long)idle_thread->id);
}
