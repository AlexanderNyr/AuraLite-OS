/* apwake.c -- RESIDUE2 T2 / ledger RES-16: a device IRQ waking a hlt-ed AP.
 *
 * See apwake.h for the contract.  The interrupt path this exercise:
 *
 *   RTC device -> PIIX IRQ8 -> I/O APIC redirection GSI8 (re-aimed at the
 *   target AP's APIC ID at runtime) -> that AP's LAPIC -> wakes its `hlt`
 *   -> vector 40 -> irq_dispatch -> the handler here (which asserts the
 *   delivery cpu).  Every hop except the RTC itself was already proven
 *   separately; this test closes the "delivered to a CHOSEN AP, waking a
 *   hlt" pair that the ledger row names.
 */

#include <stdint.h>

#include "kernel/arch/x86_64/apwake.h"
#include "kernel/arch/x86_64/ioapic.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

/* How many device IRQs make the receipt, and how long to wait for them. */
#define APWAKE_TARGET   8
#define APWAKE_WAIT_MS 500

static volatile unsigned apwake_count;   /* RTC deliveries to the target AP */
static volatile int      apwake_done;
static volatile uint32_t apwake_cpu_seen; /* bitmap: cpus that delivered */
static volatile unsigned apwake_hlt_wakes; /* count advances seen on hlt exit */

/* CMOS RTC register access (index port with NMI mask, data port). */
#define RTC_INDEX 0x70
#define RTC_DATA  0x71
static uint8_t rtc_read(uint8_t reg) {
    outb(RTC_INDEX, (uint8_t)(0x80 | reg));
    return inb(RTC_DATA);
}
static void rtc_write(uint8_t reg, uint8_t val) {
    outb(RTC_INDEX, (uint8_t)(0x80 | reg));
    outb(RTC_DATA, val);
}

static void rtc_periodic_start(void) {
    /* Rate field 0b110 = 976.5 us period (~1 kHz), divider bits untouched. */
    uint8_t a = rtc_read(0x0A);
    rtc_write(0x0A, (uint8_t)((a & 0xF0) | 0x06));
    rtc_write(0x0B, (uint8_t)(rtc_read(0x0B) | 0x40));   /* PIE */
}

static void rtc_periodic_stop(void) {
    rtc_write(0x0B, (uint8_t)(rtc_read(0x0B) & (uint8_t)~0x40));
}

/* Runs in IRQ context ON the delivery cpu (GSI8 is aimed at exactly one). */
static void apwake_rtc_irq(struct registers *regs) {
    (void)regs;
    struct cpu_local *cl = cpu_local_ready ? get_cpu_local() : NULL;
    unsigned cpu = (unsigned)(cl ? cl->cpu_id : 0);
    apwake_cpu_seen |= (1u << cpu);
    if (apwake_count < 0xFFFFFFFu) {
        apwake_count++;
    }
    if (apwake_count >= APWAKE_TARGET) {
        apwake_done = 1;
    }
}

/* The hlt-ed victim: enqueued straight onto the target AP's run queue.
 * Every loop pass was ASLEEP in `hlt`; if the delivery count advanced
 * across the hlt, that advance is what woke us. */
static void apwake_hlt_thread(void *arg) {
    (void)arg;
    unsigned prev = 0;
    while (!apwake_done) {
        __asm__ volatile ("hlt");
        unsigned now = apwake_count;
        if (now > prev) {
            apwake_hlt_wakes += (now - prev);
            prev = now;
        }
    }
}

/* sched_add_thread() balances across cpus; the receipt needs the hlt
 * looper to EXECUTE on the target AP, so enqueue onto its queue directly.
 * Same claim/lock discipline as sched_add_thread (SMP 3.2). */
static void apwake_enqueue_on_cpu(tcb_t *t, uint32_t cpu_id) {
    struct cpu_local *c = (cpu_id == 0) ? &bsp_cpu_local
                                        : &ap_cpu_locals[cpu_id];
    if (__sync_lock_test_and_set(&t->on_queue, 1) != 0) {
        return;
    }
    uint64_t flags = spinlock_acquire_irqsave(&c->rq_lock);
    t->next = NULL;
    if (c->rq_tail) {
        c->rq_tail->next = t;
    } else {
        c->rq_head = t;
    }
    c->rq_tail = t;
    c->rq_len++;
    spinlock_release_irqrestore(&c->rq_lock, flags);
}

int smp_irq_ap_wake_selftest(void) {
    if (!apic_irq_mode) {
        kprintf("[smpwake] SKIP: not in I/O-APIC mode\n");
        return -1;
    }
    if (smp_get_schedulable_cpu_count() < 2) {
        kprintf("[smpwake] SKIP: need a second cpu (-smp >= 2)\n");
        return -1;
    }

    apwake_count = 0;
    apwake_done = 0;
    apwake_cpu_seen = 0;
    apwake_hlt_wakes = 0;

    uint32_t target_cpu = 1;                    /* first AP */
    uint32_t target_apic = smp_get_lapic_id(target_cpu);
    uint32_t bsp_apic = lapic_read_id();

    /* The hlt-ed victim, placed on the target AP before anything fires. */
    tcb_t *t = kthread_create_unstarted(apwake_hlt_thread, NULL,
                                        "apwake-hlt");
    if (!t) {
        kprintf("[smpwake] FAIL: could not create the hlt thread\n");
        return -1;
    }
    apwake_enqueue_on_cpu(t, target_cpu);

    /* The device: RTC periodic on ISA IRQ8 / GSI8, aimed at the target. */
    irq_register_handler(8, apwake_rtc_irq);
    if (ioapic_route_gsi(8, 32 + 8, target_apic) != 0) {
        kprintf("[smpwake] FAIL: could not route GSI8\n");
        irq_register_handler(8, NULL);
        return -1;
    }
    rtc_periodic_start();

    for (int waited = 0; waited < APWAKE_WAIT_MS && !apwake_done;
         waited += 10) {
        timer_sleep_ms(10);
    }

    /* Restore the machine: RTC off, GSI8 back to the BSP, handler off. */
    rtc_periodic_stop();
    ioapic_route_gsi(8, 32 + 8, bsp_apic);
    irq_register_handler(8, NULL);

    if (apwake_done && apwake_cpu_seen == (1u << target_cpu) &&
        apwake_hlt_wakes > 0) {
        kprintf("[smpwake] PASS: %u RTC(GSI8) device IRQs delivered to "
                "cpu%u (apic id %u); hlt looper woken %u times -- "
                "RES-16 receipt\n",
                apwake_count, target_cpu, target_apic, apwake_hlt_wakes);
        return 0;
    }
    kprintf("[smpwake] FAIL: count=%u cpu_seen=0x%x hlt_wakes=%u "
            "(target cpu%u apic %u)\n",
            apwake_count, apwake_cpu_seen, apwake_hlt_wakes,
            target_cpu, target_apic);
    return -1;
}
