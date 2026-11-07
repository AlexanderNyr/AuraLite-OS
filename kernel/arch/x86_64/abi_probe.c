/* abi_probe.c — RESIDUE3 C2: run an aligned SSE op inside an IRQ and inside a
 * syscall, and print the receipt.
 *
 * Why a probe and not just the static model: the model can only see the phase
 * arithmetic the assembly performs.  Whether the CPU actually enters C with
 * RSP % 16 == 0 in a live boot is a claim about the whole stack of decisions
 * (the loader's stack, the TSS RSP0, the per-CPU syscall stack slot, the
 * interruption frame), and the cheapest honest way to test a claim about the
 * machine is to make the machine do the thing that faults when it is wrong.
 *
 * fxsave requires a 16-byte-aligned 512-byte memory operand and raises #GP
 * otherwise, so a misaligned entry turns into a visible BSOD/exception in the
 * serial log instead of a silent slowdown.  movaps on a 16-aligned local is
 * the second half: the compiler emits it only when it believes the frame is
 * aligned, which is the same belief the C ABI rests on.
 *
 * Both probes run once, both arm before the shell starts, and both print a
 * line that tests/integration/cases/test_stack_alignment.sh pins.  Cost: two
 * fxsaves per boot.
 */

#include <stdint.h>
#include "kernel/lib/abi_probe.h"
#include "kernel/lib/kprintf.h"

static volatile int armed;          /* set by abi_probe_arm() */
static volatile int irq_done;
static volatile int syscall_done;

void abi_probe_arm(void) {
    armed = 1;
    kprintf("[abi] probe armed: fxsave+movaps will run in IRQ and syscall "
            "context (a misaligned entry raises a general-protection fault, "
            "not a log line)\n");
}

static int probe(const char *where) {
    /* The aligned local is the point: if the caller entered C misaligned, this
     * buffer is not 16-aligned at run time and BOTH instructions fault. */
    uint8_t fx[512] __attribute__((aligned(16)));
    uint64_t scratch[2] __attribute__((aligned(16)));

    __asm__ volatile("fxsave %0"
                     : "=m"(*(uint8_t(*)[512])fx) :: "memory");
    __asm__ volatile("movaps %0, %%xmm15\n\t"
                     "movaps %%xmm15, %1"
                     : "=m"(*(uint64_t(*)[2])scratch),
                       "=m"(*(uint64_t(*)[2])scratch) :: "xmm15", "memory");
    /* fxsave leaves the state untouched; restore the MXCSR we might have
     * inherited isn't needed because fxsave is read-only.  Keep the buffer
     * live for the compiler. */
    if (fx[0] == 0xA5 && scratch[0] == 0xA5) {
        kprintf("[abi] FAIL: %s context read poison\n", where);
        return 0;
    }
    kprintf("[abi] PASS: fxsave+movaps in %s context (16-byte C ABI held)\n",
            where);
    return 1;
}

void abi_probe_irq(void) {
    if (!armed || irq_done) return;
    irq_done = probe("IRQ") ? 1 : 1;
    if (syscall_done) armed = 0;
}

void abi_probe_syscall(void) {
    if (!armed || syscall_done) return;
    syscall_done = probe("syscall") ? 1 : 1;
    if (irq_done) armed = 0;
}

int abi_probe_all_done(void) {
    return irq_done && syscall_done;
}
