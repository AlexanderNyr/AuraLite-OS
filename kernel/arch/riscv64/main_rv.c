/* kernel/arch/riscv64/main_rv.c -- rv64 kernel entry (RISCV_PLAN V1).
 *
 * V0 proved the chain (clang -> lld -> OpenSBI -> _start -> SBI
 * console); V1 makes this kernel the third CONSUMER of boot_info_t.
 * On x86 a loader fills the struct before the kernel runs; here the
 * kernel's own FDT shim fills it from the DTB in a1, and kmain_rv
 * then reads it back through the same contract kmain and kmain32
 * use -- magic first, trust nothing before it checks.
 *
 * The boot log deliberately rhymes with main32.c's: "handoff magic
 * OK", an mmap summary, an initrd line.  Three kernels, one shape --
 * a person reading any serial log knows where they are.
 */

#include <stdint.h>

#include "boot/shared/boot_info.h"
#include "kernel/arch/riscv64/fdt.h"
#include "kernel/arch/riscv64/sbi.h"

/* The struct the FDT shim fills.  Static in .bss (boot.S zeroed it):
 * ~9 KiB is too big for the V0 stack and there is no allocator yet. */
static boot_info_t    boot_info;
static fdt_platform_t platform;

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

/* ---- boot_info consumption (main32.c's shape) --------------------------- */

static int boot_info_check(void)
{
    if (boot_info.magic != BOOT_MAGIC) {
        sbi_puts("[boot] boot_info magic BAD; halting\n");
        return -1;
    }
    sbi_puts("[boot] handoff magic OK, path=SBI, boot_info filled from DTB\n");

    sbi_puts("[mm]   HHDM offset: ");
    put_hex64(boot_info.hhdm_offset);
    sbi_puts(" (Sv39 direct map; the V3 contract, satp=0 today)\n");
    return 0;
}

static void memmap_report(void)
{
    uint64_t usable = 0;

    for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
        const boot_mmap_entry_t *e = &boot_info.mmap[i];
        const char *tag = "other   ";
        switch (e->type) {
        case BOOT_MEM_USABLE:     tag = "usable  "; usable += e->length; break;
        case BOOT_MEM_RESERVED:   tag = "reserved"; break;
        case BOOT_MEM_KERNEL:     tag = "kernel  "; break;
        case BOOT_MEM_BOOTLOADER: tag = "loader  "; break;
        }
        sbi_puts("[mm]   ");
        put_hex64(e->base);
        sbi_puts(" + ");
        put_hex64(e->length);
        sbi_puts("  ");
        sbi_puts(tag);
        sbi_puts("\n");
    }

    sbi_puts("[mm]   mmap entries: ");
    put_udec(boot_info.mmap_count);
    sbi_puts(", usable RAM: ");
    put_udec(usable / (1024 * 1024));
    sbi_puts(" MiB\n");

    if (boot_info.initrd_phys) {
        sbi_puts("[mm]   initrd: ");
        put_udec(boot_info.initrd_size);
        sbi_puts(" bytes at phys ");
        put_hex64(boot_info.initrd_phys);
        sbi_puts("\n");
    } else {
        sbi_puts("[mm]   initrd: none\n");
    }
}

static void platform_report(void)
{
    sbi_puts("[hw]   harts: ");
    put_udec(boot_info.cpu_count);
    sbi_puts(" (boot hart ");
    put_udec(boot_info.bsp_lapic_id);
    sbi_puts(")\n[hw]   uart: ");
    put_hex64(platform.uart_base);
    sbi_puts("\n[hw]   plic: ");
    put_hex64(platform.plic_base);
    sbi_puts("\n[hw]   virtio-mmio windows: ");
    put_udec(platform.virtio_count);
    sbi_puts("\n");
    if (platform.bootargs) {
        sbi_puts("[hw]   bootargs: ");
        sbi_puts(platform.bootargs);
        sbi_puts("\n");
    }
}

/* ---- entry -------------------------------------------------------------- */

void kmain_rv(uint64_t hartid, uint64_t dtb_phys)
{
    sbi_console_init();

    sbi_puts("\n==============================================\n"
             " Hello from AuraLite OS kernel (riscv64)!\n"
             "  rv64gc S-mode, booted via OpenSBI\n"
             "==============================================\n\n");
    sbi_puts("[kernel] AuraLite OS riscv64, RISCV_PLAN phase V1\n");

    sbi_puts("[boot] boot hart: ");
    put_udec(hartid);
    sbi_puts("\n[boot] DTB at phys ");
    put_hex64(dtb_phys);
    sbi_puts("\n");

    /* The V1 shim: DTB -> boot_info_t.  Errors are named, not
     * numbered -- a silent boot was this port's V0 failure mode and
     * once was enough. */
    int rc = dtb_phys ? fdt_parse(dtb_phys, hartid, &boot_info, &platform)
                      : FDT_ERR_MAGIC;
    if (rc != 0) {
        sbi_puts("[boot] FDT parse FAILED: ");
        switch (rc) {
        case FDT_ERR_MAGIC:     sbi_puts("bad magic\n");            break;
        case FDT_ERR_VERSION:   sbi_puts("incompatible version\n"); break;
        case FDT_ERR_BOUNDS:    sbi_puts("offset out of bounds\n"); break;
        case FDT_ERR_TRUNCATED: sbi_puts("truncated stream\n");     break;
        default:                sbi_puts("unknown error\n");        break;
        }
        sbi_puts("[boot] cannot build boot_info; shutting down\n");
        sbi_shutdown();
    }
    sbi_puts("[boot] DTB magic OK (0xD00DFEED, big-endian read)\n");

    /* From here on: only boot_info, never the raw DTB pointer -- the
     * same discipline the x86 kernels keep toward their loaders. */
    if (boot_info_check() != 0)
        sbi_shutdown();
    memmap_report();
    platform_report();

    sbi_puts("[kernel] V1 complete; shutting down "
             "(V2 adds traps, the SBI timer and the PLIC)\n");
    sbi_shutdown();
}
