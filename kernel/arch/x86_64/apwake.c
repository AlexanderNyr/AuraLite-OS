/* RES-16: RTC -> GSI8 -> target LAPIC -> IRQ wakes the scheduler's idle HLT. */
#include <stdint.h>

#include "kernel/arch/x86_64/apwake.h"
#include "kernel/arch/x86_64/ioapic.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

#define APWAKE_TARGET   8
#define APWAKE_WAIT_MS 1000
#define RTC_IRQ         8
#define RTC_INDEX    0x70
#define RTC_DATA     0x71
#define RTC_PIE      0x40
#define RTC_PF       0x40
#define RTC_IRQF     0x80

extern const char x86_sched_idle_resume[];

static spinlock_t test_lock = SPINLOCK_UNLOCKED;
static spinlock_t rtc_lock = SPINLOCK_UNLOCKED;
static int active;
static unsigned count, hlt_wakes;
static uint32_t cpu_seen;

/* Access the index/data pair with interrupts disabled under rtc_lock. */
static uint8_t rtc_read(uint8_t reg) {
    outb(RTC_INDEX, reg);
    return inb(RTC_DATA);
}

static void rtc_write(uint8_t reg, uint8_t val) {
    outb(RTC_INDEX, reg);
    outb(RTC_DATA, val);
}

static void apwake_rtc_irq(struct registers *regs) {
    uint64_t flags = spinlock_acquire_irqsave(&rtc_lock);
    /* Reading register C acknowledges the device, not just its LAPIC.
     * Without this, later periods need not produce another rising edge. */
    if (active && (rtc_read(0x0C) & (RTC_PF | RTC_IRQF)) == (RTC_PF | RTC_IRQF)) {
        cpu_seen |= 1u << get_cpu_local()->cpu_id;
        count++;
        if (regs->rip == (uintptr_t)x86_sched_idle_resume) hlt_wakes++;
    }
    spinlock_release_irqrestore(&rtc_lock, flags);
}

int smp_irq_ap_wake_selftest(void) {
    if (!apic_irq_mode || smp_get_schedulable_cpu_count() < 2) {
        kprintf("[smpwake] SKIP: need I/O APIC and at least two CPUs\n");
        return -1;
    }
    if (!spinlock_try_acquire(&test_lock)) return -1;

    const uint32_t target_cpu = 1;
    uint32_t target_apic = smp_get_lapic_id(target_cpu);
    uint64_t saved_route;
    uint64_t flags = spinlock_acquire_irqsave(&rtc_lock);
    irq_handler_t old_handler = irq_exchange_handler(RTC_IRQ, apwake_rtc_irq);
    if (ioapic_route_gsi(RTC_IRQ, PIC_OFFSET + RTC_IRQ, target_apic, &saved_route) != 0) {
        irq_exchange_handler(RTC_IRQ, old_handler);
        spinlock_release_irqrestore(&rtc_lock, flags);
        spinlock_release(&test_lock);
        kprintf("[smpwake] FAIL: could not route GSI8\n");
        return -1;
    }
    count = hlt_wakes = cpu_seen = 0;
    uint8_t saved_a = rtc_read(0x0A);
    uint8_t saved_b = rtc_read(0x0B);
    /* Run at 64 Hz, not in a burst that can finish before CPU 1 goes idle.
     * The real idle thread never leaves its CPU, unlike a queued kthread. */
    rtc_write(0x0A, (uint8_t)((saved_a & 0x70) | 0x0A));
    (void)rtc_read(0x0C);
    active = 1;
    rtc_write(0x0B, saved_b | RTC_PIE);
    spinlock_release_irqrestore(&rtc_lock, flags);

    for (int waited = 0; waited < APWAKE_WAIT_MS; waited += 10) {
        timer_sleep_ms(10);
        flags = spinlock_acquire_irqsave(&rtc_lock);
        int done = count >= APWAKE_TARGET && hlt_wakes != 0;
        spinlock_release_irqrestore(&rtc_lock, flags);
        if (done) break;
    }

    flags = spinlock_acquire_irqsave(&rtc_lock);
    active = 0;
    rtc_write(0x0B, saved_b & (uint8_t)~RTC_PIE);
    (void)rtc_read(0x0C);
    ioapic_restore_gsi(RTC_IRQ, saved_route);
    irq_exchange_handler(RTC_IRQ, old_handler);
    rtc_write(0x0A, saved_a & 0x7F);
    rtc_write(0x0B, saved_b);
    unsigned observed = count, wakes = hlt_wakes;
    uint32_t cpus = cpu_seen;
    spinlock_release_irqrestore(&rtc_lock, flags);
    spinlock_release(&test_lock);

    if (observed >= APWAKE_TARGET && cpus == (1u << target_cpu) && wakes > 0) {
        kprintf("[smpwake] PASS: %u RTC(GSI8) device IRQs delivered to "
                "cpu%u (apic id %u); idle HLT woken %u times -- RES-16 receipt\n",
                observed, target_cpu, target_apic, wakes);
        return 0;
    }
    kprintf("[smpwake] FAIL: count=%u cpu_seen=0x%x hlt_wakes=%u "
            "(target cpu%u apic %u)\n", observed, cpus, wakes, target_cpu, target_apic);
    return -1;
}
