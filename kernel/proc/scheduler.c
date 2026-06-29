/* scheduler.c — SMP-safe round-robin preemptive scheduler (H8).
 *
 * Ready queue: singly-linked list (tail append, head dequeue = FIFO fairness),
 * protected by sched_lock spinlock.
 * Each CPU tracks its own current and idle threads via MSR_GS_BASE (cpu_local).
 */

#include <stdint.h>
#include "kernel/proc/scheduler.h"
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

static spinlock_t sched_lock;
static tcb_t *queue_head      = NULL;   /* head = next to run  */
static tcb_t *queue_tail      = NULL;   /* tail = last to run  */
static int    scheduler_ready = 0;
static uint64_t tid_counter   = 0;

/* ---- Run queue operations ---- */

void sched_add_thread(tcb_t *tcb) {
    if (!tcb) return;
    uint64_t flags = spinlock_acquire_irqsave(&sched_lock);
    tcb->next = NULL;
    if (queue_tail) {
        queue_tail->next = tcb;
    } else {
        queue_head = tcb;
    }
    queue_tail = tcb;
    spinlock_release_irqrestore(&sched_lock, flags);
}

static tcb_t *dequeue(void) {
    uint64_t flags = spinlock_acquire_irqsave(&sched_lock);
    tcb_t *t = queue_head;
    if (t) {
        queue_head = t->next;
        if (queue_head == NULL) {
            queue_tail = NULL;
        }
        t->next = NULL;
    }
    spinlock_release_irqrestore(&sched_lock, flags);
    return t;
}

/* ---- Idle thread: halts until an interrupt wakes the CPU ---- */

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* ---- Core scheduler ---- */

void schedule(void) {
    /* Must be called with interrupts disabled. */
    if (!cpu_local_ready) return;
    struct cpu_local *local = get_cpu_local();
    if (!local) return;
    tcb_t *old = local->current;

    if (old != NULL && old != local->idle && old->state == THREAD_READY) {
        sched_add_thread(old);
    }

    tcb_t *next = dequeue();
    if (next == NULL) {
        next = local->idle;       /* nothing ready: run idle */
    }

    local->current = next;
    if (next) next->state = THREAD_RUNNING;

    if (next && next->kernel_stack && local->cpu_id == 0) {
        /* Only update global TSS/syscall MSRs on BSP until per-CPU TSS lands */
        uint64_t kstack_top = (uint64_t)next->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0(kstack_top);
        set_syscall_stack(kstack_top);
    }

    /* Switch address space if the new thread has its own PML4 (BSP only for now). */
    if (next && next->pml4_phys != 0 && local->cpu_id == 0) {
        paging_switch_to(next->pml4_phys);
    }

    if (old != next && old != NULL && next != NULL) {
        context_switch(old, next);
    }
}

void sched_yield(void) {
    /* Free dead threads from a safe stack before voluntarily switching away. */
    thread_reap_zombies();
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    if (cpu_local_ready) {
        struct cpu_local *local = get_cpu_local();
        if (local && local->current != NULL && local->current != local->idle) {
            local->current->state = THREAD_READY;
        }
    }
    schedule();
    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void sched_tick(void) {
    if (!scheduler_ready || !cpu_local_ready) {
        return;
    }
    struct cpu_local *local = get_cpu_local();
    if (!local || local->current == NULL) {
        return;
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
    strncpy(idle->name, "ap-idle", THREAD_NAME_MAX - 1);
    setup_stack(idle, idle_loop, NULL);
    thread_register_tcb(idle);

    local->idle = idle;
    local->current = idle;

    __asm__ volatile ("sti");
    for (;;) {
        __asm__ volatile ("hlt");
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
    memset(kmain_thread, 0, sizeof(tcb_t));
    kmain_thread->id      = __sync_fetch_and_add(&tid_counter, 1);
    kmain_thread->state   = THREAD_RUNNING;
    kmain_thread->quantum = SCHED_QUANTUM;
    kmain_thread->umask   = 0022;
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
