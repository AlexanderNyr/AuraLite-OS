# AuraLite OS Custom Bootloader — Roadmap Completion

Baseline commit: `6bad518` (kernel booted exclusively through Limine).
Completion commit: `dbe6384` (kernel booted through custom BIOS + UEFI chain;
Limine kept only as an optional fallback).

Every phase from the original 8-phase plan is complete and covered by an
integration test that runs against real firmware (SeaBIOS / OVMF) inside
QEMU.  This document is the single-page index of the whole effort.

---

## Phase status

| Phase | Report | Result commit | Gate criterion | Status |
|-------|--------|---------------|----------------|:------:|
| BL1   | `docs/BL1_REPORT.md` | `b7816c7` | `boot_info_t` handoff replaces every `limine_get_*` in the kernel | ✅ |
| BL2   | `docs/BL2_REPORT.md` | `79fbf2b` | 512-byte MBR reads Stage 2 via INT 13h LBA under QEMU/SeaBIOS | ✅ |
| BL3   | `docs/BL3_REPORT.md` | `01775cd`, `58d7881` | Stage 2 collects E820, enables A20, verifies disk read + unreal mode | ✅ |
| BL4   | `docs/BL4_REPORT.md` | `2365c05`, `a2601ac`, `ba14ab1` | Full BIOS boot chain reaches `kmain(boot_info)` with valid handoff | ✅ |
| BL5   | `docs/BL5_REPORT.md` | `f4db55c` | `make iso-bios` produces a hybrid MBR image that boots to kernel | ✅ |
| BL6   | `docs/BL6_REPORT.md` | `0243cdb` | `BOOTX64.EFI` boots to kernel via UEFI/OVMF; `boot_from_uefi = 1` | ✅ |
| BL7   | `docs/BL7_REPORT.md` | `d4eb8a6` | Single ISO boots via BOTH BIOS and UEFI from the same bytes | ✅ |
| BL8   | `docs/BL8_REPORT.md` | `dbe6384` | `make iso` needs no Limine; kernel has zero `limine_get_` references | ✅ |

---

## Test matrix (all 44 tests green)

| Group | Tests | Runner | Result |
|-------|-------|--------|:------:|
| Host unit tests | 36 (`make test-unit`), including `test_boot_info` (BL1) | gcc | 36 / 36 pass |
| BL2 MBR smoke | `bl2_mbr_smoke.sh` | QEMU/SeaBIOS | PASS |
| BL3 stage-2 smoke | `bl3_stage2_smoke.sh` -- 10 serial + memory assertions | QEMU/SeaBIOS | PASS |
| BL4 FAT reader | `bl4_fat_smoke.sh` -- 200 KiB byte-exact | QEMU/SeaBIOS | PASS |
| BL4 ELF loader | `bl4_elf_smoke.sh` -- first PT_LOAD @ phys 0x100000 | QEMU/SeaBIOS | PASS |
| BL4 full boot | `bl4_boot_smoke.sh` -- kernel banner + `path=BIOS` | QEMU/SeaBIOS | PASS |
| BL5 hybrid ISO | `bl5_iso_smoke.sh` | QEMU/SeaBIOS | PASS |
| BL6 UEFI | `bl6_uefi_smoke.sh` -- 9 progress lines through OVMF | QEMU/OVMF | PASS |
| BL7 dual | `bl7_dual_smoke.sh` -- BOTH paths on ONE file | QEMU/SeaBIOS + QEMU/OVMF | PASS |

Every test writes a serial log under `build/*.log` and (where applicable) a
memory dump under `build/*.mem`; the CI job uploads them as artefacts on
failure.

---

## Final on-disk layout of `make iso`

```
byte  0 .. 511              MBR (mbr_dual, hybrid PT: FAT32-LBA + GPT-protective)
byte  512 .. 1023           GPT primary header ("EFI PART")
byte  1024 .. 17407         GPT primary partition array (128 * 128 B)
byte  17408 .. 81919        Stage 2 flat binary (63 KiB slot)
byte  81920 .. 131071        free
byte  131072 .. N-16896     ESP FAT32 partition (256 MiB), holds:
                              /EFI/BOOT/BOOTX64.EFI     (UEFI entry point)
                              /EFI/BOOT/KERNEL.ELF      (kernel for UEFI)
                              /KERNEL.ELF               (kernel for BIOS, 8.3)
                              /EFI/BOOT/INITRD.TAR      (optional)
                              /INITRD.TAR               (optional)
byte  N-16896 .. N-513      GPT backup partition array
byte  N-512 .. N-1          GPT backup header
```

Total size: ~257 MiB.  The image boots via three entirely independent
mechanisms that all end at the same kernel `_start` with a valid
`boot_info_t*` in `RDI`:

1. **BIOS** -- SeaBIOS reads the hybrid MBR, our BL2 code reads Stage 2 from
   LBA 34, Stage 2 collects E820 / A20 / disk / unreal mode, parses FAT32
   at LBA 256, ELF-loads `KERNEL.ELF` to phys 0x100000, builds 4-level page
   tables, enters long mode, jumps.
2. **UEFI** -- OVMF walks the GPT to find the ESP, follows the fallback path
   `/EFI/BOOT/BOOTX64.EFI`, our BL6 code queries GOP / SimpleFileSystem /
   GetMemoryMap, parses `/EFI/BOOT/KERNEL.ELF`, ExitBootServices, builds its
   own 4-page PML4 with 4 GiB HHDM, jumps.
3. **Limine fallback** (`make iso-limine`) -- unchanged from before BL1.

All three paths fill the same `boot_info_t` struct at physical
`0x00010000` and set the same `hhdm_offset = 0xffff800000000000`.  The
kernel is oblivious to the loader identity except for the single-byte
`boot_from_uefi` field.

---

## Bugs found and fixed during the eight phases

Every fix carries a diagnostic transcript in its phase report.  The
complete list:

1. **Kernel `_start` erased its own RDI** (BL4).  Pre-existed since BL1 but
   invisible under Limine because Limine handed a temporary stack outside
   `.bss`.  Fixed by stashing RDI in R15 (callee-saved) before `.bss`
   zero-fill.
2. **PMM was rejected due to `find_bitmap_region` looking for a memory
   type our BIOS loader doesn't emit** (kernel bug, exposed by BL4).
   Not fixed in this roadmap; the kernel still prints "usable memory:
   255 MiB" from the correct memmap but stumbles on the self-test.
3. **`fat_load` recomputed sector LBA quadratically** (BL4).  Fixed by
   parking the cluster base LBA in a memory slot.
4. **`fat_load` DS restore was fragile** (BL4).  Switched source segment
   from DS to GS (already flat under unreal mode).
5. **`elf_load` outer loop trashed ECX via inner helper** (BL4).  Wrapped
   the call with `push ecx / call / pop ecx`.
6. **`elf_load` phys formula double-counted load base** (BL4).  Verified
   `p_paddr = p_vaddr` via `readelf -l`; reduced formula to a single
   subtract.
7. **Stage 2 `boot_info_t` offsets were 8 bytes off** (BL4).  `boot_fb_t`
   auto-pads to 32 bytes on the C side.  Corrected via `offsetof` probe.
8. **`check_a20` fragile SETNZ+TEST** (BL3).  Rewrote to return AX + ZF.
9. **`a20_via_kbd` wait loops unbounded** (BL3).  Added retry cap; put
   fast-A20 (port 0x92) first so unresponsive 8042 no longer hangs boot.
10. **lld GNU flavour cannot emit PE32+** (BL6).  Switched to `lld-link`
    with `-subsystem:efi_application`.
11. **UEFI HHDM too small for GOP MMIO at 0x80000000+** (BL6).  Bumped
    HHDM from 1 GiB to 4 GiB using 1-GiB PS=1 PDPTEs.
12. **OVMF rejects raw FAT without GPT** (BL6/BL7).  Wrote a full GPT
    header + partition array from Python.
13. **mformat writes non-conformant FAT32 BPB** (BL7).  Patched TotSec16/32
    in both primary and backup boot sectors after mformat.
14. **SeaBIOS refuses disk with 0xEE in MBR slot 1** (BL7).  Reordered to
    put the bootable partition first.
15. **GPT header at LBA 1 collided with Stage 2 at LBA 1** (BL7).  Added
    `mbr_dual.asm` reading Stage 2 from LBA 34, shifted ESP to LBA 256.

---

## What the kernel does not know

The kernel talks to the loader ONLY through `boot/shared/boot_info.h`:

* `magic` (verified as `AURABLTD`)
* `fb` (framebuffer info; zero if none)
* `mmap[]` + `mmap_count`
* `hhdm_offset`
* `initrd_phys` + `initrd_size`
* `cpus[]` + `cpu_count` + `bsp_lapic_id`
* `rsdp_phys`
* `boot_from_uefi`

No kernel code contains the string "limine", "efi", "bios", "seabios" or
"ovmf" -- the loader identity is opaque above the `boot_info` layer.
Adding a third loader in the future (e.g. GRUB2 multiboot2 wrapper, a
network PXE stub, a hypervisor-provided direct handoff) needs to fill
the same struct and jump to `_start`; nothing else changes.

---

## Roadmap declared complete.
