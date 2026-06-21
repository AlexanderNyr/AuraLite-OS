# AuraLite OS

A from-scratch x86_64 operating system — bootable from a Limine BIOS ISO, with
preemptive multitasking, a Ring 3 user mode, a virtual file system, an
interactive shell, symmetric multi-processing, TCP/IP networking, and a
double-buffered framebuffer GUI. Built one phase at a time, every component
verified in QEMU.

> **Status: All 14 phases complete and QEMU-verified.**
> See [PLAN.md](PLAN.md) for the full milestone history.

---

## What boots right now

```
Limine v12.3.3 → 64-bit long mode, higher-half kernel
  ├── GDT (kernel/user segments + TSS)
  ├── IDT (256 gates, exception dump + stack trace)
  ├── PIC (8259A, IRQ remap)
  ├── UART (COM1, 115200 baud)
  ├── Framebuffer console (8×8 font on linear FB)
  ├── PMM (bitmap, ~510 MiB managed)
  ├── VMM (4-level paging, NX enabled)
  ├── Kernel heap (first-fit, boundary-tag coalescing)
  ├── Timer (8254 PIT, 100 Hz tick)
  ├── Scheduler (preemptive round-robin, context_switch)
  ├── SMP (4 CPUs via Limine MP)
  ├── VFS (USTAR initrd at /, devfs at /dev)
  ├── Networking (e1000 NIC, ARP/IPv4/ICMP)
  ├── Graphics (double-buffered 2D + PS/2 keyboard)
  └── Interactive shell in Ring 3 (ls, cat, echo, help, ...)
```

## Quickstart

```bash
sudo apt install clang lld nasm qemu-system-x86 xorriso   # Debian/Ubuntu
make iso      # build kernel + user programs + bootable ISO
make run      # boot in QEMU with 4 CPUs + e1000 networking
make test-unit  # run host-side unit tests (PMM bitmap, heap allocator)
```

Expected serial output (abridged — see [PLAN.md](PLAN.md) for full trace):

```
[boot] GDT loaded (kernel + user segments + TSS)
[boot] SYSCALL/SYSRET configured
[smp] all 4 CPUs online
[pmm] PASS: 1000 unique frames, no leak, contiguous alloc OK
[vmm] PASS: map / read / write / unmap all correct
[heap] PASS: 10000 cycles, no corruption, no leak, realloc OK
[timer] PASS: 99 ticks in 1s (100% of 99 Hz)
[sched] PASS: two threads interleaved correctly
[vfs] PASS: VFS layer functional
[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)
[gfx] framebuffer GUI rendered (double-buffered flip)

==============================================
   AuraLite OS v0.1.0 — Interactive Shell
==============================================
auralite# ls
  /init  (10240 bytes)
  /hello  (8608 bytes)
auralite# echo hello_world
hello_world
auralite# exit
Goodbye!
```

## Toolchain

| Component  | Version                          |
|------------|----------------------------------|
| Compiler   | Clang 19 (`--target=x86_64-elf`) |
| Linker     | LLD 19 (`ld.lld`)                |
| Assembler  | NASM 2.16                         |
| Emulator   | QEMU 10                           |
| Bootloader | Limine 12.3.3 (vendored binary)  |
| ISO tool   | xorriso                           |

## Make targets

| Target          | Action                                                      |
|-----------------|-------------------------------------------------------------|
| `make iso`      | Build user ELFs + initrd + kernel + bootable `build/auralite.iso` |
| `make kernel`   | Compile + link `build/kernel.elf` only                      |
| `make user`     | Build user-space programs (`init.elf`, `hello.elf`)         |
| `make run`      | Boot in QEMU (`-smp 4`, e1000, serial stdio)                |
| `make debug`    | Boot paused, waiting for GDB on `localhost:1234`            |
| `make test-unit`| Build + run host-side unit tests (PMM, heap)                |
| `make clean`    | Remove `build/`                                             |

## Project layout

```
auralite/
├── kernel/
│   ├── arch/x86_64/         # boot.asm, GDT, IDT, ISR, PIC, paging, TSS,
│   │                        #   SYSCALL/SYSRET, SMP, CPU, port I/O
│   ├── mm/                  # PMM (bitmap), heap (first-fit), kheap wrapper
│   ├── proc/                # scheduler, threads, context_switch, ELF loader,
│   │                        #   user-mode entry (iretq to Ring 3)
│   ├── fs/                  # VFS, USTAR initrd, devfs (/dev/null, /dev/zero)
│   ├── net/                 # Ethernet, ARP, IPv4, ICMP, UDP, DNS
│   ├── lib/                 # kprintf, string, bitmap, spinlock, assert
│   ├── limine_requests.{c,h}# Limine boot-protocol bridge
│   └── kernel.{c,h}         # kmain() — orchestrates all subsystems
├── drivers/
│   ├── uart/                # 16550 COM1 serial (TX + RX)
│   ├── framebuffer/         # linear FB console, 8×8 font, 2D graphics (double-buffered), window manager
│   ├── keyboard/            # PS/2 keyboard (scan-code set 1, IRQ 1)
│   ├── mouse/               # PS/2 mouse (8042 aux, IRQ 12)
│   ├── timer/               # 8254 PIT (100 Hz)
│   ├── pci/                 # PCI config-space access (0xCF8/0xCFC)
│   └── e1000/               # Intel 82540EM NIC (MMIO, TX/RX descriptor rings)
├── libc/                    # user-space libc (crt0, syscall wrappers, printf, string)
├── userspace/
│   ├── init/                # interactive shell (built-in commands)
│   └── hello/               # simple test program
├── tests/unit/              # host-side unit tests (PMM bitmap, heap allocator)
├── tools/                   # ISO build, QEMU launch, initrd, binary embedding,
│                            #   framebuffer screenshot + analysis
├── scripts/                 # CI integration gate
├── docs/                    # architecture, memory map, syscall ABI, driver guide
├── boot/limine/limine.conf  # Limine boot configuration
├── kernel.ld                # higher-half linker script
├── libc/user.ld             # user-space linker script (0x40000000)
├── Makefile                 # Clang/LLD/NASM build system
└── limine/                  # vendored Limine 12.3.3 (binary + limine.h)
```

## Architecture notes

- **Limine** over GRUB/multiboot2: drops us into long mode, higher-half, with
  a memory map, HHDM, framebuffer, and module (initrd) — minimal boot assembly.
- **Framebuffer** over VGA text mode: Limine programs a VBE graphics mode, so we
  render to the linear framebuffer with an embedded font. Phase 14 adds a
  double-buffered 2D graphics layer.
- **Segment permissions**: Limine refuses to map two PT_LOAD segments of
  differing permissions onto the same page. `kernel.ld` page-aligns every
  segment boundary and keeps the Limine request structs inside the writable
  `.data` segment.
- **SYSCALL/SYSRET**: The GDT has user data at index 3 and user code at index 4
  (swapped order) so that SYSRET's formula `CS=base+0x10`, `SS=base+0x08`
  produces correct DPL-3 selectors with `STAR[63:48]=0x10`.
- **DMA addresses**: the e1000 NIC requires physical addresses for its TX/RX
  descriptor rings and packet buffers. These are allocated from the PMM and
  accessed through the HHDM.

See [docs/architecture.md](docs/architecture.md),
[docs/memory_map.md](docs/memory_map.md),
[docs/syscall_abi.md](docs/syscall_abi.md), and
[docs/driver_guide.md](docs/driver_guide.md) for details.
