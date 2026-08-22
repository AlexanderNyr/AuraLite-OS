/* kernel/arch/aarch64/psci.c -- PSCI over hvc (ARM64_PLAN A0).
 *
 * This platform's SBI: firmware-shaped services reached by a
 * trap-to-higher-agent instruction.  The mapping from the riscv64
 * port is exact -- sbi_shutdown <-> SYSTEM_OFF, HSM hart_start <->
 * CPU_ON (D5's named exit ramp, unused until SMP) -- and it is used
 * the same way V0 used sbi_shutdown: every smoke run ends by
 * power-off, never by hanging to the timeout.
 *
 * The conduit is hvc, hardcoded here because A0 runs before the DTB
 * walk exists; the dumped DTB says method = "hvc" (plan Fact 3/4,
 * SYSTEM_OFF measured working in fact-finding).  A1's walker asserts
 * the method string, so a hypothetical smc board fails loudly at
 * parse time instead of hanging in this file.
 *
 * One inline-asm site for the same reason sbi.c has one: the sweep
 * audits a funnel, not a scatter.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/psci.h"
#include "kernel/arch/aarch64/pl011.h"

#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_CPU_ON_64  0xC4000003u   /* P6: SMC64 CPU_ON */

static uint64_t psci_call(uint64_t fid)
{
    register uint64_t x0 __asm__("x0") = fid;

    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    return x0;
}

/* P6: the three-argument conduit CPU_ON needs (x1..x3 live). */
static uint64_t psci_call3(uint64_t fid, uint64_t a1, uint64_t a2,
                           uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

long psci_cpu_on(uint64_t target_mpidr, uint64_t entry_pa,
                 uint64_t context)
{
    return (long)psci_call3(PSCI_CPU_ON_64, target_mpidr, entry_pa,
                            context);
}

void psci_system_off(void)
{
    /* A7: the TX ring must drain before the power goes -- the boot
     * log's tail is where the smoke assertions live, and SYSTEM_OFF
     * takes ringed-but-unsent bytes with it. */
    pl011_tx_flush();
    psci_call(PSCI_SYSTEM_OFF);
    /* SYSTEM_OFF does not return; if it somehow did, park honestly
     * rather than executing past the end of the world. */
    for (;;)
        __asm__ volatile("wfi");
}
