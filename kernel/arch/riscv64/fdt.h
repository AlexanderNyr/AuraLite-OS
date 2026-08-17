/* kernel/arch/riscv64/fdt.h -- flattened-device-tree walk into
 * boot_info_t (RISCV_PLAN V1).
 *
 * The third producer of the one handoff struct.  On x86 a *loader*
 * (Stage 2 / BOOTX64.EFI) fills boot_info_t before the kernel runs;
 * here there is no loader of ours -- OpenSBI hands the kernel a DTB
 * pointer in a1 -- so the kernel's own shim does the filling, and
 * kmain_rv consumes the same contract kmain and kmain32 consume.
 *
 * Not libfdt.  The kernel needs four things from the tree (/memory,
 * /chosen's initrd range, the UART, the virtio windows) plus what
 * falls out of walking past them (PLIC, hart count); a bounds-checked
 * single-pass walk is ~250 lines and has no failure modes we did not
 * write ourselves.
 */

#ifndef AURALITE_ARCH_RISCV64_FDT_H
#define AURALITE_ARCH_RISCV64_FDT_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* What the walk found besides boot_info_t fields: MMIO addresses the
 * later phases attach drivers to (V2: PLIC; V7: virtio-mmio).  These
 * have no slot in boot_info_t -- x86 finds its equivalents through
 * ACPI/PCI at runtime -- so they travel in this arch-private struct. */
#define FDT_MAX_VIRTIO 8

typedef struct {
    uint64_t uart_base;                  /* ns16550a; 0 = not found */
    uint32_t uart_irq;                   /* its PLIC line (interrupts prop) */
    uint32_t timebase_freq;              /* /cpus timebase-frequency, Hz;
                                          * what rdtime counts in (V2) */
    uint64_t plic_base;                  /* PLIC;     0 = not found */
    uint64_t virtio_base[FDT_MAX_VIRTIO];/* virtio-mmio windows */
    uint32_t virtio_count;
    const char *bootargs;                /* /chosen bootargs, NUL-terminated,
                                          * points INTO the DTB; 0 if absent */
} fdt_platform_t;

/* Parse the DTB at dtb_phys (satp=0 in V1: physical pointers
 * dereference directly) and fill bi + plat.  Writes bi->magic LAST --
 * a partially-filled struct must never carry a valid magic, the same
 * ordering rule Stage 2 follows.  boot_hartid lands in
 * bi->bsp_lapic_id (the "which CPU booted" slot; RISC-V's name for
 * that concept is the hartid).
 *
 * Returns 0 on success, or a negative FDT_ERR_* the caller can name. */
#define FDT_ERR_MAGIC     -1   /* first 4 bytes are not big-endian 0xD00DFEED */
#define FDT_ERR_VERSION   -2   /* last_comp_version > 17 */
#define FDT_ERR_BOUNDS    -3   /* an offset or size points outside totalsize */
#define FDT_ERR_TRUNCATED -4   /* walk ran past the end without FDT_END */

int fdt_parse(uint64_t dtb_phys, uint64_t boot_hartid,
              boot_info_t *bi, fdt_platform_t *plat);

#endif /* AURALITE_ARCH_RISCV64_FDT_H */
