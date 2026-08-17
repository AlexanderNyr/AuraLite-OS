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

#define PSCI_SYSTEM_OFF 0x84000008u

static uint64_t psci_call(uint64_t fid)
{
    register uint64_t x0 __asm__("x0") = fid;

    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    return x0;
}

void psci_system_off(void)
{
    psci_call(PSCI_SYSTEM_OFF);
    /* SYSTEM_OFF does not return; if it somehow did, park honestly
     * rather than executing past the end of the world. */
    for (;;)
        __asm__ volatile("wfi");
}
