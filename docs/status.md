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
| User pointer validation | 🧪 | `validate_user_range`, `copy_from_user`, `copy_to_user` are used by syscall dispatch; not yet a fault-recovering uaccess layer. |
| Slab allocator | ❌ | Future work. |

## Scheduling and processes

| Feature | Status | Notes |
|---|---:|---|
| Kernel threads | ✅ | 16 KiB kernel stacks. |
| Preemptive round-robin | ✅ | PIT tick, quantum-based scheduling. |
| Ring 3 user mode | ✅ | ELF entry via `iretq`. |
| ELF loader | ✅ | Loads PT_LOAD segments at linked virtual addresses. |
| `spawn` | 🧪 | Used by shell `run <prog>` and integration-tested. |
| `fork` | 🧪 | Deep-copy implementation, simplified semantics. |
| `execve` | 🧪 | Replaces current address space, simplified. |
| `wait4` / `wait` | 🧪 | Yield-polling, no precise child PID semantics. |
| Thread/process reaping | 🧪 | Dead TCBs/stacks are deferred-reaped from a safe stack; user page-table frames are still leaked. |
| Per-process FD tables | 🧪 | Each TCB has its own FD table; `fork` shallow-copies entries. Lifetime/inheritance semantics remain simplified. |

## Filesystems

| Feature | Status | Notes |
|---|---:|---|
| VFS mount table | ✅ | Longest-prefix mount matching. |
| `readdir` / `mkdir` / `unlink` / `rename` / `stat` | ✅ | Generic VFS ops + matching syscalls (`100..105`). |
| USTAR initrd | ✅ | Read-only root with user ELFs. |
| DevFS | ✅ | `/dev/null`, `/dev/zero`. |
| `tmpfs` | ✅ | Writable in-memory `/tmp`, supports `unlink` and `truncate`. |
| Tiny diskfs | ✅ | Persistent `/disk` (8 files × 4 KiB, AHCI port 0). |
| **FAT32 — full** | ✅ | `/fat`: subdirs, **LFN (UCS-2 read+write)**, mkdir/rmdir/unlink/rename/truncate, FSInfo, FAT date/time stamps. |
| **ext2 — full** | ✅ | `/ext2`: mounts existing Linux-mkfs images **and** formats blank disks in-kernel.  Direct + single/double/triple indirect blocks; mkdir/rmdir/unlink/rename; cross-OS round-trip verified with `debugfs`. |

## Syscalls

| Area | Status | Notes |
|---|---:|---|
| Console/file I/O | ✅ | `read`, `write`, `open`, `close`. |
| Process basics | 🧪 | `getpid`, `exit`, `spawn`, `fork`, `execve`, `wait4`. |
| Directory/path ops | ✅/🧪 | `listdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `truncate`, `stat`. |
| Networking | 🧪 | DNS, ping, legacy TCP calls and process-owned socket-style syscalls. |
| GUI | 🧪 | `SYS_GUI_CALL` and `SYS_GUI_EVENT` power `libauragui` apps. |
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
| Socket API | 🧪 | AF_INET/SOCK_STREAM process-owned handles exist; underlying TCP transport still allows one active stream. |
| virtio-net / vmxnet3 / e1000e | 🚧 | Recognised by the virtual-driver probe, but no data path yet. Use legacy e1000. |

## Graphics and input

| Feature | Status | Notes |
|---|---:|---|
| Framebuffer console | ✅ | Limine-provided linear framebuffer. |
| PSF/bitmap font rendering | ✅ | Embedded console font. |
| 2D graphics | ✅ | Double-buffered drawing. |
| Window manager demo | ✅ | Windows, widgets, taskbar, mouse interaction. |
| PS/2 keyboard | ✅ | Scan-code set 1, ASCII + rich key-event queues. |
| PS/2 mouse | ✅ | IRQ 12, cursor/buttons and wheel-event support. |
| Kernel GUI/compositor | 🧪 | Window manager, taskbar, event queues, owner-checked GUI syscalls, process-exit window cleanup and `libauragui` apps. |
| 3D software renderer | 🧪 | Demo renderer, CPU/SSE float math. |
| Native VBox/VMware SVGA drivers | ❌ | Limine framebuffer is used instead. |

## Storage and USB

| Feature | Status | Notes |
|---|---:|---|
| AHCI detection/init | ✅/🧪 | Controller/port setup works in QEMU AHCI. |
| AHCI sector read/write | ✅/🧪 | DMA READ/WRITE self-test passes on the QEMU AHCI test disk. |
| UHCI controller | ✅/🧪 | Controller + port + CONTROL/BULK TD/QH transfers used by MSC. |
| OHCI controller | 🚧 | Detection/init/port logic plus stable control/bulk backend API; ED/TD scheduling still pending. |
| EHCI controller | 🚧 | Detection/init/port logic plus stable control/bulk backend API; async qTD scheduling still pending. |
| xHCI controller | 🚧 | Detection/init/ring scaffolding plus stable control/bulk backend API; slot/address/endpoints still pending. |
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
| `/init` shell | ✅ | Interactive serial/keyboard shell. |
| `/hello` | ✅ | Smoke test app. |
| `/calc` | ✅ | Calculator. |
| `/sysinfo` | ✅ | Feature info display. |
| `/editor` | ✅ | Simple line editor. |
| `/clock` | ✅ | Uptime/countdown demo. |
| `/guess` | ✅ | Guessing game. |
| `/snake` | ✅ | Terminal snake. |
| `/http` | 🧪 | Uses DNS/TCP syscalls. |
| `/browser` | 🧪 | Text rendering of simple HTTP/HTML responses. |
| `/gcalc`, `/gedit`, `/gfiles`, `/gterm`, `/gsysmon`, `/gabout`, `/glaunch` | 🧪 | GUI apps using `libauragui`. |

## Highest-priority gaps

1. Finish full address-space/page-table reaping for exited user processes.
2. Extend user-copy into a fault-recovering uaccess layer and audit remaining kernel-internal callers.
3. Complete USB bulk/control transfer paths across OHCI/EHCI/xHCI.
4. Replace the single global TCP connection with per-connection/socket objects.
5. Make scheduling SMP-aware or explicitly keep APs disabled in normal configs.
6. Enforce strict user ELF segment permissions and add user memory syscalls.
7. Tighten FD inheritance/lifetime semantics around `fork`, `execve` and process exit.
