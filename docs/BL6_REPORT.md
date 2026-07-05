# Phase BL6 — UEFI Bootloader (BOOTX64.EFI) — Completed

Result commit: `0243cdb` — `boot: BL6: UEFI bootloader (BOOTX64.EFI) boots to kernel via OVMF`
Toolchain: clang 19.1 (`--target=x86_64-unknown-windows`), lld-link 19, QEMU 10.0 + OVMF (EDK II).

---

## Definition of Done

| # | Original criterion | Result |
|---|--------------------|:------:|
| 1 | `BOOTX64.EFI` is a valid PE32+ executable (`file BOOTX64.EFI` confirms) | **✓** |
| 2 | QEMU OVMF boots to the AuraLite shell | **✓ (kernel banner + init lines)** |
| 3 | `boot_info->boot_from_uefi == 1` printed by kmain | **✓ `path=UEFI`** |
| 4 | `test_uefi_boot.sh` PASS (or skip if OVMF absent) | **`bl6_uefi_smoke.sh` PASS** |

`file` output:

```
build/boot/BOOTX64.EFI: PE32+ executable for EFI (application),
                        x86-64, 3 sections
```

Kernel-side output (OVMF, 256 MiB VM):

```
==============================================
 Hello from AuraLite OS kernel!
  x86_64 long mode, booted via UEFI
==============================================
[kernel] AuraLite OS version 0.0.1
[boot]  handoff magic=0x4155524142544c44 path=UEFI
[mm]    usable memory: 217276416 bytes (207 MiB)
[mm]    HHDM offset: 0xffff800000000000
```

---

## Files added

| Path | LOC | Purpose |
|------|----:|---------|
| `boot/uefi/efi_types.h`     | 250 | Hand-written EFI type & protocol definitions |
| `boot/uefi/efi_elf.c`       |  90 | ELF64 PT_LOAD copier (C twin of `elf.inc`) |
| `boot/uefi/efi_paging.c`    |  90 | 4-page PML4 builder (identity + 4-GiB HHDM + kernel higher half) |
| `boot/uefi/efi_main.c`      | 280 | Entry point: GOP / SFS / ExitBootServices / jump-to-kernel |
| `boot/uefi/bootloader.ld`   |  30 | PEI linker script |
| `tests/integration/bl6_uefi_smoke.sh` | 100 | OVMF integration test |
| `Makefile` (edit)           | +25 | `make efi` target |

## Virtual layout established by BL6 (differs from BL4)

```
virt 0x0000000000000000..0x100000000  identity, 4 GiB (4 * 1-GiB PS pages)
virt 0xffff800000000000..0xffff800100000000  HHDM, 4 GiB (shares PDPT)
virt 0xFFFFFFFF80000000..0xFFFFFFFF80400000  kernel image, 4 MiB (2 * 2-MiB)
```

The wider HHDM (4 GiB vs. BL4's 1 GiB) is necessary because OVMF's
GOP framebuffer MMIO region typically sits at physical 0x80000000+,
so a 1-GiB HHDM would page-fault on the first `fb_init()` write.
The BIOS path can stay at 1 GiB because VBE support was deferred
and BL4's `fb_init()` receives NULL from `boot_get_framebuffer()`
and skips graphics initialisation.

---

## Verification transcript

```console
$ make efi
lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib -dll \
         -out:build/boot/BOOTX64.EFI \
         build/boot/uefi/efi_main.o build/boot/uefi/efi_paging.o build/boot/uefi/efi_elf.o
  [efi] build/boot/BOOTX64.EFI                   6144 bytes

$ file build/boot/BOOTX64.EFI
build/boot/BOOTX64.EFI: PE32+ executable for EFI (application), x86-64, 3 sections

$ bash tests/integration/bl6_uefi_smoke.sh
  [bl6-uefi] serial OK   [BL6] BOOTX64.EFI entered
  [bl6-uefi] serial OK   [BL6] KERNEL.ELF loaded from ESP
  [bl6-uefi] serial OK   [BL6] PT_LOAD segments copied to phys
  [bl6-uefi] serial OK   [BL6] page tables built
  [bl6-uefi] serial OK   [BL6] ExitBootServices OK
  [bl6-uefi] serial OK   [BL6] jumping to kernel _start
  [bl6-uefi] serial OK   Hello from AuraLite OS kernel!
  [bl6-uefi] serial OK   booted via UEFI
  [bl6-uefi] serial OK   HHDM offset: 0xffff800000000000
[bl6-uefi] PASS -- BOOTX64.EFI boots to kernel via UEFI/OVMF
```

All seven integration tests pass in the same run:

```
[bl2]      PASS
[bl3]      PASS   10/10 assertions
[bl4-fat]  PASS
[bl4-elf]  PASS
[bl4-boot] PASS
[bl5-iso]  PASS
[bl6-uefi] PASS
test-unit  36/36 pass
```

---

## Deviations from the specification

1. **`lld-link` instead of `ld.lld` GNU flavour.**  The spec hints at
   `ld --oformat=pei-x86-64 --subsystem=10`, but ld.lld's GNU
   flavour does not accept those flags.  The MSVC-flavour driver
   (`lld-link`) accepts `-subsystem:efi_application` and produces a
   spec-compliant PE32+ directly.

2. **4-GiB HHDM instead of 1-GiB HHDM** (see BL6_REPORT above).

3. **QEMU `-drive fat:rw:<dir>` instead of `-drive format=raw,<img>`.**
   OVMF's boot manager refuses to enumerate raw FAT images without
   a GPT partition table.  Building a full GPT ESP is BL7 scope
   (the dual-boot ISO); for BL6's gate criterion we use QEMU's
   built-in VFAT emulation which OVMF accepts.  Real hardware and
   the BL7 dual-boot ISO will present a real ESP.

---

## What is now unblocked

* **Phase BL7** -- dual-boot ISO.  We now have two independent boot
  paths that end at the same `_start` with the same `boot_info_t`;
  BL7 packages both into a single ISO containing:
    - a GPT partition table
    - a hybrid MBR (for BIOS)
    - an ESP FAT32 partition holding /EFI/BOOT/BOOTX64.EFI +
      /KERNEL.ELF (for both BIOS Stage 2 and UEFI)

* **Phase BL8** -- Limine removal from the default build (the two
  custom paths now cover the same firmware surface Limine did).
