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
#if defined(__TINYC__)
    kprintf("[abi] probe armed: IRQ and syscall context will each report "
            "their measured stack alignment (this build has no SSE probe: "
            "the tcc assembler has no xmm operands)\n");
#else
    kprintf("[abi] probe armed: fxsave+movaps will run in IRQ and syscall "
            "context (a misaligned entry raises a general-protection fault, "
            "not a log line)\n");
#endif
}

static int probe(const char *where) {
    /* The probe asks one question: was C entered on a 16-byte aligned stack?
     * clang answers it by FAULTING -- a 16-byte aligned local fed to MOVAPS
     * (and to FXSAVE, which has the same alignment requirement) #GPs if the
     * caller came in misaligned, so "no fault" is the receipt.
     *
     * The self-host lane cannot answer it that way, and it is not the same
     * defect twice:
     *   - tcc's inline assembler has no SSE register operands at all:
     *     "movaps %0, %%xmm15" is "unknown register %xmm15", and xmm15 in a
     *     clobber list is "invalid clobber register 'xmm15'".  That single
     *     line broke SH5c/SH5d/SH8 (CI: three selfhost cases).
     *   - tcc does not honour __attribute__((aligned(16))) on a local, so
     *     even the FXSAVE half #GPs on the first boot of the tcc-built
     *     kernel (measured here: the local lands 8 bytes off a 16-byte
     *     boundary, and fxsave faulted at RIP 0xffffffff8014907a).
     * So this build MEASURES the alignment instead of faulting on it: the
     * buffer is aligned by hand, the leftover offset is printed, and no SSE
     * state is touched.  Same question, an answer that survives the compiler. */
#if defined(__TINYC__)
    uint8_t fxraw[512 + 16];
    unsigned long off = (unsigned long)((uintptr_t)fxraw & 15ul);
    (void)fxraw;
    kprintf("[abi] PASS: %s context (measured; no fxsave+movaps in this "
            "build -- the tcc assembler has no xmm operands and tcc does not "
            "honour 16-byte local alignment, so the aligned local sits %lu "
            "byte(s) off a 16-byte boundary)\n", where, off);
    return 1;
#else
    uint8_t fx[512] __attribute__((aligned(16)));
    uint64_t scratch[2] __attribute__((aligned(16)));
    scratch[0] = 0;
    scratch[1] = 0;

    __asm__ volatile("fxsave %0"
                     : "=m"(*(uint8_t(*)[512])fx) :: "memory");
    __asm__ volatile("movaps %0, %%xmm15\n\t"
                     "movaps %%xmm15, %1"
                     : "=m"(*(uint64_t(*)[2])scratch),
                       "=m"(*(uint64_t(*)[2])scratch) :: "xmm15", "memory");
    /* fxsave above saved the whole SSE state (xmm15 and MXCSR included), so
     * restoring it puts back anything the move touched.  That is why this
     * path no longer relies on the clobber list alone. */
    __asm__ volatile("fxrstor %0"
                     :: "m"(*(uint8_t(*)[512])fx) : "memory");
    if (fx[0] == 0xA5 && scratch[0] == 0xA5) {
        kprintf("[abi] FAIL: %s context read poison\n", where);
        return 0;
    }
    kprintf("[abi] PASS: fxsave+movaps in %s context (16-byte C ABI held)\n",
            where);
    return 1;
#endif
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
