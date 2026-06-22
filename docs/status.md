# AuraLite OS Status Matrix

This document describes the current state of the repository. It is more current
than the historical 14-phase roadmap in `PLAN.md`.

Legend:

- ✅ **Implemented / exercised** — built by default and has a boot-time or
  host-side test path.
- 🧪 **Experimental** — code exists and may work in constrained scenarios, but
  semantics or coverage are incomplete.
- 🚧 **WIP / partial** — scaffolding or protocol code exists; full data path is
  not complete.
- ❌ **Not implemented** — no working support yet.

## Boot and CPU

| Feature | Status | Notes |
|---|---:|---|
| Limine ISO boot | ✅ | BIOS path is best-tested; UEFI files are included in the ISO. |
| Higher-half kernel | ✅ | Linked at `0xFFFFFFFF80100000`. |
| GDT / IDT / PIC | ✅ | 256 IDT gates, PIC IRQ remap. |
| TSS | ✅ | RSP0 + IST stack support. |
| SYSCALL/SYSRET | ✅ | Linux-like register ABI, custom syscall table. |
| SMP bring-up | 🧪 | APs are started and load tables, but they idle. Scheduler is not SMP-safe. |
| LAPIC / IOAPIC | ❌ | PIC/PIT are used instead. |

## Memory management

| Feature | Status | Notes |
|---|---:|---|
| Physical memory manager | ✅ | Bitmap over 4 KiB frames from Limine memmap. |
| Virtual memory manager | ✅ | Extends Limine page tables through HHDM. |
| Kernel heap | ✅ | First-fit allocator with coalescing. |
| Per-process PML4 | 🧪 | Implemented for spawned user processes. |
| Copy-on-write | ❌ | `fork` deep-copies user pages. |
| User pointer validation | ❌ | Syscalls currently trust user pointers. |
| Slab allocator | ❌ | Future work. |

## Scheduling and processes

| Feature | Status | Notes |
|---|---:|---|
| Kernel threads | ✅ | 16 KiB kernel stacks. |
| Preemptive round-robin | ✅ | PIT tick, quantum-based scheduling. |
| Ring 3 user mode | ✅ | ELF entry via `iretq`. |
| ELF loader | ✅ | Loads PT_LOAD segments at linked virtual addresses. |
| `spawn` | 🧪 | Used by shell `run <prog>`. |
| `fork` | 🧪 | Deep-copy implementation, simplified semantics. |
| `execve` | 🧪 | Replaces current address space, simplified. |
| `wait4` / `wait` | 🧪 | Yield-polling, no precise child PID semantics. |
| Thread/process reaping | ❌ | Dead TCBs/stacks are leaked. |
| Per-process FD tables | ❌ | FD table is global. |

## Filesystems

| Feature | Status | Notes |
|---|---:|---|
| VFS mount table | ✅ | Longest-prefix mount matching. |
| USTAR initrd | ✅ | Read-only root with user ELFs. |
| DevFS | ✅ | `/dev/null`, `/dev/zero`. |
| Writable filesystem | ✅/🧪 | `/tmp` tmpfs, tiny `/disk`, and flat FAT32 `/fat` support create/read/write. |
| Directory vnodes/readdir | 🚧 | `listdir` is special-cased for initrd root. |

## Syscalls

| Area | Status | Notes |
|---|---:|---|
| Console/file I/O | ✅ | `read`, `write`, `open`, `close`. |
| Process basics | 🧪 | `getpid`, `exit`, `spawn`, `fork`, `execve`, `wait4`. |
| Directory listing | 🧪 | Non-standard `listdir`. |
| Networking | 🧪 | DNS, ping and single TCP connection syscalls. |
| Memory syscalls | ❌ | No `mmap`, `munmap`, `brk`. |
| Sockets | ❌ | No BSD socket API. |

## Networking

| Feature | Status | Notes |
|---|---:|---|
| PCI e1000 detection | ✅ | Supports common QEMU/VirtualBox/VMware 8254x IDs. |
| e1000 TX/RX | ✅ | Legacy descriptor rings, polling. |
| Ethernet / ARP | ✅ | Gateway routing support. |
| IPv4 / ICMP | ✅ | Ping self-test. |
| DHCP | ✅ | QEMU/VM NAT-oriented DORA flow. |
| UDP | ✅ | Used by DNS. |
| DNS resolver | ✅ | A-record lookup. |
| TCP client | 🧪 | One connection, no retransmission/sliding window. |
| BSD sockets | ❌ | Future work. |
| virtio-net / vmxnet3 / e1000e | 🚧 | Recognised by the virtual-driver probe, but no data path yet. Use legacy e1000. |

## Graphics and input

| Feature | Status | Notes |
|---|---:|---|
| Framebuffer console | ✅ | Limine-provided linear framebuffer. |
| PSF/bitmap font rendering | ✅ | Embedded console font. |
| 2D graphics | ✅ | Double-buffered drawing. |
| Window manager demo | ✅ | Windows, widgets, taskbar, mouse interaction. |
| PS/2 keyboard | ✅ | Scan-code set 1. |
| PS/2 mouse | ✅ | 3-byte packets, IRQ 12. |
| 3D software renderer | 🧪 | Demo renderer, CPU/SSE float math. |
| Native VBox/VMware SVGA drivers | ❌ | Limine framebuffer is used instead. |

## Storage and USB

| Feature | Status | Notes |
|---|---:|---|
| AHCI detection/init | ✅/🧪 | Controller/port setup works in QEMU AHCI. |
| AHCI sector read/write | ✅/🧪 | DMA READ/WRITE self-test passes on the QEMU AHCI test disk. |
| UHCI controller | ✅/🧪 | Controller + port + CONTROL/BULK TD/QH transfers used by MSC. |
| OHCI controller | 🚧 | Detection/init/port logic; transfer layer not wired to USB core. |
| EHCI controller | 🚧 | Detection/init/port logic; transfer layer not wired to USB core. |
| xHCI controller | 🚧 | Detection/init/ring scaffolding; full enumeration not complete. |
| USB device enumeration | 🧪 | UHCI devices are enumerated through standard requests; other controllers are reported only. |
| USB Mass Storage | 🧪 | Bulk-Only/SCSI path works through UHCI; OHCI/EHCI/xHCI MSC backends remain future work. |

## Wireless and Bluetooth

| Feature | Status | Notes |
|---|---:|---|
| Bluetooth HCI protocol | 🚧 | HCI commands/events implemented; depends on USB transport. |
| Wi-Fi 802.11 MAC layer | 🚧 | Management frames/state machine; no chipset driver registered by default. |

## Userspace applications

| App | Status | Notes |
|---|---:|---|
| `/init` shell | ✅ | Interactive serial shell. |
| `/hello` | ✅ | Smoke test app. |
| `/calc` | ✅ | Calculator. |
| `/sysinfo` | ✅ | Feature info display. |
| `/editor` | ✅ | Simple line editor. |
| `/clock` | ✅ | Uptime/countdown demo. |
| `/guess` | ✅ | Guessing game. |
| `/snake` | ✅ | Terminal snake. |
| `/http` | 🧪 | Uses DNS/TCP syscalls. |
| `/browser` | 🧪 | Text rendering of simple HTTP/HTML responses. |

## Highest-priority gaps

1. Validate user pointers in all syscalls.
2. Add per-process FD tables.
3. Reap dead threads/processes.
4. Extend FAT32 with directories, long filenames, timestamps and free-space accounting.
5. Complete USB bulk/control transfer paths across OHCI/EHCI/xHCI.
6. Replace single global TCP connection with per-connection/socket objects.
7. Make scheduling SMP-aware or explicitly keep APs disabled in normal configs.
