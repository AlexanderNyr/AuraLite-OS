# NovOS Virtual Memory Map (x86_64)

The address space is established by Limine at load time; the kernel will take
full ownership in Phase 4.

## Kernel image (higher half)

| Region          | Virtual address              | Notes                         |
|-----------------|------------------------------|-------------------------------|
| `.text`         | `0xFFFFFFFF80100000`         | Entry `_start` lives here; R+X|
| `.rodata`       | `0xFFFFFFFF80102000` region  | R                             |
| `.data`         | `0xFFFFFFFF80103000` region  | R+W (Limine requests live here)|
| `.bss` (+stack) | immediately after `.data`    | R+W; 64 KiB stack at the top  |

Exact addresses vary slightly per build; inspect with
`readelf -lW build/kernel.elf`.

## Limine-provided regions

| Region | Address / offset                    | Source request     |
|--------|-------------------------------------|--------------------|
| HHDM   | base `0xFFFF800000000000`           | `LIMINE_HHDM_REQUEST` |
| FB     | physical `0xFD000000` (QEMU stdvga) | `LIMINE_FRAMEBUFFER_REQUEST` |

The HHDM is a direct map of **all** physical memory at a fixed virtual offset,
so the kernel can reach any physical address as
`physical + HHDM_offset`. Reported at boot as "HHDM offset".

## Intended full layout (target)

```
0x0000000000000000 – 0x00007FFFFFFFFFFF   User space (128 TiB)        [later]
0xFFFF800000000000 – 0xFFFFBFFFFFFFFFFF   Direct physical map (HHDM)  [provided]
0xFFFFFFFF80000000 – 0xFFFFFFFFFFFFFFFF   Kernel image + heap + stacks[current]
```

## Physical memory (from Limine memmap)

QEMU `-m 512M` reports ~510 MiB `LIMINE_MEMMAP_USABLE`. The Phase 3 PMM consumes
this map into a bitmap over 4 KiB frames.

### PMM bitmap

| Property       | Value (example, QEMU 512M)        |
|----------------|-----------------------------------|
| Physical base  | `0x0000000000001000` (bootloader-reclaimable) |
| Size           | 16 384 bytes (4 frames)           |
| Tracked frames | 130 925 (~511 MiB)                |
| Usable frames  | 130 671 (~510 MiB)                |
| Free at boot   | 130 671 (== usable → bitmap stole none) |

The bitmap is reached through the HHDM as `0xFFFF800000000000 + bitmap_phys`.
It is placed in **bootloader-reclaimable** memory when available (falling back
to usable), so it does not reduce the usable pool. Allocation returns a physical
address; the caller maps it via the VMM (Phase 4) to actually use it.

### Memory-map types consumed by the PMM

| Type                              | PMM treatment                       |
|-----------------------------------|-------------------------------------|
| `LIMINE_MEMMAP_USABLE` (0)        | free / allocatable                  |
| `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE` (5) | preferred bitmap storage    |
| everything else                   | marked used (not allocatable)       |
