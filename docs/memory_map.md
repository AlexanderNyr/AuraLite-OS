# AuraLite OS Virtual Memory Map (x86_64)

The address space is established by Limine at load time and extended by the
kernel's VMM.

## Kernel image (higher half)

| Region          | Virtual address              | Flags       | Notes                          |
|-----------------|------------------------------|-------------|--------------------------------|
| `.text`         | `0xFFFFFFFF80100000`         | R + X       | Entry `_start` lives here      |
| `.rodata`       | `~0xFFFFFFFF80102000`        | R           | Read-only data                 |
| `.data`         | `~0xFFFFFFFF80103000`        | R + W       | Limine request structs live here |
| `.bss`          | after `.data`                | R + W       | Zero-initialised globals       |
| Boot stack      | top of `.bss`                | R + W       | 64 KiB, set in `boot.asm`      |

Exact addresses vary per build; inspect with `readelf -lW build/kernel.elf`.

## Kernel heap

| Region      | Virtual address              | Size    | Flags       |
|-------------|------------------------------|---------|-------------|
| Kernel heap | `0xFFFFFFFF88000000`         | 16 MiB  | R + W + NX  |

The heap grows on demand: `kheap_expand()` maps PMM frames via the VMM in 64 KiB
chunks as `kmalloc` exhausts the free list. Pages are mapped No-Execute.

## Limine-provided regions

| Region | Address / offset               | Source request              |
|--------|--------------------------------|-----------------------------|
| HHDM   | base `0xFFFF800000000000`      | `LIMINE_HHDM_REQUEST`       |
| PML4   | phys `0x1FF85000` (QEMU 512M)  | CR3 (read by VMM at init)   |
| FB     | phys `0xFD000000` (QEMU stdvga)| `LIMINE_FRAMEBUFFER_REQUEST`|
| Initrd | passed as a module             | `LIMINE_MODULE_REQUEST`     |

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

## Physical memory (from Limine memmap)

QEMU `-m 512M` reports ~510 MiB `LIMINE_MEMMAP_USABLE`.

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
| `LIMINE_MEMMAP_USABLE` (0)        | free / allocatable                  |
| `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE` (5) | preferred bitmap storage    |
| everything else                   | marked used (not allocatable)       |

## Device MMIO

The e1000 NIC's BAR0 is mapped explicitly by `e1000_init()`:

| Region | Physical address      | Size    | Notes                            |
|--------|-----------------------|---------|----------------------------------|
| e1000  | `0xFEBC0000`          | 128 KiB | Mapped at `HHDM + phys` via paging |

TX/RX descriptor rings and packet buffers are allocated from the PMM (physical
frames) so the NIC can DMA to them. The kernel accesses them through the HHDM.
