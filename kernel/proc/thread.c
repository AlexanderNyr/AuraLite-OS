/* thread.c — kernel thread creation, initial stack setup, and exit.
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

/* Implemented in context.asm */
extern void context_switch(tcb_t *old, tcb_t *new);

static uint64_t next_tid = 1;

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
        return NULL;
    }

    tcb->kernel_stack = stack;
    tcb->id           = next_tid++;
    tcb->state        = THREAD_READY;
    tcb->quantum      = SCHED_QUANTUM;
    if (name != NULL) {
        strncpy(tcb->name, name, THREAD_NAME_MAX - 1);
    }

    setup_initial_stack(tcb, fn, arg);

    sched_add_thread(tcb);
    return tcb;
}

void thread_exit(void) {
    /* Disable interrupts: we're about to manipulate scheduler state. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *self = sched_current();
    self->state = THREAD_DEAD;
    kprintf("[thread] '%s' (tid %llu) exited\n",
            self->name, (unsigned long long)self->id);

    /* If a parent is waiting in wait4(), clear the flag so its busy-wait
     * loop exits. We do NOT add it to the queue — sched_yield already keeps
     * it cycling through the queue. */
    if (self->parent && self->parent->waited_on) {
        self->parent->waited_on = 0;
    }

    /* schedule() picks the next thread and switches; since we're DEAD, we are
       not re-added to the queue.  We never return here. */
    schedule();

    /* Unreachable, but keep the compiler happy with noreturn. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
