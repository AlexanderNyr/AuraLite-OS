# AuraLite OS Virtual Memory Map (x86_64)

The address space is established by AuraLite's own bootloader (BIOS Stage 2
BL3/BL4, or the UEFI `BOOTX64.EFI`) at load time and extended by the kernel's
VMM. The kernel half is shared into every user process address space;
user-space PML4 entries are process-local for spawned programs.

> Historical note: earlier revisions booted via Limine. It was removed in
> favour of the in-tree BL2..BL7 loader chain; the handoff is now the
> `boot_info_t` structure in `boot/shared/boot_info.h`, passed in `RDI`.

For feature-completeness details, see [`status.md`](status.md).

## Kernel image (higher half)

| Region          | Virtual address              | Flags       | Notes                          |
|-----------------|------------------------------|-------------|--------------------------------|
| `.text`         | `0xFFFFFFFF80100000`         | R + X       | Entry `_start` lives here      |
| `.rodata`       | `~0xFFFFFFFF80102000`        | R           | Read-only data                 |
| `.data`         | `~0xFFFFFFFF80103000`        | R + W       | Initialised globals            |
| `.bss`          | after `.data`                | R + W       | Zero-initialised globals       |
| Boot stack      | top of `.bss`                | R + W       | 64 KiB, set in `boot.asm`      |

Exact addresses vary per build; inspect with `readelf -lW build/kernel.elf`.

## Kernel heap

| Region      | Virtual address              | Size    | Flags       |
|-------------|------------------------------|---------|-------------|
| Kernel heap | `0xFFFFFFFF88000000`         | 16 MiB  | R + W + NX  |

The heap grows on demand: `kheap_expand()` maps PMM frames via the VMM in 64 KiB
chunks as `kmalloc` exhausts the free list. Pages are mapped No-Execute.

## Stack regions (guarded)

| Region | Virtual address | Layout |
|--------|-----------------|--------|
| Thread kernel stacks | `0xFFFFFFFF8C000000` | 128 slots × 24 KiB: `[4 KiB guard][16 KiB usable][4 KiB guard]` |
| Per-CPU IST1 stacks (FIX_R1) | `0xFFFFFFFF8C300000` | 32 slots × 20 KiB: `[4 KiB guard][16 KiB usable]` |

Guard pages are simply never mapped via the VMM, so a stack overflow takes a
page fault on the guard instead of corrupting a neighbour; for the IST1 slots
the next slot's guard page sits immediately above the usable area.  The IST1
stacks back the double-fault handler: with the #DF gate armed on IST1
(`tss_init()`), a kernel fault on a dead stack runs its diagnostic on a
known-good stack instead of triple-faulting.

## Bootloader-provided regions

All of these arrive in the `boot_info_t` handoff structure
(`boot/shared/boot_info.h`), whose physical address the loader passes in `RDI`.
The kernel latches it in `boot_info_init()` and reads it through the
`boot_get_*()` accessors.

| Region | Address / offset               | Source field                |
|--------|--------------------------------|-----------------------------|
| HHDM   | base `0xFFFF800000000000`      | `hhdm_offset`               |
| PML4   | phys `0x01000000` (QEMU 512M)  | built by BL4, then CR3      |
| FB     | phys `0xFD000000` (QEMU stdvga)| framebuffer fields          |
| Initrd | phys `0x01800000` (QEMU 512M)  | `boot_get_initrd()`         |

### The low-memory early-boot reserve

`pmm_init()` marks the first **40 MiB** of physical RAM as permanently used
(`PMM_EARLY_BOOT_RESERVE`, `kernel/mm/pmm.c`) so the allocator can never hand
out a frame the loader is still using:

| Physical | Occupant |
|---|---|
| `0x00007000` / `0x00008000` | SMP AP trampoline data / code (must be < 1 MiB for SIPI) |
| `0x00010000` | `boot_info_t` handoff (~9 KiB) |
| `0x00100000` | Kernel `PT_LOAD` segments |
| `0x00200000` | `kernel.elf` staging buffer (BL4, temporary) |
| `0x01000000` | Boot page tables (BL4) |
| `0x01800000` | `initrd.tar` — **16 MiB slot**, ends at the 40 MiB ceiling |

The reserve ceiling is what caps the initrd. It was 32 MiB, giving the archive
exactly 8 MiB; the initrd had grown to ~8.0 MiB, so a marginally larger build
on another host overflowed it and failed `make iso`. The bound is encoded in
three places that must stay in step: `PMM_EARLY_BOOT_RESERVE`,
`INITRD_MAX_BYTES` (`boot/bios/stage2/stage2_start.asm`) and the build-time
check in `tools/mkisoimage_dual.sh`.

The HHDM is a direct map of **all physical RAM** at a fixed virtual offset.
The kernel reaches any physical address as `physical + HHDM_offset`.

> **Important:** the HHDM only covers physical RAM. Device MMIO (e.g. the
> e1000 NIC's BAR0 at `0xFEBC0000`) lives beyond the RAM range and must be
> explicitly mapped via `paging_map()`.

## User space (Ring 3)

| Region       | Virtual address              | Size    | Notes                           |
|--------------|------------------------------|---------|---------------------------------|
| User code    | `0x40000000`                 | varies  | ELF PT_LOAD segments (RWX+User) |
| User data    | `~0x40000120`                | varies  | rodata + .bss (co-located)      |
| User stack   | `0x7FFFF0000000` – top       | 64 KiB  | Grows down, USER + RW           |

The ELF loader maps segments at their `p_vaddr` (linked at `0x40000000` via
`libc/user.ld`). The user stack is mapped just below the 128 TiB canonical
boundary.

Current caveat: PT_LOAD segments are mapped writable/user-accessible during
loading, and final segment `p_flags` are not yet enforced as strict R/W/X
permissions. User pointer validation for syscalls is also future work.

## Paging (VMM)

The VMM walks the 4-level hierarchy (`PML4 → PDPT → PD → PT`) starting from
the PML4 physical base in CR3. Virtual address decomposition:

| Bits      | Field        |
|-----------|--------------|
| 47–39     | PML4 index   |
| 38–30     | PDPT index   |
| 29–21     | PD index     |
| 20–12     | PT index     |
| 11–0      | page offset  |

Each PTE is 8 bytes; bits 12–51 hold the physical frame address. The NX bit
(bit 63) is enabled via EFER.NXE. Intermediate entries created by `walk_pte()`
carry Present|Writable|User; the final PTE gets the caller's full flag set.

## Physical memory (from the boot_info memory map)

QEMU `-m 512M` reports ~511 MiB of `BOOT_MEM_USABLE`.

### PMM bitmap

| Property       | Value (example, QEMU 512M)        |
|----------------|-----------------------------------|
| Physical base  | `0x0000000000001000` (bootloader-reclaimable) |
| Size           | 16 384 bytes (4 frames)           |
| Tracked frames | 130 925 (~511 MiB)                |
| Usable frames  | 130 671 (~510 MiB)                |
| Free at boot   | 130 671 (== usable → bitmap stole none) |

### Memory-map types consumed by the PMM

| Type                              | PMM treatment                       |
|-----------------------------------|-------------------------------------|
| `BOOT_MEM_USABLE` (1)             | free / allocatable                  |
| `BOOT_MEM_BOOTLOADER` (6)         | preferred bitmap storage            |
| everything else                   | marked used (not allocatable)       |

## Device MMIO

The HHDM covers physical RAM, not arbitrary PCI MMIO windows. Device BARs must
therefore be explicitly mapped with `paging_map()` before use.

Known MMIO users:

| Driver | Region | Typical size | Notes |
|---|---|---:|---|
| e1000 | BAR0 | 128 KiB | Register file for Intel 8254x NICs. |
| AHCI | BAR5 / ABAR | at least 8 KiB in current driver | HBA global + per-port registers. |
| OHCI/EHCI/xHCI | PCI MMIO BARs | controller-dependent | Used during USB controller bring-up. |

TX/RX descriptor rings, USB transfer descriptors and similar DMA-visible
structures are allocated from PMM physical frames. The device sees physical
addresses; the kernel accesses the same memory through `HHDM + phys`.
