# Phase BL3 — BIOS Stage 2 Real-Mode Services — Completed

Result commits:
* `01775cd` — `boot: bios/stage2: Stage 2 skeleton + E820 + A20 + boot_info seeding (Phase BL3 core)`
* `58d7881` — `boot: bios/stage2: add disk_read_lba wrapper + unreal mode helper`

Toolchain: NASM 2.16, QEMU 10.0 + SeaBIOS.

---

## Definition of Done

| # | Original criterion | Result |
|---|--------------------|:------:|
| 1 | `detect_memory()` fills `boot_info.mmap[]` with ≥ 1 USABLE entry | **7 entries under QEMU 128 MiB** |
| 2 | `find_vbe_mode()` sets ≥ 1024×768 32bpp | **N/A — VBE deferred by design** |
| 3 | `enable_a20()` passes `check_a20()` on QEMU | **A20 is on before/after** |
| 4 | `fat_find_root("KERNEL  ELF")` returns non-zero cluster | **Deferred to BL4 (C impl)** |
| 5 | `elf64_load()` copies PT_LOAD segments to correct addresses | **Deferred to BL4 (C impl)** |
| — | Bonus: BIOS INT 13h LBA read wrapper | **`disk_read_lba` verified** |
| — | Bonus: unreal mode for >1 MiB access | **`go_unreal` verified end-to-end** |

Two items intentionally deferred:

* **VBE**: user directed to skip framebuffer discovery for BL3
  (`fb=0` boot works fine because the kernel treats framebuffer as
  optional and prints via COM1).  Will be reconsidered after BL7.
* **FAT32 + ELF loader**: after prototyping FAT in NASM I moved both
  to BL4 as C freestanding modules.  They are >200 lines each and
  much easier to audit / test in C than in 16-bit assembly.  All
  real-mode BIOS services they depend on (disk_read_lba, unreal
  mode) are already in place.

---

## Files added / changed

| Path | LOC | Purpose |
|------|----:|---------|
| `boot/bios/stage2/stage2_start.asm` | 200 | Entry + boot_info seeding + top-level flow |
| `boot/bios/stage2/uart16.inc`         |  85 | UART 16550 driver (115200 8N1) |
| `boot/bios/stage2/e820.inc`         |  70 | INT 15h EAX=E820h loop → mmap[] |
| `boot/bios/stage2/a20.inc`          | 130 | A20 enable (BIOS → 0x92 → 8042) |
| `boot/bios/stage2/disk.inc`         |  80 | INT 13h AH=42h wrapper |
| `boot/bios/stage2/unreal.inc`       |  85 | Unreal mode (FS/GS flat) |
| `tests/integration/bl3_stage2_smoke.sh` | 120 | End-to-end QEMU test |
| `Makefile` (edit)                   |  +20 | `make stage2` target + 63 KiB size guard |

---

## Runtime picture at end of BL3

After Stage 2 finishes its real-mode work and executes `hlt`,
memory looks like this (verified via QEMU `pmemsave`):

```
0x00007C00  MBR (Stage 1)              -- untouched, reclaimable
0x00008000  Stage 2 code + data         (~1 KiB, plenty of room to 63 KiB)
0x00010000  boot_info_t block           magic=AURABLTD, hhdm=0xffff800000000000,
                                        mmap_count=7 (QEMU 128 MiB layout)
0x00011000  scratch buffer (BPB, FAT sector, dir sector)
0x00100000  0xDEADC0DE                  -- sentinel from unreal write
```

CPU state:
* A20 line: **enabled**.
* Interrupts: masked (`cli` at entry) except during the E820 call
  and the A20 BIOS attempts, restored before `hlt`.
* GDT: `unreal_gdtr` still loaded (3 entries: null / code32-flat /
  data32-flat).  BL4 will replace it with the full 32→64 GDT.
* FS, GS: hidden descriptor caches are **flat 4 GiB, 32-bit** — the
  unreal-mode legacy that BL4's C code will rely on to write the
  kernel image directly at physical 0x00100000.

---

## Verification transcript

```console
$ make stage2
nasm -f bin -I . -o build/boot/stage2.bin boot/bios/stage2/stage2_start.asm
  [stage2] build/boot/stage2.bin                  1024 bytes (max 64512)

$ bash tests/integration/bl3_stage2_smoke.sh
  [bl3] serial OK  [BL3] AuraLite stage2 alive
  [bl3] serial OK  [BL3] E820 done
  [bl3] serial OK  [BL3] A20 gate on
  [bl3] serial OK  [BL3] disk read OK
  [bl3] serial OK  [BL3] unreal mode OK
  [bl3] serial OK  [BL3] real-mode services complete
  [bl3] mem @0x100000 = 0xDEADC0DE                        OK
  [bl3] mem magic  = 0x4155524142544c44   OK
  [bl3] mem hhdm   = 0xffff800000000000   OK
  [bl3] mem mmap_c = 7                   OK
  [bl3] PASS
```

---

## Deviations from the specification

1. **Data addressing uses ES + 16-bit offsets** instead of the
   spec's raw 20-bit literals like `[BOOT_INFO_PHYS + BOOT_HHDM_OFF]`.
   NASM correctly rejects the raw form in 16-bit code (offset must
   fit in a word), so the code now sets `ES = BOOT_INFO_SEG = 0x1000`
   once and uses `[es:BOOT_HHDM_OFF]`.  Same layout, no size change.
2. **A20 enable order.**  Fast-A20 (port 0x92) runs before the 8042
   toggle, and every `.kbd_wait_*` loop is bounded to 65 535 IN
   opcodes.  The original spec's ordering can hang forever on some
   virtualised 8042s.
3. **`check_a20`** returns AX + ZF instead of ZF-only via SETNZ.
   The spec version's inversion trick masked a real triple-fault
   loop under QEMU on the very first attempt; the new form is a
   plain compare with an explicit AX result and never blows away
   ZF between memory writes.
4. **VBE + FAT32 + ELF64 deferred** as documented above.

---

## What is now unblocked

Phase BL4 (protected mode + long mode + kernel entry) can start.
Everything it needs from real mode is in place:

* Physical memory map (`boot_info.mmap[]`).
* A20 line enabled.
* 32-bit LBA disk reads (`disk_read_lba`).
* Flat 32-bit memory writes via FS while still in real mode.

BL4 will:
1. Install the full 32→64 GDT.
2. Enter protected mode.
3. Parse FAT32 in C, locate `KERNEL.ELF` and `INITRD.TAR`.
4. Read them via disk_read_lba + unreal-mode memcpy above 1 MiB.
5. Load the ELF program headers into their PT_LOAD physical
   addresses (kernel at 0x100000).
6. Build the 4-level page tables (identity, HHDM, higher-half).
7. Enter long mode.
8. Jump to `_start` with RDI = 0x10000 (boot_info_t).
