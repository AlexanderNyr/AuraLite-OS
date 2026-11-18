# RBFI — the optional raw-firmware boot path

Issue [#16](https://github.com/AlexanderNyr/AuraLite-OS/issues/16) asks
AuraLite to "support the RBFI API, just compile it into a flat binary".

This document is the honest account of what that means, what was built, and
what cannot be done — with the measurements behind each claim.

---

## 1. What RBFI is

[RBFI](https://github.com/Syscall1dev/RBFI) (*Raw Boot Firmware Interface*,
MIT, by `$yscall` / Syscall1dev) is a 64-bit replacement firmware for
LGA-1150 and LGA-1155 boards. It replaces the platform BIOS entirely:

| RBFI does | RBFI does **not** do |
|---|---|
| CAR (Cache-As-RAM) setup via MSR 0x2FF | Provide E820 / a memory map |
| DDR3 / DDR4 training (SPD loop at `0xFED10000`) | Set a video mode / VBE |
| PCIe, LPC, PCH, SATA, GPU BAR programming | Publish ACPI tables / MADT |
| Builds **one 2 MiB identity page** (`PD[0] = 0x00000083`) | Load ELF files |
| Copies **192 KiB** from the boot USB stick to `0x00100000` | Start APs (SMP) |
| Jumps there in long mode, flat GDT | Anything a UEFI/bootloader does |

That is the whole interface: *your first byte runs at 1 MiB in long mode*.
"Raw" is literal.

Its CPUID gate accepts only Sandy Bridge (0x2A), Ivy Bridge (0x3A), Haswell
(0x3C), Broadwell (0x3D) and Bay Trail (0x37). Anything else halts at
`no_found_proccesor`.

---

## 2. Why AuraLite cannot use that surface directly

The kernel is a ~20 MiB ELF linked at `0xFFFFFFFF80100000` with three
`PT_LOAD` segments. `kernel/arch/x86_64/boot.asm` documents the contract it
expects:

* `RDI` = physical pointer to a `boot_info_t` with `BOOT_MAGIC`;
* paging already on, with a **higher-half direct map** at
  `0xffff800000000000` (the kernel rewrites its `boot_info` pointer through
  it, so without the HHDM every later read faults);
* a memory map — `kernel/mm/pmm.c` refuses to start without one;
* a stack, and interrupts masked.

RBFI provides none of that: 2 MiB of address space, no map, no direct map.

**The gap is the integration.** `firmware/rbfi/rbfi_stage2.asm` is the
adapter that turns RBFI's interface into the one the kernel already expects.

---

## 3. What was built

```
firmware/rbfi/
├── RBFI_x86.nas        vendored upstream ROM (MIT, unmodified behaviour)
├── README.md           attribution + the three upstream defects we patch
├── rbfi_stage2.asm     the 192 KiB adapter stage (flat binary)
└── LICENSE             upstream MIT licence, verbatim
tools/
├── mkrbfi.py           packs header + kernel ELF + initrd into the USB image
└── check_rbfi.py       the machine-checkable half of this integration
userspace/apps/gui-rbfi/grbfi.c   the desktop panel (opt-in by construction)
```

### The stage, in four steps

1. **Relocate to `0x00030000`.** RBFI drops it at 1 MiB, which is exactly
   where the kernel's first `PT_LOAD` must land, so the first thing the stage
   does is copy itself to the one 192 KiB window RBFI never writes.
2. **Build page tables.** This is the load-bearing step:

   | PML4 slot | Virtual | Physical | Why |
   |---|---|---|---|
   | 0 | `0x0000000000000000` | `[0, 1 GiB)` | the stage writes past RBFI's 2 MiB |
   | 256 | `0xffff800000000000` | `[0, 1 GiB)` | the HHDM `boot_info.c` reads through |
   | 511 | `0xffffffff80000000` | `[0, 1 GiB)` | the kernel's own link base |

   All 2 MiB pages. Without this the very first segment copy triple-faults
   with **no output at all** — the failure mode this project keeps having.
3. **Load the kernel ELF** through RBFI's live bulk endpoint ring, using the
   same `READ(10)` sequence RBFI itself uses, with
   `phys = low32(p_vaddr) - 0x80000000` (the arithmetic
   `boot/bios/stage2/elf.inc` documents) and the `memsz - filesz` tail zeroed.
4. **Fill `boot_info_t`** and jump to `e_entry` with `RDI = boot_info phys`,
   `RSP = 0xF0000`, interrupts masked — byte-for-byte the ABI stage 2
   provides.

### The header (written by `mkrbfi.py`, read by the stage)

| Offset | Type | Field |
|---|---|---|
| `0x00` | u32 | magic `'RBFI'` (`0x49464252`) |
| `0x04` | u32 | version (1) |
| `0x08` | u32 | header bytes (512) |
| `0x0C` | u32 | flags — bit0 = initrd present |
| `0x10` | u64 | kernel offset |
| `0x18` | u64 | kernel length |
| `0x20` | u64 | initrd offset (0 = none) |
| `0x28` | u64 | initrd length |
| `0x30` | u64 | RAM bytes (for the synthetic map) |
| `0x38` | u64 | entry hint (the ELF's `e_entry` wins) |

`tools/check_rbfi.py` cross-checks every offset the stage reads against what
the packer writes, so the two cannot drift apart.

---

## 4. A memory map that admits it is synthetic

RBFI publishes no E820. The stage therefore **synthesizes** a map from the
RAM size RBFI reports, and says so in a new `boot_info_t` field:

```c
uint8_t loader_flags;      /* BOOT_FLAG_RBFI_SYNTH | BOOT_FLAG_NO_FRAMEBUF */
```

`BOOT_FLAG_RBFI_SYNTH` exists so `pmm_init()` can take the conservative path:
a map derived from a single RAM-size figure cannot describe holes, and a map
that *looked* like E820 while being derived would be a lie the PMM trusts.
Every other loader leaves `loader_flags = 0`.

*(The field reuses the old `_pad[7]`, so no byte offset in
`tools/gen_boot_offsets.c` moved — verified: `BOOT_INFO_SIZEOF` is still
7776 and `BOOT_LOADER_FLAGS_OFF` is 7769.)*

---

## 5. Why there is no `make run-rbfi`

**RBFI does not run under QEMU.** This is measured, not assumed:

| Test | Result |
|---|---|
| `-bios RBFI.bin`, default `qemu64` | halts at `no_found_proccesor` (CPUID gate) |
| `-cpu Haswell`, q35 | no serial output; its DDR loop polls `0xFED10000+` that QEMU does not implement |
| `-cpu IvyBridge`, q35 | same |
| `-cpu SandyBridge`, q35 | same |

So a `run-rbfi` target would be a target that builds an image and then lies
about running it. `make rbfi-image` builds the image and prints exactly what
was verified; testing the *stage* itself needs hardware or a KVM host with
the right chipset.

The parts that **are** machine-checkable run on every build via
`make rbfi-check` (and `tools/check_rbfi.py --selftest` proves the checker
can fail).

---

## 6. Opt-in, at three levels

The user's requirement was explicit: RBFI must not be the default path, and
reaching it must require a flag and a GUI choice.

| Level | How |
|---|---|
| **Build** | `make iso` is unchanged. `make rbfi-image` is a separate target; nothing depends on it. |
| **Boot** | The board must be told to boot the stick. AuraLite's own loaders (Stage 1/2, BOOTX64.EFI) are untouched. |
| **Desktop** | *RBFI Firmware* sits in **System** in the start menu (`apps.rs`, icon 12). It opens a panel that explains the path and prints the one command that produces the stick. |

Nothing in the boot path names RBFI, and `check_rbfi.py` enforces that
(`boot/bios/stage2/stage2_start.asm` must not mention it).

---

## 7. The vendored ROM does not build upstream

Upstream `RBFI_x86.nas` fails `nasm -f bin` at line 745 with
`error: label or instruction expected at start of line`. The vendored copy
fixes **three** defects, and nothing else:

| Line | Defect | Fix |
|---|---|---|
| 745 | a stray `0` on its own line | removed |
| 782 | `htl` instead of `hlt` in `launch32bit` | corrected |
| 832 | `gdt_poiii` references `compat322`, which exists only inside the commented-out 32-bit block | stub zeroed; it is never the operand of an `lgdt`/`ljmp` |

With those, the ROM assembles to **exactly 131072 bytes** with the reset
vector `EA 0000 E000` at the tail — which is what makes
`-bios RBFI.bin` land at `0xFFFE0000`.

`check_rbfi.py` assembles the vendored copy on every run, so if upstream
fixes one of these the patch becomes a visible no-op instead of silent drift.

---

## 8. Using it

```bash
make rbfi-image
# build/auralite.rbfi — write it to a USB stick:
sudo dd if=build/auralite.rbfi of=/dev/sdX bs=1M conv=fsync
```

Then set the board to boot that stick. RBFI performs its chipset bring-up,
copies the first 192 KiB to 1 MiB, and the stage takes over from there.

Verified in this tree:

| Check | Result |
|---|---|
| `make rbfi-rom` | 131072 bytes |
| `make rbfi-stage` | 196608 bytes (RBFI's exact window) |
| `make rbfi-image` | header + ELF + initrd, sector-aligned |
| `tools/check_rbfi.py --check` | OK |
| `tools/check_rbfi.py --selftest` | every injected defect reported |
| `make iso` (unchanged path) | rc=0, boots to shell, 0 FAIL |
| `grbfi` in the desktop | launches in Ring 3, PID 9 |

Not verified in this tree, and why: the boot handoff itself, because it
requires the hardware (§5).
