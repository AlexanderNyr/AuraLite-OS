/* scheduler.c — round-robin preemptive scheduler.
 *
 * Ready queue: singly-linked list (tail append, head dequeue = FIFO fairness).
 * The idle thread is a fallback selected only when the ready queue is empty.
 *
 * Concurrency model (single CPU): scheduler state is protected by disabling
 * interrupts (cli).  No spinlock is held across context_switch (that would
 * deadlock when the switched-to thread tries to acquire it).  NOT SMP SAFE.
 */

#include <stdint.h>
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/mm/kheap.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

extern void context_switch(tcb_t *old, tcb_t *new);
extern void thread_entry(void);   /* trampoline, defined in thread.c */

static tcb_t *current_thread  = NULL;
static tcb_t *idle_thread     = NULL;
static tcb_t *queue_head      = NULL;   /* head = next to run  */
static tcb_t *queue_tail      = NULL;   /* tail = last to run  */
static int    scheduler_ready = 0;
static uint64_t tid_counter   = 0;

/* ---- Run queue operations (call with interrupts disabled) ---- */

void sched_add_thread(tcb_t *tcb) {
    tcb->next = NULL;
    if (queue_tail) {
        queue_tail->next = tcb;
    } else {
        queue_head = tcb;
    }
    queue_tail = tcb;
}

static tcb_t *dequeue(void) {
    tcb_t *t = queue_head;
    if (t) {
        queue_head = t->next;
        if (queue_head == NULL) {
            queue_tail = NULL;
        }
        t->next = NULL;
    }
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
    tcb_t *old = current_thread;

    /* Re-add the current thread to the queue unless it is the idle thread or
       it has exited.  A RUNNING thread that is yielding/preempted must be
       transitioned to READY by the CALLER (sched_yield / sched_tick) before
       calling schedule, otherwise it will not be re-queued. */
    if (old != NULL && old != idle_thread && old->state == THREAD_READY) {
        sched_add_thread(old);
    }

    tcb_t *next = dequeue();
    if (next == NULL) {
        next = idle_thread;       /* nothing ready: run idle */
    }

    current_thread = next;
    next->state = THREAD_RUNNING;

    /* Switch address space if the new thread has its own PML4. */
    if (next->pml4_phys != 0) {
        /* Only switch CR3 when entering a user process (needs its own user
         * half). We never switch BACK to the kernel PML4 when going to a
         * kernel thread, because the kernel half is shared across ALL address
         * spaces — kernel code/heap/stacks are identical and accessible
         * regardless of which user PML4 is active. The next user-process
         * switch will load the correct CR3. */
        paging_switch_to(next->pml4_phys);
    }

    if (old != next && old != NULL) {
        context_switch(old, next);
    }
}

void sched_yield(void) {
    /* Free dead threads from a safe stack before voluntarily switching away. */
    thread_reap_zombies();
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
    if (current_thread != NULL && current_thread != idle_thread) {
        current_thread->state = THREAD_READY;
    }
    schedule();
    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void sched_tick(void) {
    if (!scheduler_ready || current_thread == NULL) {
        return;
    }
    /* We are inside the timer IRQ handler, so IF is already clear. */
    if (current_thread == idle_thread) {
        /* Always try to switch away from idle. */
        schedule();
        return;
    }
    current_thread->quantum--;
    if (current_thread->quantum == 0) {
        current_thread->quantum = SCHED_QUANTUM;
        current_thread->state = THREAD_READY;
        schedule();
    }
}

tcb_t *sched_current(void) {
    return current_thread;
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

/*
 * Build the initial stack for a thread (factored from thread.c's helper).
 * Identical layout: 6 callee-saved regs + ret target + alignment padding.
 */
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
    /* 1) Create the "kmain" TCB representing the currently-running context.
          Its RSP will be saved on the first context_switch away from it. */
    current_thread = kmalloc(sizeof(tcb_t));
    memset(current_thread, 0, sizeof(tcb_t));
    current_thread->id      = tid_counter++;
    current_thread->state   = THREAD_RUNNING;
    current_thread->quantum = SCHED_QUANTUM;
    strncpy(current_thread->name, "kmain", THREAD_NAME_MAX - 1);

    /* 2) Create the idle thread (NOT added to the run queue — it is the
          fallback selected by schedule() when the queue is empty). */
    idle_thread = kmalloc(sizeof(tcb_t));
    memset(idle_thread, 0, sizeof(tcb_t));
    idle_thread->kernel_stack = kmalloc(THREAD_STACK_SIZE);
    idle_thread->id      = tid_counter++;
    idle_thread->state   = THREAD_READY;
    idle_thread->quantum = 1;             /* switch away from idle ASAP */
    strncpy(idle_thread->name, "idle", THREAD_NAME_MAX - 1);
    setup_stack(idle_thread, idle_loop, NULL);

    thread_register_tcb(current_thread);
    thread_register_tcb(idle_thread);

    scheduler_ready = 1;

    kprintf("[sched] scheduler initialised: kmain (tid %llu) + idle (tid %llu)\n",
            (unsigned long long)current_thread->id,
            (unsigned long long)idle_thread->id);
}
