# AuraLite OS

A from-scratch x86_64 operating system kernel — bootable today from a Limine
BIOS ISO, printing to the serial console and to a linear framebuffer. This is
the foundation of a long-term project to build a complete OS, one milestone at a
time, from "Hello from kernel" up to a multi-process, file-system-capable,
networked system with a shell.

> **Status:** **All 14 phases COMPLETE** (0–14): bootstrap, hello-kernel,
> interrupts, PMM, paging, heap, timer, multitasking, user mode, system calls,
> VFS, interactive shell, SMP, networking, and GUI graphics — **all QEMU-
> verified.** See [PLAN.md](PLAN.md).

---

## What boots right now

- **Limine v12.3.3** loads the kernel into the higher half
  (`0xFFFFFFFF80100000`) and hands off in 64-bit long mode with paging,
  a higher-half direct map, a memory map, a linear framebuffer, and a USTAR
  initrd module ready.
- The kernel brings up all subsystems, mounts a VFS (initrd at `/`, devfs at
  `/dev`), and drops to an **interactive shell** in Ring 3 that reads from
  serial input and runs built-in commands (`ls`, `cat`, `echo`, `help`, etc.).

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
[boot] GDT loaded (kernel + user segments + TSS)
[boot] IDT installed: 256 gates
[boot] PIC remapped (IRQs -> vectors 32-47), all masked
[boot] TSS loaded (RSP0 + IST1 for #DF)
[boot] SYSCALL/SYSRET configured

==============================================
 Hello from AuraLite OS kernel!
  x86_64 long mode, booted via Limine
==============================================

[kernel] AuraLite OS version 0.1.0
[kernel] build: Jun 20 2026 17:02:39
[limine] requested base revision supported
[mm]    usable memory: 535289856 bytes (522744 KiB / 510 MiB)
[mm]    HHDM offset: 0xffff800000000000

[boot] initialising physical memory manager...
[pmm] bitmap at phys 0x0000000000001000, 16384 bytes (4 frames)
[pmm] tracked frames: 130925 (511 MiB)
[pmm] usable frames: 130671 (510 MiB)
[pmm] free frames:   130671 (510 MiB)
[pmm] self-test: allocating 1000 frames...
[pmm] PASS: 1000 unique frames, no leak, contiguous alloc OK
[boot] initialising virtual memory manager...
[vmm] PML4 at phys 0x000000001ff85000, HHDM 0xffff800000000000, NXE enabled
[vmm] self-test: mapping 0x0000006000000000...
[vmm] PASS: map / read / write / unmap all correct
[boot] initialising kernel heap...
[heap] self-test: 10000 alloc/free cycles...
[heap] PASS: 10000 cycles, no corruption, no leak, realloc OK
[heap] region 0xffffffff88000000 (16 MiB), committed 1920 KiB (480 pages)
[heap] used 0 KiB, free 1920 KiB
[boot] initialising timer (PIT @ 100 Hz)...
[timer] PIT channel 0: mode 3, divisor 11932 -> 99 Hz (100 Hz requested)
[timer] self-test: measuring 1-second delay...
[timer] PASS: 99 ticks in 1s (100% of 99 Hz)
[boot] initialising scheduler...
[sched] thread-A: message 0 (tid 1, tick 100)
[sched] thread-B: message 0 (tid 2, tick 100)
...
[sched] PASS: two threads interleaved correctly
[boot] testing user mode (Ring 3)...
[user] self-test: loading compiled ELF binary...
[elf]  loaded 2 segment(s), entry 0x40000020
[user] entering Ring 3 at 0x40000020 (stack top 0x7ffff0000000)
hello
[thread] 'hello' (tid 3) exited
[user] PASS: compiled ELF ran in Ring 3, write() worked

[kernel] reached end of kmain; halting.
```

The same text is also rendered to the on-screen framebuffer (verified by
capturing the QEMU framebuffer and decoding it).

## Other targets

| Target         | Action                                            |
|----------------|---------------------------------------------------|
| `make kernel`  | Compile + link `build/kernel.elf` only            |
| `make iso`     | Build the bootable `build/auralite.iso`              |
| `make run`     | Boot the ISO in QEMU                              |
| `make test-unit` | Build + run host-side unit tests (PMM bitmap)   |
| `make clean`   | Remove `build/`                                    |

## Headless debugging helpers (`tools/`)

These run QEMU as a managed subprocess because the sandbox has no display.

| Script                  | Purpose                                                        |
|-------------------------|----------------------------------------------------------------|
| `boot_debug.py [secs]`  | Boot, capture the framebuffer screenshot + serial log          |
| `analyze_screen.py`     | Decode `build/screen.png` and report colour stats + ASCII map  |
| `read_screen.py R N C`  | High-resolution ASCII dump of `N` text rows starting at row `R`|

## Project layout

```
auralite/
├── boot/limine/limine.conf     # Limine boot config
├── kernel/
│   ├── arch/x86_64/            # boot, GDT, IDT, ISR, PIC, paging, CPU, TSS,
│   │                           #   syscall, port I/O
│   ├── mm/                     # PMM (bitmap), kernel heap (first-fit)
│   ├── proc/                   # scheduler, threads, user-mode, ELF loader
│   ├── lib/                    # kprintf, string, bitmap, spinlock, assert
│   ├── limine_requests.{c,h}   # Limine protocol request bridge
│   ├── kernel.{c,h}            # kmain()
├── drivers/
│   ├── uart/                   # 16550 COM1 serial
│   ├── framebuffer/            # linear FB console + 8x8 font
│   └── timer/                  # 8254 PIT (100 Hz tick)
├── libc/                       # minimal user-space libc (crt0, syscall, wrappers)
├── userspace/hello/            # compiled test program
├── kernel.ld                   # higher-half linker script (Limine-aware)
├── libc/user.ld                # user-space linker script (0x40000000)
├── Makefile                    # Clang/LLD/NASM build (+ user binary pipeline)
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
