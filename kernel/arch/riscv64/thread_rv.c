/* kernel/arch/riscv64/thread_rv.c -- kernel threads + preemptive
 * round-robin (RISCV_PLAN V4).
 *
 * thread32.c's shape at LP64: static TCB table, kmalloc_rv'd kernel
 * stacks, timer-driven preemption, boot-hart only (plan D5).  The
 * preemption point is AFTER the timer re-arm in rv_trap -- the
 * post-EOI placement's SBI flavour: sbi_set_timer both re-arms and
 * clears STIP, so switching before it would leave the timer dead for
 * every thread but the interrupted one (the exact freeze the x86_64
 * port measured in phase 6; inherited here as a design input).
 *
 * No TSS on this ISA -- the U-mode trap-stack contract lives in
 * sscratch instead, armed by user_rv.c per image (the I7 esp0 lesson;
 * see trapentry.S's swap-and-test entry).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/riscv64/thread_rv.h"
#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/sbi.h"

/* context_rv.S */
extern void context_switch_rv(uint64_t *save_sp, uint64_t new_sp);

struct tcb_rv {
    uint64_t    sp;             /* saved kernel stack pointer           */
    uint64_t    kstack_base;    /* kmalloc_rv block (0 for the boot tcb) */
    uint64_t    kstack_top;
    int         state;
    const char *name;
    thread_rv_fn fn;
    void       *arg;
};

static struct tcb_rv tcbs[THREAD_RV_MAX];
static int current;
static volatile int need_resched;
static int sched_online;

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

int thread_rv_current_tid(void) { return current; }

int thread_rv_state(int tid)
{
    if (tid < 0 || tid >= THREAD_RV_MAX)
        return -1;
    return tcbs[tid].state;
}

const char *thread_rv_name(int tid)
{
    if (tid < 0 || tid >= THREAD_RV_MAX || !tcbs[tid].name)
        return "?";
    return tcbs[tid].name;
}

/* Interrupt gating: sstatus.SIE off/on around scheduler internals --
 * the cli/sti pair's CSR spelling. */
static inline void irq_off(void)
{
    __asm__ volatile("csrci sstatus, 2");
}
static inline void irq_on(void)
{
    __asm__ volatile("csrsi sstatus, 2");
}

/* First-entry trampoline: context_switch_rv "returns" here on a fresh
 * stack.  Interrupts re-enabled explicitly -- the switch that got us
 * here ran inside the timer-trap window with SIE clear. */
static void thread_entry_rv(void)
{
    irq_on();
    struct tcb_rv *me = &tcbs[current];
    me->fn(me->arg);
    thread_rv_exit();
}

/* boot.S: top of the boot stack thread 0 runs on. */
extern uint8_t boot_stack_top[];

void sched_rv_init(void)
{
    for (int i = 0; i < THREAD_RV_MAX; i++)
        tcbs[i].state = THREAD_RV_STATE_FREE;

    /* Adopt the boot context: kmain_rv's stack becomes thread 0's. */
    tcbs[0].state      = THREAD_RV_STATE_READY;
    tcbs[0].name       = "boot/idle";
    tcbs[0].kstack_top = (uint64_t)boot_stack_top;
    current            = 0;
    sched_online       = 1;

    sbi_puts("[sched] round-robin online (boot hart only, ");
    put_udec_(THREAD_RV_MAX);
    sbi_puts(" slots, ");
    put_udec_(THREAD_RV_KSTACK / 1024);
    sbi_puts(" KiB kernel stacks)\n");
}

int thread_rv_create(const char *name, thread_rv_fn fn, void *arg)
{
    int tid = -1;
    for (int i = 0; i < THREAD_RV_MAX; i++) {
        if (tcbs[i].state == THREAD_RV_STATE_FREE) {
            tid = i;
            break;
        }
    }
    if (tid < 0)
        return -1;

    uint8_t *stack = (uint8_t *)kmalloc_rv(THREAD_RV_KSTACK);
    if (!stack)
        return -1;

    struct tcb_rv *t = &tcbs[tid];
    t->kstack_base = (uint64_t)stack;
    t->kstack_top  = ((uint64_t)stack + THREAD_RV_KSTACK) & ~15UL;
    t->name        = name;
    t->fn          = fn;
    t->arg         = arg;

    /* Fabricate the frame context_switch_rv will pop: 14 slots, ra at
     * offset 0 pointing at the trampoline, s-registers zero. */
    uint64_t *sp = (uint64_t *)(t->kstack_top - 112);
    sp[0] = (uint64_t)thread_entry_rv;      /* ra */
    for (int i = 1; i < 14; i++)
        sp[i] = 0;
    t->sp = (uint64_t)sp;

    t->state = THREAD_RV_STATE_READY;       /* publish last */
    return tid;
}

static int pick_next(void)
{
    for (int off = 1; off <= THREAD_RV_MAX; off++) {
        int i = (current + off) % THREAD_RV_MAX;
        if (tcbs[i].state == THREAD_RV_STATE_READY)
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
    context_switch_rv(&tcbs[prev].sp, tcbs[next].sp);
}

void sched_rv_yield(void)
{
    irq_off();
    switch_to(pick_next());
    irq_on();
}

void thread_rv_exit(void)
{
    irq_off();
    tcbs[current].state = THREAD_RV_STATE_DONE;
    /* The stack is freed by the reaper in thread 0, never here -- we
     * are standing on it. */
    switch_to(pick_next());
    for (;;)
        __asm__ volatile("wfi");
}

void thread_rv_reap(void)
{
    irq_off();
    for (int i = 1; i < THREAD_RV_MAX; i++) {
        if (tcbs[i].state == THREAD_RV_STATE_DONE) {
            if (tcbs[i].kstack_base)
                kfree_rv((void *)tcbs[i].kstack_base);
            tcbs[i].kstack_base = 0;
            tcbs[i].state = THREAD_RV_STATE_FREE;
        }
    }
    irq_on();
}

void sched_rv_tick(void)
{
    if (sched_online)
        need_resched = 1;
}

void sched_rv_maybe_preempt(void)
{
    if (!sched_online || !need_resched)
        return;
    need_resched = 0;
    /* Called from rv_trap AFTER sbi_set_timer re-armed (the post-EOI
     * placement).  SIE is clear inside the trap.  The switched-out
     * thread resumes exactly here when next picked, unwinds the trap
     * frame and sret's. */
    switch_to(pick_next());
}
