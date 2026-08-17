/* kernel/arch/aarch64/main_a64.c -- aarch64 kernel main (ARM64_PLAN A0).
 *
 * The A0 stub: prove the whole path clang -> lld -> QEMU ELF load ->
 * EL1 _start -> PL011 -> PSCI power-off, and echo every measured fact
 * from the plan's fact-finding so the smoke test regression-covers
 * them:
 *
 *   - CurrentEL == 1 (Fact 2.1: ELF -kernel enters at EL1);
 *   - x0 at entry (Fact 2.2: it is 0, NOT the DTB pointer -- printed
 *     so a QEMU behaviour change announces itself as a diff in this
 *     line, not as a mystery later);
 *   - the DTB magic probed at the RAM base 0x40000000 (Fact 2.2: the
 *     FDT lives there for ELF payloads; big-endian 0xD00DFEED read
 *     byte-wise, the byte-order fact A1's parser inherits) -- and a
 *     refusal if it is absent, because every later phase stands on
 *     this address;
 *   - CNTFRQ_EL0 (Fact 2.3: the timer frequency is a register, not a
 *     DTB field -- one fewer boot_info field than riscv64 needed).
 *
 * Ends in psci_system_off() rather than a wfi idle so every smoke run
 * exits in under a second -- the V0 tradition; A2's timer work
 * re-introduces the idle loop when there is something to wake up for.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/psci.h"

#define RAM_BASE      0x40000000UL
#define FDT_MAGIC_BE  0xD00DFEEDUL

static uint64_t read_currentel(void)
{
    uint64_t el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return el >> 2;
}

static uint64_t read_cntfrq(void)
{
    uint64_t f;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

/* The DTB magic, read big-endian byte-wise from the RAM base.  With
 * the MMU off this region is Device memory and the compiler must not
 * merge or widen the accesses -- byte loads are both the byte-order
 * answer and the alignment answer (A0 is compiled -mstrict-align,
 * plan Fact 5.1, but byte loads owe nothing to the flag). */
static uint32_t fdt_magic_at_ram_base(void)
{
    const volatile uint8_t *p = (const volatile uint8_t *)RAM_BASE;

    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

void kmain_a64(uint64_t x0_at_entry)
{
    pl011_puts("\nHello from AuraLite OS kernel (aarch64)!\n");
    pl011_puts("[kernel] AuraLite OS aarch64, ARM64_PLAN phase A0\n");

    pl011_puts("[boot] CurrentEL: EL");
    pl011_putdec64(read_currentel());
    pl011_putc('\n');

    /* Fact 2.2, first half: x0 is NOT the DTB pointer for ELF
     * payloads.  Print what actually arrived; the smoke asserts it is
     * 0, so the day QEMU starts honouring the Image protocol for ELF
     * too, this line changes and the assertion names the change. */
    pl011_puts("[boot] x0 at entry: ");
    pl011_puthex64(x0_at_entry);
    pl011_puts(" (not the DTB pointer for ELF payloads -- measured, plan Fact 2)\n");

    /* Fact 2.2, second half: the DTB is parked at the RAM base. */
    uint32_t magic = fdt_magic_at_ram_base();

    pl011_puts("[boot] DTB probe at RAM base ");
    pl011_puthex64(RAM_BASE);
    pl011_puts(": magic ");
    pl011_puthex64(magic);
    if (magic == FDT_MAGIC_BE) {
        pl011_puts(" OK (big-endian read)\n");
    } else {
        /* Refuse, don't limp: A1 and everything after it stand on
         * this address.  A wrong probe today is a NULL boot_info
         * tomorrow. */
        pl011_puts(" MISMATCH; refusing to continue (expected 0xD00DFEED)\n");
        psci_system_off();
    }

    /* Fact 2.3: the timer frequency is architecture, not DTB. */
    pl011_puts("[boot] CNTFRQ_EL0: ");
    pl011_putdec64(read_cntfrq());
    pl011_puts(" Hz\n");

    pl011_puts("[kernel] A0 stub complete; powering off via PSCI "
               "(A1 adds the DTB -> boot_info walk)\n");
    psci_system_off();
}
