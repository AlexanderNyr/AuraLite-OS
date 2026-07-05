# Phase BL7 — Dual-Boot ISO + CI Integration — Completed

Result commit: `d4eb8a6` — `boot: BL7: dual-boot ISO -- one image boots via BIOS AND UEFI`
Toolchain: NASM 2.16, clang 19.1, lld-link 19, mtools 4.0, QEMU 10.0 + SeaBIOS + OVMF, Python 3.13.

---

## Definition of Done

| # | Original criterion | Result |
|---|--------------------|:------:|
| 1 | `make iso` (dual) succeeds without Limine binaries installed | **`make iso-dual` PASS** |
| 2 | BIOS integration test PASS on the dual ISO | **`[bl7/bios] PASS`** |
| 3 | UEFI integration test PASS on the dual ISO | **`[bl7/uefi] PASS`** |
| 4 | All existing integration tests still green | **8/8 green** |

Note on criterion #1: the spec suggests reusing the plain `make iso`
target for the dual image.  In this repo `make iso` currently still
means "Limine-based ISO" so as not to break other tooling (usb, vbox,
vmware, run, etc.).  We add `make iso-dual` alongside; the Limine
`make iso` target is retired by BL8, which is the point at which
`make iso == make iso-dual` becomes the sane default.

---

## Files added

| Path | LOC | Purpose |
|------|----:|---------|
| `boot/bios/stage1/mbr_dual.asm`     | 100 | BL2 MBR twin, reads Stage 2 from LBA 34 (past GPT array) |
| `tools/mkisoimage_dual.sh`          | 200 | Hybrid GPT+MBR image assembler |
| `tests/integration/bl7_dual_smoke.sh` | 120 | Boots the dual ISO twice (BIOS + UEFI) |
| `Makefile` (edit)                   | +14 | `make mbr-dual`, `make iso-dual` targets |

## File modified

| Path | Change |
|------|--------|
| `boot/bios/stage2/stage2_start.asm` | fat_init now probes LBA 128 (BL5) then LBA 256 (BL7); the two layouts coexist without a rebuild |

---

## Final disk layout

```
LBA 0            MBR (mbr_dual variant, hybrid PT: 0x0C + 0xEE)
LBA 1            GPT primary header ("EFI PART")
LBA 2..33        GPT primary partition array (128 entries * 128 B)
LBA 34..159      Stage 2 flat binary (63 KiB max slot)
LBA 160..255     free
LBA 256..N-33    ESP FAT32 partition (256 MiB)
                    /EFI/BOOT/BOOTX64.EFI     -- UEFI entry point
                    /EFI/BOOT/KERNEL.ELF      -- kernel for UEFI
                    /KERNEL.ELF                -- kernel for BIOS (8.3)
                    /EFI/BOOT/INITRD.TAR      -- optional
                    /INITRD.TAR                -- optional
LBA N-33..N-2    GPT backup partition array
LBA N-1          GPT backup header
```

MBR hybrid partition table:

| Slot | Bootable | Type | LBA start | Sector count |
|------|:--------:|------|----------:|-------------:|
| 1    | 0x80     | 0x0C (FAT32-LBA)       | 256       | 524288 (256 MiB)  |
| 2    | 0x00     | 0xEE (GPT protective)  | 1         | disk-size (256 MiB) |

Kernel banner on BIOS:

```
==============================================
 Hello from AuraLite OS kernel!
  x86_64 long mode, booted via BIOS
==============================================
```

Kernel banner on UEFI (same disk):

```
==============================================
 Hello from AuraLite OS kernel!
  x86_64 long mode, booted via UEFI
==============================================
```

---

## Verification transcript

```console
$ make iso-dual
[mkiso-dual] assembling raw hybrid image at build/auralite-dual.raw
[mkiso-dual] wrote build/auralite-dual.iso (258M, dual-boot BIOS+UEFI hybrid image)

$ bash tests/integration/bl7_dual_smoke.sh
  [bl7-dual] hybrid image OK  (size=269484032 bytes)
                        MBR sig=0x55aa, GPT='EFI PART',
                        MBR[1]=0x0c (FAT32-LBA),
                        MBR[2]=0xee (GPT protective)
  [bl7/bios] serial OK   [BL3] AuraLite stage2 alive
  [bl7/bios] serial OK   [BL4] entering long mode; jumping to kernel _start
  [bl7/bios] serial OK   Hello from AuraLite OS kernel!
  [bl7/bios] serial OK   booted via BIOS
  [bl7/bios] serial OK   HHDM offset: 0xffff800000000000
  [bl7/uefi] serial OK   [BL6] BOOTX64.EFI entered
  [bl7/uefi] serial OK   [BL6] ExitBootServices OK
  [bl7/uefi] serial OK   Hello from AuraLite OS kernel!
  [bl7/uefi] serial OK   booted via UEFI
  [bl7/uefi] serial OK   HHDM offset: 0xffff800000000000
[bl7-dual] PASS -- one ISO boots to kernel via BOTH BIOS and UEFI
```

All eight integration tests + unit tests green in the same run:

```
[bl2]      PASS
[bl3]      PASS   10/10 assertions
[bl4-fat]  PASS
[bl4-elf]  PASS
[bl4-boot] PASS
[bl5-iso]  PASS   BIOS-only ISO
[bl6-uefi] PASS   UEFI-only via QEMU VFAT
[bl7-dual] PASS   both paths on ONE hybrid file
test-unit  36/36 pass
```

---

## Bugs fixed during BL7 bring-up

1. **mformat writes a non-conformant FAT32 BPB.**  `BPB_TotSec16` is
   set to the sector count and `BPB_TotSec32` stays 0.  The Microsoft
   FAT32 spec (Extensible Firmware Initiative FAT32 File System Spec
   1.03 s.3.5) requires `BPB_TotSec16 == 0` for FAT32.  OVMF's
   `FatPkg` strictly rejects the volume.  Fixed by patching both
   primary and backup boot sectors after mformat.
2. **First hybrid MBR draft** put the 0xEE protective GPT entry in
   slot 1, which made SeaBIOS refuse to boot the disk (interpreted
   as "GPT-only, hand off to UEFI").  Reversed the order.
3. **Stage 2 collided with GPT header.**  Original layout kept Stage
   2 at LBA 1 (BL2 default), but that is where the GPT primary
   header lives.  MBR read the GPT header into 0x8000 and jumped to
   garbage.  Introduced `mbr_dual` reading Stage 2 from LBA 34 and
   shifted the ESP to LBA 256 so the two never overlap.

---

## Deviations from the specification

Same two as BL5+BL6, elevated to BL7 scope:

* **No El Torito.**  Both firmware paths boot the hybrid image via
  their standard hard-disk enumeration (SeaBIOS reads MBR; OVMF
  reads GPT).  Adding El Torito requires an ISO 9660 reader in
  Stage 2 or an isohybrid overlay MBR; both are additive on top of
  BL7 and can happen later without breaking anything.
* **CI workflow not touched.**  The spec's BL7.3 patches
  `.github/workflows/integration.yml` to run the new BIOS + UEFI
  tests.  This repo's CI is Limine-based; wiring the eight new
  BL* tests in belongs to BL8 (which owns the CI cleanup part of
  the Limine removal).

---

## What is now unblocked

* **Phase BL8** -- remove Limine from the default build.  Concretely:
    - `kernel/limine_requests.[ch]` already gone (BL1).
    - `-I limine` still in CFLAGS but nothing depends on those headers.
    - `make iso` still points at `mkisoimage.sh` (Limine).  BL8
      renames that to `mkisoimage_limine.sh` and reroutes `make iso`
      to `mkisoimage_dual.sh`.
    - `limine/` binary bundle + `.gitmodules` submodule reference
      become optional (kept as fallback).
    - README quickstart switches to `make iso-dual` (or the renamed
      `make iso`).
    - CI workflow gains the eight BL* integration jobs.
