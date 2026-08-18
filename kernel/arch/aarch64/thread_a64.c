/* kernel/arch/aarch64/thread_a64.c -- kernel threads + preemptive
 * round-robin (ARM64_PLAN A4).
 *
 * thread_rv.c's shape at the fourth arch: static TCB table,
 * kmalloc_a64'd kernel stacks, timer-driven preemption, boot CPU only
 * (plan D5).  The preemption point is AFTER the GIC dispatch returns
 * in a64_trap -- the post-EOI placement's GIC flavour: EOIR has
 * completed the timer INTID and the TVAL write has un-asserted the
 * line, so a context switch cannot park a still-asserted interrupt
 * (the exact freeze the x86_64 port measured in phase 6; inherited
 * as a design input through two ports now).
 *
 * No TSS on this ISA either -- and no scratch CSR dance: the EL0
 * trap-stack contract is the hardware's own SPSel switch (any EL0
 * trap lands on SP_EL1), so "arm the trap stack" is just "hold it in
 * sp when eret'ing down" (see user_enter_a64).  The I7 esp0 lesson
 * costs ZERO instructions here; it is an architecture feature.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/aarch64/thread_a64.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/pl011.h"

/* context_a64.S */
extern void context_switch_a64(uint64_t *save_sp, uint64_t new_sp);

struct tcb_a64 {
    uint64_t    sp;             /* saved kernel stack pointer            */
    uint64_t    kstack_base;    /* kmalloc_a64 block (0 for the boot tcb) */
    uint64_t    kstack_top;
    int         state;
    const char *name;
    thread_a64_fn fn;
    void       *arg;
};

static struct tcb_a64 tcbs[THREAD_A64_MAX];
static int current;
static volatile int need_resched;
static int sched_online;

int thread_a64_current_tid(void) { return current; }

int thread_a64_state(int tid)
{
    if (tid < 0 || tid >= THREAD_A64_MAX)
        return -1;
    return tcbs[tid].state;
}

const char *thread_a64_name(int tid)
{
    if (tid < 0 || tid >= THREAD_A64_MAX || !tcbs[tid].name)
        return "?";
    return tcbs[tid].name;
}

/* Interrupt gating: DAIF.I off/on around scheduler internals -- the
 * cli/sti pair's fourth spelling. */
static inline void irq_off(void)
{
    __asm__ volatile("msr daifset, #2");
}
static inline void irq_on(void)
{
    __asm__ volatile("msr daifclr, #2");
}

/* First-entry trampoline: context_switch_a64 "returns" here on a
 * fresh stack.  Interrupts re-enabled explicitly -- the switch that
 * got us here ran inside the timer-trap window with IRQ masked. */
static void thread_entry_a64(void)
{
    irq_on();
    struct tcb_a64 *me = &tcbs[current];
    me->fn(me->arg);
    thread_a64_exit();
}

/* boot.S: top of the boot stack thread 0 runs on. */
extern uint8_t boot_stack_top[];

void sched_a64_init(void)
{
    for (int i = 0; i < THREAD_A64_MAX; i++)
        tcbs[i].state = THREAD_A64_STATE_FREE;

    /* Adopt the boot context: kmain_a64's stack becomes thread 0's. */
    tcbs[0].state      = THREAD_A64_STATE_READY;
    tcbs[0].name       = "boot/idle";
    tcbs[0].kstack_top = (uint64_t)boot_stack_top;
    current            = 0;
    sched_online       = 1;

    pl011_puts("[sched] round-robin online (boot cpu only, ");
    pl011_putdec64(THREAD_A64_MAX);
    pl011_puts(" slots, ");
    pl011_putdec64(THREAD_A64_KSTACK / 1024);
    pl011_puts(" KiB kernel stacks, 624-byte switch frames: FPU eager)\n");
}

int thread_a64_create(const char *name, thread_a64_fn fn, void *arg)
{
    int tid = -1;
    for (int i = 0; i < THREAD_A64_MAX; i++) {
        if (tcbs[i].state == THREAD_A64_STATE_FREE) {
            tid = i;
            break;
        }
    }
    if (tid < 0)
        return -1;

    uint8_t *stack = (uint8_t *)kmalloc_a64(THREAD_A64_KSTACK);
    if (!stack)
        return -1;

    struct tcb_a64 *t = &tcbs[tid];
    t->kstack_base = (uint64_t)stack;
    t->kstack_top  = ((uint64_t)stack + THREAD_A64_KSTACK) & ~15UL;
    t->name        = name;
    t->fn          = fn;
    t->arg         = arg;

    /* Fabricate the frame context_switch_a64 will pop: 624 bytes,
     * lr (offset 88 -- the second half of the x29/x30 pair) pointing
     * at the trampoline, everything else zero (528 zero bytes of FPU
     * state IS the correct initial FPU state). */
    uint64_t *sp = (uint64_t *)(t->kstack_top - 624);
    for (int i = 0; i < 624 / 8; i++)
        sp[i] = 0;
    sp[11] = (uint64_t)thread_entry_a64;    /* [88] = x30/lr */
    t->sp = (uint64_t)sp;

    t->state = THREAD_A64_STATE_READY;      /* publish last */
    return tid;
}

static int pick_next(void)
{
    for (int off = 1; off <= THREAD_A64_MAX; off++) {
        int i = (current + off) % THREAD_A64_MAX;
        if (tcbs[i].state == THREAD_A64_STATE_READY)
            return i;
    }
    return current;
}

static void switch_to(int next)
{
    if (next == current)
        return;
    int prev = current;
    current  = next;
    context_switch_a64(&tcbs[prev].sp, tcbs[next].sp);
}

void sched_a64_yield(void)
{
    irq_off();
    switch_to(pick_next());
    irq_on();
}

void thread_a64_exit(void)
{
    irq_off();
    tcbs[current].state = THREAD_A64_STATE_DONE;
    /* The stack is freed by the reaper in thread 0, never here -- we
     * are standing on it. */
    switch_to(pick_next());
    for (;;)
        __asm__ volatile("wfi");
}

void thread_a64_reap(void)
{
    irq_off();
    for (int i = 1; i < THREAD_A64_MAX; i++) {
        if (tcbs[i].state == THREAD_A64_STATE_DONE) {
            if (tcbs[i].kstack_base)
                kfree_a64((void *)tcbs[i].kstack_base);
            tcbs[i].kstack_base = 0;
            tcbs[i].state = THREAD_A64_STATE_FREE;
        }
    }
    irq_on();
}

void sched_a64_tick(void)
{
    if (sched_online)
        need_resched = 1;
}

void sched_a64_maybe_preempt(void)
{
    if (!sched_online || !need_resched)
        return;
    need_resched = 0;
    /* Called from a64_trap AFTER gic_dispatch returned (post-EOI).
     * IRQ is masked inside the trap.  The switched-out thread
     * resumes exactly here when next picked, unwinds the trap frame
     * and erets. */
    switch_to(pick_next());
}
