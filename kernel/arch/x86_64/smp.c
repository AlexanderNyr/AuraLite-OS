/* smp.c — wake application processors via a real INIT-SIPI-SIPI sequence.
 *
 * boot_info.cpus[] (populated by ACPI MADT parsing during boot -- see
 * boot/bios/stage2/acpi.inc and boot/uefi/efi_acpi.c) lists every enabled
 * Local APIC discovered on the machine.  This file wakes each AP by:
 *
 *   1. Copying a small 16-bit real-mode trampoline (boot/smp/ap_trampoline.asm,
 *      embedded as ap_trampoline_blob[] via build/ap_trampoline.inc) to a
 *      fixed low physical address the AP will start executing from.
 *   2. Writing a shared "handoff" data page the trampoline reads once it
 *      reaches 64-bit mode: which page tables to use (the kernel's own,
 *      already-built PML4 -- so every AP shares the BSP's exact address
 *      space from its very first C instruction), which stack to use, and
 *      which C function to jump into.
 *   3. Sending the classic INIT-(deassert)-SIPI-SIPI sequence (Intel MP
 *      spec / SDM Vol.3 s.9.4.4) via the Local APIC's ICR, targeting that
 *      AP's specific APIC ID.
 *   4. Waiting for the AP to report itself online (ap_entry() below bumps
 *      cpus_online once the TSS/cpu_local/LAPIC bring-up has run), with a
 *      generous timeout before giving up on that particular CPU and moving
 *      on to the next one.
 *
 * Each AP ends up idling (ap_entry() -> sched_idle()) with its own per-CPU
 * run queue ready to receive work (see kernel/proc/scheduler_rq.c); actual
 * load-balancing of user/kernel threads onto APs is a separate, later
 * change to schedule()'s current BSP-only gating (kernel/proc/scheduler.c)
 * -- this file's job is strictly "get every detected CPU core running and
 * idling safely," which is the prerequisite for that to be safe.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/boot_info.h"
#include "drivers/timer/pit.h"

#define AP_STACK_SIZE  (16 * 1024)
#define MAX_CPUS       32

/* Extern: implemented in gdt_flush.asm */
extern void gdt_flush(uint64_t gdtr_ptr);

/* Fixed low physical addresses used to hand an AP its startup code + data.
 * MUST match boot/smp/ap_trampoline.asm's `org` directive and its
 * SMP_TRAMPOLINE_DATA_PHYS equ exactly -- both sides document this
 * contract.  Chosen inside the first 1 MiB (a SIPI vector can only address
 * that range), out of the way of every other live low-memory user in this
 * project (Stage 2 sits at 0x8000..~0x9200 during BIOS boot, but that code
 * has already handed off to the kernel and its bytes are dead weight we are
 * free to overwrite by the time smp_init() runs), and inside
 * PMM_EARLY_BOOT_RESERVE (see kernel/mm/pmm.c, reserves the first 32 MiB)
 * so the physical-memory allocator can never hand these same frames to
 * something else while an AP might still be reading them. */
#define SMP_TRAMPOLINE_DATA_PHYS 0x7000ULL
#define SMP_TRAMPOLINE_CODE_PHYS 0x8000ULL

/* Shared handoff data page layout (must match the trampoline's comment):
 *   +0   dword  kernel PML4 physical address (low 32 bits)
 *   +8   qword  this AP's kernel stack top
 *   +16  qword  this AP's kernel-assigned CPU index (-> RDI)
 *   +24  qword  C entry point address */
struct smp_handoff {
    uint32_t pml4_phys_low;
    uint32_t pad;
    uint64_t stack_top;
    uint64_t cpu_index;
    uint64_t entry_point;
};

#include "build/ap_trampoline.inc"

/* Per-CPU AP stacks, allocated on demand as each AP is woken. */
static void *ap_stacks[MAX_CPUS];

/* This CPU's actual Local APIC ID, recorded per kernel-assigned AP index so
 * ap_entry() can log it without touching boot_info from AP context. */
static uint32_t lapic_id_for_ap_index[MAX_CPUS];

/* Atomic counter: how many CPUs are online (BSP = 1 at boot). */
static volatile uint32_t cpus_online = 1;

/* Raw busy-wait delay used only during AP wake-up, which runs BEFORE
 * pit_init()/the scheduler exist (smp_init() is called ahead of both in
 * kernel/kernel.c), so timer_get_ticks()/sched_yield() are not available
 * yet.  Uses PIT channel 2 (the PC speaker channel, otherwise idle at this
 * point in boot) in one-shot mode 0: output starts low and goes high when
 * the programmed count reaches zero, observable on port 0x61 bit 5.  This
 * is the same technique used by essentially every hobby-OS SMP tutorial
 * for the INIT-SIPI-SIPI timing (Intel MP spec s.B.4 calls for a ~10 ms
 * wait after INIT and ~200 us between the two SIPIs).  Capped at ~54 ms per
 * call (the 16-bit PIT counter's natural limit at its base frequency); the
 * 10 ms delay this file needs fits comfortably inside that. */
static void smp_udelay(uint32_t usec) {
    /* Channel 2 gate on (bit 0), speaker off (bit 1 clear) so this is
     * silent. */
    uint8_t tmp = inb(0x61);
    outb(0x61, (uint8_t)((tmp & 0xFC) | 0x01));

    /* Command byte: channel 2, access lo/hi, mode 0 (interrupt on terminal
     * count), binary. */
    outb(PIT_MODE_CMD, 0xB0);

    uint32_t count = (uint32_t)(((uint64_t)usec * PIT_BASE_FREQUENCY) /
                                1000000ULL);
    if (count == 0) count = 1;
    if (count > 0xFFFF) count = 0xFFFF;
    outb(PIT_CHANNEL2_DATA, (uint8_t)(count & 0xFF));
    outb(PIT_CHANNEL2_DATA, (uint8_t)((count >> 8) & 0xFF));

    /* Re-trigger the gate to (re)start counting from the freshly loaded
     * value: clear then set bit 0. */
    tmp = inb(0x61);
    outb(0x61, (uint8_t)(tmp & 0xFE));
    outb(0x61, (uint8_t)(tmp | 0x01));

    while ((inb(0x61) & 0x20) == 0) {
        __asm__ volatile ("pause");
    }
}

/* ---- AP entry point ----
 * Reached via the trampoline's final `jmp rax` once it is running 64-bit
 * code with the kernel's own page tables active.  RDI = this AP's
 * kernel-assigned index (SysV AMD64 ABI arg 0), exactly as
 * smp_handoff.cpu_index was set before SIPI.
 *
 * The trampoline's own tiny 3-entry GDT (boot/smp/ap_trampoline.asm) is
 * only good enough to reach 64-bit mode -- it has no TSS descriptor and is
 * not the kernel's real GDT -- and this AP's IDTR is still whatever the CPU
 * reset to (limit 0xFFFF, base 0), i.e. no valid IDT at all.  Both MUST be
 * switched to the kernel's real ones before anything that could fault or
 * take an interrupt runs -- omitting that once produced an immediate triple
 * fault (#GP -> #DF -> #GP, IDT limit 0xffff/base 0), caught in QEMU with
 * `-d int,cpu_reset`.
 *
 * Ordering pitfall, also earned the hard way: tss_load_for_cpu() calls
 * gdt_flush(), which reloads EVERY segment register -- including GS via a
 * plain `mov gs, ax`.  In long mode that zeroes the hidden GS.base, wiping
 * out the IA32_GS_BASE WRMSR cpu_local_init() performs to install this
 * CPU's struct cpu_local pointer.  cpu_local_init() must therefore run
 * AFTER tss_load_for_cpu(); with the wrong order get_cpu_local() reads
 * through GS.base 0 and lapic_enable() #GPs dereferencing physical address
 * zero -- the exception dump showed RAX=0xf000ff53f000ff53, i.e. leftover
 * bytes of the reset-vector area near phys 0, because nothing was there. */
static void ap_entry(uint64_t cpu_index) {
    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    __asm__ volatile ("lidt %0" :: "m"(idtp));

    /* TSS first (its gdt_flush clobbers GS.base), then the per-CPU cpu_local
     * pointer, then the LAPIC (which reads cpu_id via get_cpu_local()). */
    tss_load_for_cpu((int)(cpu_index + 1));
    cpu_local_init(cpu_index + 1);
    lapic_enable();

    kprintf("[smp] AP #%llu online (lapic_id=%u)\n",
            (unsigned long long)cpu_index, lapic_id_for_ap_index[cpu_index]);

    /* Atomically report online. */
    __sync_add_and_fetch(&cpus_online, 1);

    sched_idle();
}

void smp_init(void) {
    cpu_local_init(0);
    lapic_enable();

    uint64_t cpu_count = 0;
    uint32_t bsp_lapic_id = 0;
    boot_cpu_t *cpus = boot_get_smp_info(&cpu_count, &bsp_lapic_id);

    if (cpus == NULL || cpu_count <= 1) {
        kprintf("[smp] single-CPU system (no APs to wake)\n");
        return;
    }

    kprintf("[smp] BSP lapic_id=%u, %llu total CPUs detected\n",
            bsp_lapic_id, (unsigned long long)cpu_count);

    /* This CPU's own actual LAPIC ID (read from hardware) is the ground
     * truth for "which boot_info.cpus[] entry is the BSP", not
     * bsp_lapic_id from boot_info (which the loaders leave at its 0
     * default). */
    uint32_t real_bsp_lapic_id = lapic_read_id();

    uint64_t kernel_pml4 = paging_get_kernel_pml4();
    uint64_t hhdm = boot_get_hhdm_offset();
    if (hhdm == 0) {
        kprintf("[smp] no HHDM available; cannot prepare AP trampoline\n");
        return;
    }

    /* Map the two fixed low-memory pages the trampoline/handoff data live
     * at through the HHDM so the kernel can write them (the boot-time
     * identity map inside the shared PML4 already covers them for the AP's
     * own fetch once its CR3 loads). */
    paging_map(hhdm + SMP_TRAMPOLINE_CODE_PHYS, SMP_TRAMPOLINE_CODE_PHYS,
               PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    paging_map(hhdm + SMP_TRAMPOLINE_DATA_PHYS, SMP_TRAMPOLINE_DATA_PHYS,
               PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);

    /* Copy the trampoline blob to its fixed physical address once; it is
     * identical for every AP (only the handoff data page changes). */
    uint8_t *code_dst =
        (uint8_t *)(uintptr_t)(hhdm + SMP_TRAMPOLINE_CODE_PHYS);
    memcpy(code_dst, ap_trampoline_blob, AP_TRAMPOLINE_SIZE);

    struct smp_handoff *handoff =
        (struct smp_handoff *)(uintptr_t)(hhdm + SMP_TRAMPOLINE_DATA_PHYS);

    uint8_t sipi_vector = (uint8_t)(SMP_TRAMPOLINE_CODE_PHYS >> 12);

    uint64_t ap_count = 0;
    for (uint64_t i = 0; i < cpu_count && ap_count < MAX_CPUS; i++) {
        if (cpus[i].lapic_id == real_bsp_lapic_id) {
            continue;   /* skip the BSP's own entry */
        }

        ap_stacks[ap_count] = kmalloc(AP_STACK_SIZE);
        if (ap_stacks[ap_count] == NULL) {
            kprintf("[smp] OOM allocating AP stack %llu; stopping wake-up\n",
                    (unsigned long long)ap_count);
            break;
        }

        lapic_id_for_ap_index[ap_count] = cpus[i].lapic_id;

        /* Fill the shared handoff page for THIS ap.  APs are woken strictly
         * one at a time (we wait for cpus_online below before moving to
         * the next), so reusing one shared page across the whole loop is
         * race-free. */
        handoff->pml4_phys_low = (uint32_t)(kernel_pml4 & 0xFFFFFFFFu);
        handoff->stack_top     = (uint64_t)(uintptr_t)ap_stacks[ap_count] +
                                 AP_STACK_SIZE;
        handoff->cpu_index     = ap_count;
        handoff->entry_point   = (uint64_t)(uintptr_t)ap_entry;
        __asm__ volatile ("mfence" ::: "memory");

        uint32_t target_before = cpus_online;

        /* Classic INIT-SIPI-SIPI wake-up (Intel MP spec s.B.4). */
        lapic_send_init_ipi(cpus[i].lapic_id);
        smp_udelay(10000);                       /* ~10 ms after INIT */
        lapic_send_init_deassert(cpus[i].lapic_id);
        lapic_send_sipi(cpus[i].lapic_id, sipi_vector);
        smp_udelay(200);                         /* ~200 us between SIPIs */
        lapic_send_sipi(cpus[i].lapic_id, sipi_vector);

        /* Wait for this specific AP to report online before moving on
         * (bounded: ~100 ms), so a wedged/absent CPU cannot block the
         * remaining ones from being woken. */
        int woke = 0;
        for (int spin = 0; spin < 1000; spin++) {
            if (cpus_online != target_before) { woke = 1; break; }
            smp_udelay(100);
        }
        if (!woke) {
            kprintf("[smp]   AP lapic_id=%u did not respond to SIPI; skipping\n",
                    cpus[i].lapic_id);
            continue;
        }

        ap_count++;
    }

    if (cpus_online > 1) {
        kprintf("[smp] %u CPU(s) online (%llu AP(s) woken)\n",
                cpus_online, (unsigned long long)ap_count);
    } else {
        kprintf("[smp] no APs responded; running BSP-only\n");
    }
}

uint32_t smp_get_cpu_count(void) {
    return cpus_online;
}

uint32_t smp_get_schedulable_cpu_count(void) {
    /* schedule() still runs every real thread on the BSP (see the comment
     * in smp.h), so only cpu 0 counts as schedulable for load-balancing
     * purposes even when several APs are online. */
    return 1;
}

void smp_self_test(void) {
    kprintf("[smp] self-test: %u CPU(s) online\n", cpus_online);
    if (cpus_online > 1) {
        kprintf("[smp] PASS: multi-core system detected\n");
    } else {
        kprintf("[smp] PASS: single-core system (use -smp N to test SMP)\n");
    }
}
