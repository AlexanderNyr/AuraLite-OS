/* kernel/arch/i386/thread32.c -- kernel threads + preemptive
 * round-robin (I386_PLAN I4).
 *
 * Mirrors kernel/proc/{thread,scheduler}.c at bring-up scope:
 * static TCB table, kmalloc32'd kernel stacks, PIT-driven preemption,
 * BSP-only (plan D5).  The preemption point is deliberately placed
 * AFTER the EOI in irq32_dispatch -- switching stacks while the PIC
 * still awaits its acknowledgement would freeze IRQ0 for every thread
 * but the interrupted one (the x86_64 port learned this in phase 6;
 * cheaper to inherit the lesson than the debugging session).
 *
 * TSS.esp0 is refreshed on every switch: it is dead weight for
 * kernel->kernel switches but is the load-bearing wall the moment a
 * Ring 3 thread takes an interrupt (user32.c), and refreshing it
 * unconditionally here means Ring 3 cannot forget.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/thread32.h"
#include "kernel/arch/i386/kheap32.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/gdt.h"

/* context32.asm */
extern void context_switch32(uint32_t *save_esp, uint32_t new_esp);

struct tcb32 {
    uint32_t    esp;            /* saved kernel stack pointer          */
    uint32_t    kstack_base;    /* kmalloc32 block (0 for the boot tcb) */
    uint32_t    kstack_top;
    uint32_t    esp0;           /* TSS.esp0 while this thread runs --
                                 * a DEDICATED trap stack armed by
                                 * user32 before entering Ring 3, and
                                 * 0 for threads that never do.  See
                                 * the I7 lesson at thread32_set_esp0. */
    int         state;
    const char *name;
    thread32_fn fn;
    void       *arg;
};

static struct tcb32 tcbs[THREAD32_MAX];
static int current;                     /* index into tcbs             */
static volatile int need_resched;
static int sched_online;

int thread32_current_tid(void) { return current; }

int thread32_state(int tid)
{
    if (tid < 0 || tid >= THREAD32_MAX)
        return -1;
    return tcbs[tid].state;
}

const char *thread32_name(int tid)
{
    if (tid < 0 || tid >= THREAD32_MAX || !tcbs[tid].name)
        return "?";
    return tcbs[tid].name;
}

/* First-entry trampoline: context_switch32 "returns" here on a fresh
 * stack.  Interrupts are re-enabled explicitly -- the switch that got
 * us here ran inside the IRQ0 window with IF clear in the saved
 * EFLAGS image we fabricated. */
static void thread32_entry(void)
{
    __asm__ volatile("sti");
    struct tcb32 *me = &tcbs[current];
    me->fn(me->arg);
    thread32_exit();
}

/* boot32.asm: top of the boot stack thread 0 runs on. */
extern uint8_t boot_stack_top[];

void sched32_init(void)
{
    for (int i = 0; i < THREAD32_MAX; i++)
        tcbs[i].state = THREAD32_STATE_FREE;

    /* Adopt the boot context: kmain32's stack becomes thread 0's.  Its
     * esp is live (never read until the first switch stores it).
     * kstack_top is recorded so switch_to arms TSS.esp0 for thread 0
     * exactly like any other thread -- the I7 shell found the gap:
     * after the sched self-test, esp0 still pointed at the LAST
     * WORKER'S freed stack, and the boot thread's Ring 3 traps ran on
     * reclaimed heap until a nested spawn corrupted it (#PF with a
     * heap eip, measured).  A thread that can host user code must own
     * the esp0 it traps onto -- no exceptions, including thread 0. */
    tcbs[0].state      = THREAD32_STATE_READY;
    tcbs[0].name       = "boot/idle";
    tcbs[0].kstack_top = (uint32_t)boot_stack_top;
    current            = 0;
    sched_online       = 1;
    tss_set_esp0(tcbs[0].kstack_top);

    kprintf32("[sched] round-robin online (BSP-only, %u slots, "
              "%u KiB kernel stacks)\n",
              (uint32_t)THREAD32_MAX, THREAD32_KSTACK / 1024u);
}

int thread32_create(const char *name, thread32_fn fn, void *arg)
{
    int tid = -1;
    for (int i = 0; i < THREAD32_MAX; i++) {
        if (tcbs[i].state == THREAD32_STATE_FREE) {
            tid = i;
            break;
        }
    }
    if (tid < 0)
        return -1;

    uint8_t *stack = (uint8_t *)kmalloc32(THREAD32_KSTACK);
    if (!stack)
        return -1;

    struct tcb32 *t = &tcbs[tid];
    t->kstack_base = (uint32_t)stack;
    t->kstack_top  = (uint32_t)stack + THREAD32_KSTACK;
    t->name        = name;
    t->fn          = fn;
    t->arg         = arg;

    /* Fabricate the frame context_switch32 will pop:
     *   [ebp] [edi] [esi] [ebx] [eflags] [ret-eip=thread32_entry]
     * EFLAGS image: IF clear (entry does sti itself), bit 1 always set. */
    uint32_t *sp = (uint32_t *)t->kstack_top;
    *--sp = (uint32_t)thread32_entry;   /* ret target  */
    *--sp = 0x00000002;                 /* eflags      */
    *--sp = 0;                          /* ebx         */
    *--sp = 0;                          /* esi         */
    *--sp = 0;                          /* edi         */
    *--sp = 0;                          /* ebp         */
    t->esp = (uint32_t)sp;

    t->state = THREAD32_STATE_READY;    /* publish last */
    return tid;
}

static int pick_next(void)
{
    for (int off = 1; off <= THREAD32_MAX; off++) {
        int i = (current + off) % THREAD32_MAX;
        if (tcbs[i].state == THREAD32_STATE_READY)
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

    /* Arm the ring-transition stack for the incoming thread (see the
     * file comment).  esp0 (armed by user32 while the thread hosts a
     * Ring 3 image; see thread32_set_esp0) wins over kstack_top: the
     * setjmp-trampoline design keeps live C frames ON the kernel
     * stack while Ring 3 runs, so trapping onto kstack_top would
     * bulldoze them -- the trap needs its own stack. */
    if (tcbs[next].esp0)
        tss_set_esp0(tcbs[next].esp0);
    else if (tcbs[next].kstack_top)
        tss_set_esp0(tcbs[next].kstack_top);

    context_switch32(&tcbs[prev].esp, tcbs[next].esp);
}

void sched32_yield(void)
{
    __asm__ volatile("cli");
    switch_to(pick_next());
    __asm__ volatile("sti");
}

void thread32_exit(void)
{
    __asm__ volatile("cli");
    struct tcb32 *me = &tcbs[current];
    me->state = THREAD32_STATE_DONE;
    /* The stack is freed by the reaper in thread 0 (kmain32's idle
     * loop), never here -- we are standing on it. */
    switch_to(pick_next());
    /* A DONE thread is never picked again. */
    for (;;)
        __asm__ volatile("hlt");
}

/* Reap DONE threads from thread 0's idle loop: safe stack, no locks
 * needed BSP-only with interrupts off. */
void thread32_reap(void)
{
    __asm__ volatile("cli");
    for (int i = 1; i < THREAD32_MAX; i++) {
        if (tcbs[i].state == THREAD32_STATE_DONE) {
            if (tcbs[i].kstack_base)
                kfree32((void *)tcbs[i].kstack_base);
            tcbs[i].kstack_base = 0;
            tcbs[i].state = THREAD32_STATE_FREE;
        }
    }
    __asm__ volatile("sti");
}

/* I7: arm/disarm a dedicated Ring 3 trap stack for the CURRENT thread.
 *
 * The lesson, twice-measured, recorded once: TSS.esp0 must point at a
 * stack that is EMPTY at trap time.  kstack_top is not it -- the
 * setjmp-trampoline keeps user32_run_elf's live C frames on the kernel
 * stack while Ring 3 runs, and the first version (esp0 = kstack_top)
 * had every int 0x80 bulldoze those frames from the top down.  It
 * *appeared* to work while 16 KiB of headroom separated the frames
 * from the top; the nested `run` from the shell moved the frames close
 * enough for the trap to overwrite saved_esp/saved_ebp, and the parent
 * resumed into garbage (#PF at heap eip, then #PF at cr2=0x2a = the
 * child's exit code where a return address should be).  user32 now
 * allocates a trap stack per image and arms it here; disarm on exit. */
void thread32_set_esp0(uint32_t esp0)
{
    tcbs[current].esp0 = esp0;
    if (esp0)
        tss_set_esp0(esp0);
    else if (tcbs[current].kstack_top)
        tss_set_esp0(tcbs[current].kstack_top);
}

uint32_t thread32_current_esp0(void)
{
    return tcbs[current].esp0;
}

void sched32_tick(void)
{
    if (sched_online)
        need_resched = 1;
}

void sched32_maybe_preempt(void)
{
    if (!sched_online || !need_resched)
        return;
    need_resched = 0;
    /* Called from irq32_dispatch AFTER EOI, IF already clear (interrupt
     * gate).  The switched-out thread resumes exactly here when it is
     * next picked, unwinds the IRQ frame and iret's. */
    switch_to(pick_next());
}
