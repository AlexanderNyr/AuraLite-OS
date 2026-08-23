/* kernel/dt/fdt.h -- flattened-device-tree walk into boot_info_t
 * (RISCV_PLAN V1; promoted from kernel/arch/riscv64/ in ARM64_PLAN A1).
 *
 * THE SHARED WALKER.  Two kernels consume this exact object now --
 * riscv64 (OpenSBI hands a DTB pointer in a1) and aarch64 (QEMU parks
 * the DTB at the RAM base for ELF payloads) -- and that is the A1
 * thesis made mechanical: the walker was always portable C over a
 * big-endian byte stream; only its includes were riscv-shaped.  The
 * claim checkers assert both kernels compile this one file, so the
 * promotion cannot silently bitrot into two copies.
 *
 * What stayed arch-owned, and travels as contracts instead:
 *   - dt_phys_to_virt(): how a physical DTB address becomes a
 *     dereferencable pointer (riscv64: the HHDM; aarch64 pre-MMU:
 *     identity).  One function, defined by each consuming arch.
 *   - kernel_layout[8]: the image's physical bounds, exported as
 *     DATA by each arch's boot.S -- medany C cannot address absolute
 *     low symbols across the HHDM gap (the V1 lesson), so the walker
 *     never tries; it reads the pool.
 *
 * Not libfdt.  The kernel needs four things from the tree (/memory,
 * /chosen's initrd range, the UART, the virtio windows) plus what
 * falls out of walking past them (interrupt controller, CPU count);
 * a bounds-checked single-pass walk is ~300 lines and has no failure
 * modes we did not write ourselves.
 */

#ifndef AURALITE_DT_FDT_H
#define AURALITE_DT_FDT_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* What the walk found besides boot_info_t fields: MMIO addresses the
 * later phases attach drivers to.  These have no slot in boot_info_t
 * -- x86 finds its equivalents through ACPI/PCI at runtime -- so
 * they travel in this platform struct.
 *
 * 32 windows: the aarch64 virt board ships 32 virtio-mmio slots (the
 * riscv one ships 8; the array is sized for the larger tenant and
 * the walker stops at the cap either way). */
#define FDT_MAX_VIRTIO 32

/* Interrupt controller kind, as discovered from `compatible`.  The
 * walker uses it to normalise `interrupts` properties ONCE, centrally
 * (ARM64_PLAN A1): GIC trees encode devices as 3 cells <type nr
 * flags> where SPI n means INTID n+32 and PPI n means INTID n+16 --
 * an off-by-32 that no driver should ever add itself.  PLIC trees
 * encode 1 raw cell.  Bring-up honesty: this assumes ONE interrupt
 * controller per board (true on both virt machines); a
 * multi-controller tree would need interrupt-parent tracking. */
#define FDT_INTC_NONE 0
#define FDT_INTC_PLIC 1
#define FDT_INTC_GIC  2

/* PSCI conduit, from /psci's `method` property (aarch64 boards).
 * The aarch64 kernel asserts HVC -- an SMC board would need one
 * instruction changed in psci.c, and the assert names it (A1). */
#define FDT_PSCI_ABSENT 0
#define FDT_PSCI_HVC    1
#define FDT_PSCI_SMC    2

typedef struct {
    uint64_t uart_base;                  /* ns16550a / arm,pl011; 0 = none */
    uint32_t uart_irq;                   /* NORMALISED: PLIC line or GIC
                                          * INTID (SPI+32/PPI+16 applied) */
    uint32_t timebase_freq;              /* /cpus timebase-frequency, Hz;
                                          * riscv's rdtime unit.  aarch64
                                          * trees do not carry it -- the
                                          * frequency is CNTFRQ_EL0, a
                                          * register (plan Fact 2.3);
                                          * stays 0 there. */
    uint64_t plic_base;                  /* PLIC (riscv); 0 = not found */
    uint64_t gicd_base;                  /* GICv2 distributor (aarch64) */
    uint64_t gicc_base;                  /* GICv2 CPU interface (aarch64) --
                                          * or the GICR redistributor region
                                          * when gic_is_v3 says so (R4) */
    uint32_t gic_is_v3;                  /* R4: from the DTB compatible --
                                          * QEMU's v2 distributor is a 4 KiB
                                          * region and PIDR2 lives at +0xFFE8,
                                          * so probe-by-read ABORTS on v2;
                                          * the DTB is the honest source */
    uint32_t intc_kind;                  /* FDT_INTC_* */
    uint32_t psci_method;                /* FDT_PSCI_* */
    uint64_t virtio_base[FDT_MAX_VIRTIO];/* virtio-mmio windows */
    uint32_t virtio_irq[FDT_MAX_VIRTIO]; /* their lines, NORMALISED (A7
                                          * consumes; riscv V7 probed
                                          * instead and may keep doing so) */
    uint32_t virtio_count;
    const char *bootargs;                /* /chosen bootargs, NUL-terminated,
                                          * points INTO the DTB; 0 if absent */
    /* RESIDUE R7: the pci-host-ecam-generic node (both virt boards
     * carry one; ARM64_PLAN D7 measured it and deferred).  reg is the
     * ECAM window; the 32-bit non-prefetchable entry of `ranges` is
     * where BARs may be placed (pci-side address goes INTO the BAR,
     * cpu-side is what the kernel maps -- identical on rv64's board,
     * distinct numbers kept anyway because assuming they match is
     * exactly the class of shortcut the ledger exists to catch). */
    uint64_t pcie_ecam_base;             /* 0 = no ECAM node found */
    uint64_t pcie_ecam_size;
    uint64_t pcie_mmio_cpu;              /* cpu address of the mem32 window */
    uint64_t pcie_mmio_pci;              /* pci address of the same window */
    uint64_t pcie_mmio_size;
    uint64_t fwcfg_base;                 /* qemu,fw-cfg-mmio; 0 = none
                                          * (R11/RES-34: the a64 knob) */
} fdt_platform_t;

/* Arch contract 1: translate the physical DTB address into something
 * this kernel can dereference.  riscv64 returns an HHDM pointer;
 * aarch64 in A1 (MMU off) returns identity, and A3 re-points it. */
const void *dt_phys_to_virt(uint64_t phys);

/* Arch contract 2 (used inside fdt_parse): kernel_layout[8], the
 * image bounds exported as data from each arch's boot.S --
 *   [0] image start PHYS, [7] image end PHYS (the walker only reads
 *   these two; [1..6] serve the arch's own paging code). */

/* Parse the DTB at dtb_phys and fill bi + plat.  Writes bi->magic
 * LAST -- a partially-filled struct must never carry a valid magic,
 * the same ordering rule Stage 2 follows.  boot_hartid lands in
 * bi->bsp_lapic_id (the "which CPU booted" slot; riscv calls it a
 * hartid, aarch64 an MPIDR -- the slot does not care).
 *
 * Returns 0 on success, or a negative FDT_ERR_* the caller can name. */
#define FDT_ERR_MAGIC     -1   /* first 4 bytes are not big-endian 0xD00DFEED */
#define FDT_ERR_VERSION   -2   /* last_comp_version > 17 */
#define FDT_ERR_BOUNDS    -3   /* an offset or size points outside totalsize */
#define FDT_ERR_TRUNCATED -4   /* walk ran past the end without FDT_END */

int fdt_parse(uint64_t dtb_phys, uint64_t boot_hartid,
              boot_info_t *bi, fdt_platform_t *plat);

#endif /* AURALITE_DT_FDT_H */
