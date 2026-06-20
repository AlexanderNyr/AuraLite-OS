# NovOS

A from-scratch x86_64 operating system kernel — bootable today from a Limine
BIOS ISO, printing to the serial console and to a linear framebuffer. This is
the foundation of a long-term project to build a complete OS, one milestone at a
time, from "Hello from kernel" up to a multi-process, file-system-capable,
networked system with a shell.

> **Status:** Phase 0 (bootstrap) and Phase 1 ("Hello from kernel") are
> **complete and QEMU-verified.** See [PLAN.md](PLAN.md).

---

## What boots right now

- **Limine v12.3.3** loads the kernel into the higher half
  (`0xFFFFFFFF80100000`) and hands off in 64-bit long mode with paging,
  a higher-half direct map, a memory map, and a linear framebuffer ready.
- The kernel zeroes `.bss`, loads its own flat GDT, initialises the COM1 UART,
  brings up the framebuffer console, and prints a banner + boot diagnostics,
  then halts.

## Toolchain

| Component  | Version used          |
|------------|-----------------------|
| Compiler   | Clang 19 (`--target=x86_64-elf`) |
| Linker     | LLD 19 (`ld.lld`)     |
| Assembler  | NASM 2.16             |
| Emulator   | QEMU 10 (`qemu-system-x86_64`) |
| Bootloader | Limine 12.3.3 (vendored binary) |
| ISO tool   | xorriso               |

On Debian/Ubuntu: `sudo apt install clang lld nasm qemu-system-x86 xorriso`.

## Quickstart

```bash
# 1. Build the kernel + bootable ISO
make iso

# 2. Boot in QEMU (serial console on stdout, halts at end of kmain)
make run
```

Expected serial output:

```
limine: Loading executable `boot():/boot/kernel.elf`...
[boot] UART (COM1) initialised @ 115200 baud
[boot] framebuffer console initialised
[boot] GDT loaded (flat 64-bit segments)

==============================================
 Hello from NovOS kernel!
  x86_64 long mode, booted via Limine
==============================================

[kernel] NovOS version 0.1.0
[kernel] build: Jun 20 2026 17:02:39
[limine] requested base revision supported
[mm]    usable memory: 535289856 bytes (522744 KiB / 510 MiB)
[mm]    HHDM offset: 0xffff800000000000

[kernel] reached end of kmain; halting.
```

The same text is also rendered to the on-screen framebuffer (verified by
capturing the QEMU framebuffer and decoding it).

## Other targets

| Target        | Action                                            |
|---------------|---------------------------------------------------|
| `make kernel` | Compile + link `build/kernel.elf` only            |
| `make iso`    | Build the bootable `build/novos.iso`              |
| `make run`    | Boot the ISO in QEMU                              |
| `make clean`  | Remove `build/`                                    |

## Headless debugging helpers (`tools/`)

These run QEMU as a managed subprocess because the sandbox has no display.

| Script                  | Purpose                                                        |
|-------------------------|----------------------------------------------------------------|
| `boot_debug.py [secs]`  | Boot, capture the framebuffer screenshot + serial log          |
| `analyze_screen.py`     | Decode `build/screen.png` and report colour stats + ASCII map  |
| `read_screen.py R N C`  | High-resolution ASCII dump of `N` text rows starting at row `R`|

## Project layout

```
novos/
├── boot/limine/limine.conf     # Limine boot config
├── kernel/
│   ├── arch/x86_64/            # boot.asm entry, GDT (+flush), port I/O
│   ├── lib/                    # kprintf, freestanding string, assert
│   ├── limine_requests.{c,h}   # Limine protocol request bridge
│   ├── kernel.{c,h}            # kmain()
├── drivers/
│   ├── uart/                   # 16550 COM1 serial
│   └── framebuffer/            # linear FB console + 8x8 font
├── kernel.ld                   # higher-half linker script (Limine-aware)
├── Makefile                    # Clang/LLD/NASM build
├── limine/                     # vendored Limine 12.3.3 (binary + limine.h)
└── tools/                      # ISO build, QEMU launch, debug capture
```

## Architecture notes

- **Limine over GRUB/multiboot2**: Limine drops us straight into long mode,
  higher-half, with a clean memory map, an HHDM (direct physical map), and a
  framebuffer — far less boot assembly to write ourselves.
- **Framebuffer over VGA text mode**: Limine sets a VBE graphics mode, so the
  classic `0xB8000` text buffer is no longer displayed. We render to the linear
  framebuffer with an embedded 8x8 font instead.
- **Segment permissions**: Limine refuses to map two PT_LOAD segments of
  differing permissions onto the same page, so `kernel.ld` page-aligns every
  text/rodata/data boundary and keeps the Limine request structs inside the
  writable `.data` segment (Limine writes the response pointer back into them).

See [docs/architecture.md](docs/architecture.md) and
[docs/memory_map.md](docs/memory_map.md) for details.
