/* kernel/arch/riscv64/main_rv.c -- rv64 kernel entry (RISCV_PLAN V0).
 *
 * The V0 stub, deliberately in the I1-stub shape: prove the CHAIN --
 * clang -> lld -> OpenSBI -> _start -> SBI console -- before growing
 * the payload.  Everything printed here is an assertion the smoke
 * test greps: the hartid and DTB pointer prove the a0/a1 handoff
 * survived boot.S; the DTB magic probe proves the pointer actually
 * points at a device tree (big-endian 0xD00DFEED -- the one place
 * byte order bites on this port, met in the stub on purpose so V1's
 * parser inherits a verified fact rather than an assumption).
 *
 * V1 replaces the magic peek with the real FDT walk into boot_info_t;
 * this file then grows toward kmain32's shape phase by phase.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/sbi.h"

/* ---- tiny formatting (kprintf32's opening subset; the shared
 * kprintf arrives with the V6 sweep's console work) ---- */

static void put_hex64(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

static void put_udec(uint64_t v)
{
    char buf[20];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i--)
        sbi_putc(buf[i]);
}

/* ---- entry -------------------------------------------------------------- */

void kmain_rv(uint64_t hartid, uint64_t dtb_phys)
{
    sbi_console_init();

    sbi_puts("\n==============================================\n"
             " Hello from AuraLite OS kernel (riscv64)!\n"
             "  rv64gc S-mode, booted via OpenSBI\n"
             "==============================================\n\n");
    sbi_puts("[kernel] AuraLite OS riscv64, RISCV_PLAN phase V0\n");

    /* The a0/a1 handoff, echoed for the smoke test. */
    sbi_puts("[boot] boot hart: ");
    put_udec(hartid);
    sbi_puts("\n[boot] DTB at phys ");
    put_hex64(dtb_phys);
    sbi_puts("\n");

    /* DTB magic: 0xD00DFEED stored BIG-endian -- the bytes in memory
     * read D0 0D FE ED.  satp=0, so the physical pointer dereferences
     * directly.  A wrong-endian read here would print EDFE0DD0 and
     * the smoke test would say so. */
    if (dtb_phys) {
        const uint8_t *p = (const uint8_t *)dtb_phys;
        uint32_t magic_be = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                            ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
        if (magic_be == 0xD00DFEED) {
            sbi_puts("[boot] DTB magic OK (0xD00DFEED, big-endian read)\n");
        } else {
            sbi_puts("[boot] DTB magic BAD: ");
            put_hex64(magic_be);
            sbi_puts("\n");
        }
    } else {
        sbi_puts("[boot] no DTB pointer -- firmware handoff broken\n");
    }

    sbi_puts("[kernel] V0 stub complete; shutting down "
             "(V1 adds the DTB -> boot_info walk)\n");

    /* Clean exit so the smoke test finishes in seconds, not at the
     * QEMU timeout -- the -no-reboot + hlt idiom, SBI-flavoured. */
    sbi_shutdown();
}
