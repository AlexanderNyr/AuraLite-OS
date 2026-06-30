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
| SMP bring-up | 🧪 | APs online + scheduler lock; BSP-only scheduling remains the normal execution mode while APs enter `sched_idle()`. |
| LAPIC / IOAPIC | 🧪 | LAPIC enable + timer on each CPU are implemented; IOAPIC routing is still future work and legacy PIC/PIT paths remain available. |

## Memory management

| Feature | Status | Notes |
|---|---:|---|
| Physical memory manager | ✅ | Bitmap over 4 KiB frames from Limine memmap. |
| Virtual memory manager | ✅ | Extends Limine page tables through HHDM. |
| Kernel heap | ✅ | First-fit allocator with coalescing. |
| Per-process PML4 | 🧪 | Implemented for spawned user processes. |
| Copy-on-write | ✅ | `fork` via `paging_clone_user_space()` performs mark-and-share COW: page-table pages are copied, writable user frames are shared (write-protected in both parent and child) via `PAGE_FLAG_COW`; first write triggers `paging_handle_cow_fault()` to copy. PMM refcount (reference counting for shared frames) is implemented in `pmm.c`. |
| User pointer validation | 🧪 | `validate_user_range`, `copy_from_user`, `copy_to_user` use a #PF fixup path so TOCTOU/unmap during copy returns an error instead of panicking. |
| User ELF segment permissions / NX | ✅ | User linker emits page-aligned RX/R/NX/RW/NX `PT_LOAD` segments; `elf_load()` maps `PF_W` only as writable and applies `PAGE_FLAG_NO_EXEC` to non-`PF_X` segments. User stacks are writable+NX with guard pages left unmapped. `/elfperm` + `test_elf_permissions.sh` cover write-to-text and execute-from-data faults. |
| Stack guard pages + overflow diagnosis | ✅ | Kernel-thread stacks are bracketed by unmapped guard pages on both sides of each slot; user stacks have an unmapped guard page below them. `kernel/proc/guard.c` classifies a `#PF` on a known guard region and the `#PF` handler reports `[GUARD] kernel/user stack overflow`: a kernel-stack guard hit is fatal (`kernel_halt()`), a user-stack guard hit becomes SIGSEGV. `/stackguard` + `test_stack_guard.sh` and a host unit test cover the user overflow path and the classification boundaries. |
| Slab allocator | ✅ | `kernel/mm/slab.c` provides fixed-size caches; `tcb_cache`, `ofd_cache`, and `vnode_cache` are initialized during boot and used by thread/VFS allocation paths. |

## Scheduling and processes

| Feature | Status | Notes |
|---|---:|---|
| Kernel threads | ✅ | 16 KiB kernel stacks. |
| Preemptive round-robin | ✅ | PIT tick, quantum-based scheduling. |
| Blocking I/O / wait queues | ✅ | `kernel/proc/wait_queue.c` backs blocking pipes, futex waits, `select()`, and `nanosleep()` without yield-polling loops. |
| Ring 3 user mode | ✅ | ELF entry via `iretq`. |
| ELF loader | ✅ | Loads PT_LOAD segments at linked virtual addresses. |
| `spawn` | 🧪 | Used by shell `run <prog>` and integration-tested. |
| `fork` | 🧪 | COW fork via `paging_clone_user_space()` (O(page-tables) not O(address-space)); simplified PID semantics. |
| `execve` | 🧪 | Replaces current address space, simplified. |
| `wait4` / `wait` | 🧪 | Yield-polling, no precise child PID semantics. |
| SA_RESTART syscall restarting | 🧪 | Restartable blocking syscalls save restart metadata on `-EINTR` and are transparently re-dispatched from `sigreturn` when the handler was installed with `SA_RESTART`; signal frames preserve FPU/SSE state via `FXSAVE`/`FXRSTOR`. |
| Thread/process reaping | ✅ | Dead TCBs/stacks are deferred-reaped from a safe stack via `thread_reap_zombies()` called every PIT tick. User address spaces are fully freed by `paging_free_address_space()` (walk user PML4, free all data frames + page-table frames + PML4 itself). COW shared frames use PMM refcount — frame is only returned to the free pool when all sharers have released it. Boot log confirms: `[thread] reaped '/hello' (tid 6, 35 frames)`. |
| Per-process FD tables | 🧪 | Each TCB has its own FD table; `fork` shallow-copies entries. Lifetime/inheritance semantics remain simplified. |

## Filesystems

| Feature | Status | Notes |
|---|---:|---|
| VFS mount table | ✅ | Longest-prefix mount matching. |
| `readdir` / `mkdir` / `unlink` / `rename` / `stat` | ✅ | Generic VFS ops + matching syscalls (`100..105`); `stat()` exposes size/mode/link metadata plus seconds-resolution `mtime`/`ctime`/`atime` where supported. Baseline `mkfifo` named FIFOs, `symlink`/`readlink`, and `lstat`/`fstat` are wired (in-memory, `/tmp`-tested). |
| USTAR initrd | ✅ | Read-only root with user ELFs. |
| DevFS | ✅ | `/dev/null`, `/dev/zero`. |
| `tmpfs` | ✅ | Writable in-memory `/tmp`, supports `unlink`, `truncate`, and seconds-resolution `stat()` timestamps. |
| Tiny diskfs | ✅ | Persistent `/disk` (8 files × 4 KiB, AHCI port 0) with persistent stat timestamps in the on-disk table. |
| **FAT32 — full** | ✅ | `/fat`: subdirs, **LFN (UCS-2 read+write)**, mkdir/rmdir/unlink/rename/truncate, FSInfo, FAT date/time stamps decoded through `stat()`. |
| **ext2 — full** | ✅ | `/ext2`: mounts existing Linux-mkfs images **and** formats blank disks in-kernel. Direct + single/double/triple indirect blocks; mkdir/rmdir/unlink/rename; inode timestamps; cross-OS round-trip verified with `debugfs`. |
| buffer cache | 🧪 | Buffer cache layer for block I/O caching and synchronization. |
| exFAT | 🚧 | skeleton |
| NTFS | 🚧 | skeleton |
| ext4 | 🚧 | experimental ext4-like |
| Btrfs | 🚧 | experimental CoW prototype |
| F2FS | 🚧 | experimental log-structured prototype |

## Syscalls

| Area | Status | Notes |
|---|---:|---|
| Console/file I/O | ✅ | `read`, `write`, `open`, `close`. |
| Process basics | 🧪 | `getpid`, `exit`, `spawn`, `fork`, `execve`, `wait4`. |
| Directory/path ops | ✅/🧪 | `listdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `truncate`, `stat`. |
| Networking | 🧪 | DNS, ping, legacy TCP calls and process-owned socket-style syscalls. |
| GUI | ✅/🧪 | `SYS_GUI_CALL` (200), `SYS_GUI_EVENT` (201), `SYS_GUI_THEME` (202). v2.0 theme engine, icons, notifications, snap, context menus. |
| Memory syscalls | 🧪 | `brk` implemented. `mmap`/`munmap` support eager private anonymous mappings and eager private file-backed reads. |
| Sockets | 🧪 | AF_INET/SOCK_STREAM handles include `socket`, `connect`, `send`, `recv`, `close`, plus server-side `bind`, `listen`, and `accept` (`305..307`). AF_INET/SOCK_DGRAM supports `sendto=44` and `recvfrom=45`. |

## Networking

| Feature | Status | Notes |
|---|---:|---|
| NIC abstraction (netdev) | ✅ | `kernel/net/netdev.{h,c}` selects an active NIC backend at boot; the IPv4/ARP/DHCP/UDP/TCP stack talks to it via `netdev_*` instead of a specific driver. e1000 is the default; virtio-net is the fallback when e1000 is absent. |
| PCI e1000 detection | ✅ | Supports common QEMU/VirtualBox/VMware 8254x IDs. |
| e1000 TX/RX | 🧪 | Legacy descriptor rings with INTx IRQ enable, IRQ cause handling, preallocated software RX queue, non-blocking compatibility receive, and blocking/timed receive helpers. TCP, ARP, DHCP, ICMP, and kernel UDP/DNS receive waits now use IRQ/wait-queue-backed bounded NIC waits; user UDP sockets (`sendto`/`recvfrom`) and basic fixed-RTO TCP retransmission are implemented; remaining N2 work is deeper socket blocking edge cases and production TCP features. |
| Ethernet / ARP | ✅ | Gateway routing support. |
| IPv4 / ICMP | ✅ | Ping self-test. |
| DHCP | ✅ | QEMU/VM NAT-oriented DORA flow. |
| UDP | ✅ | Used by DNS and exposed to userspace through AF_INET/SOCK_DGRAM `sendto`/`recvfrom`. |
| DNS resolver | ✅ | A-record lookup. |
| TCP client | 🧪 | Per-connection TCP state (up to 8 connections) with a one-segment retransmission slot and fixed RTO/retry handling for SYN, data ACK wait, and FIN close. Legacy `SYS_NET_*` are deprecated. |
| Socket API | 🧪 | AF_INET/SOCK_STREAM process-owned handles plus AF_INET/SOCK_DGRAM `sendto`/`recvfrom` exist. |
| TCP server (bind/listen/accept) | 🧪 | Server-side socket path is implemented with `tcp_listen()`/`tcp_accept()` and the `/tcpserver` minimal HTTP echo server. |
| virtio-net | 🧪 | Modern virtio PCI driver (`drivers/virtio_net/`) with a real data path: `VIRTIO_F_VERSION_1` negotiation, RX (queue 0) / TX (queue 1) split virtqueues, MAC read from device config, and a 12-byte `virtio_net_hdr` (no `MRG_RXBUF`). Registers as a netdev backend; selected when e1000 is absent. Validated end-to-end (DHCP + ICMP + DNS + TCP) under QEMU `-device virtio-net-pci`. Polling data path; no IRQ yet. |
| vmxnet3 / e1000e | 🚧 | Recognised by the virtual-driver probe, but no data path yet. Use e1000 or virtio-net. |

## Graphics and input

| Feature | Status | Notes |
|---|---:|---|
| Framebuffer console | ✅ | Limine-provided linear framebuffer. |
| PSF/bitmap font rendering | ✅ | Embedded console font. |
| 2D graphics | ✅ | Double-buffered drawing. |
| Window manager demo | ✅ | Windows, widgets, taskbar, mouse interaction. |
| PS/2 keyboard | ✅ | Scan-code set 1, ASCII + rich key-event queues. |
| PS/2 mouse | ✅ | IRQ 12, cursor/buttons and wheel-event support. |
| Kernel GUI/compositor | ✅ | **v2.0**: Theme engine (30+ params), desktop icons (32), notifications, window snapping (left/right/top/bottom/maximize), start menu, context menus, always-on-top windows, tool windows, edge/corner resize, double-click titlebar maximize, alpha blit, explicit event ABI (#define values), 64 windows, 128-event rings. Dirty-rect partial redraw is implemented via `compositor_render_dirty()` and `gfx_flip_rect()`, with idle frames skipping flips. Per-process window/icon cleanup on exit. |
| 3D software renderer | 🧪 | Demo renderer, CPU/SSE float math. |
| Native VBox/VMware SVGA drivers | ❌ | Limine framebuffer is used instead. |
| virtio-gpu 2D scanout | 🧪 | Modern virtio-gpu PCI probe, control queue, RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D and RESOURCE_FLUSH are implemented as an optional mirror path for `gfx_flip()`. The driver is initialised during graphics boot so mirroring is available before the GUI compositor starts. |
| virtio-gpu VirGL command transport | 🧪 | Feature negotiation, 3D context create/destroy, RESOURCE_CREATE_3D, context attach/detach, fenced SUBMIT_3D payload chains and TRANSFER_TO_HOST_3D are present. A tiny VirGL command-stream builder submits clear/framebuffer plus experimental vertex-buffer/triangle packet streams, and a full present pipeline now drives TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH to scan a 3D render target out to the display (with graceful software fallback when no virgl host is attached). Command-stream encoding is host-unit-tested (`test_virgl`). There is still no full OpenGL/Gallium state tracker. |

## Storage and USB

| Feature | Status | Notes |
|---|---:|---|
| AHCI detection/init | ✅/🧪 | Controller/port setup works in QEMU AHCI. |
| AHCI sector read/write | ✅/🧪 | DMA READ/WRITE self-test passes on the QEMU AHCI test disk. |
| UHCI controller | ✅/🧪 | Controller + port + CONTROL/BULK TD/QH transfers used by MSC. |
| OHCI controller | 🧪 | Detection/init/root-port reset plus ED/TD control, bulk and interrupt scheduling works in QEMU for HID/MSC. |
| EHCI controller | 🧪 | Detection/init/root-port reset plus async qTD control/bulk scheduling works in QEMU for high-speed MSC. Periodic interrupt and split transactions are pending. |
| xHCI controller | 🧪 | QEMU xHCI slot/address/device-context setup, endpoint contexts, command/event rings, control/bulk/interrupt transfer rings are implemented for HID/MSC. |
| USB device enumeration | 🧪 | UHCI/OHCI/EHCI/xHCI devices are enumerated through standard requests/controller-specific address setup. |
| USB HID keyboard/mouse | 🧪 | Boot Protocol/generic keyboards and pointer devices work through UHCI, OHCI, xHCI and high-speed EHCI polling; generic HID parser handles keyboard reports and mouse/tablet pointer reports (QEMU `usb-kbd`/`usb-mouse`/`usb-tablet` tested). EHCI full/low-speed split transactions remain future work. |
| USB hubs | 🧪 | Standard hub descriptor/status, port power/reset and downstream child enumeration work in QEMU, including xHCI route-string addressing for devices behind a hub. |
| USB hotplug monitor | 🧪 | Polling monitor scans root ports and known hubs, marks removed records and dynamically attaches HID and MSC class drivers for newly enumerated devices. QEMU xHCI HID and MSC attach/detach are integration-tested. |
| USB Mass Storage | 🧪 | Bulk-Only/SCSI path works through UHCI, OHCI, high-speed EHCI and xHCI in QEMU, including runtime hotplug attach/read/detach. Active media is exposed through `/usb` via usbfs (`info`, `sector0.bin`, `disk.img`) and FAT32 superfloppy/partition root files are auto-detected read-only under `/usb/fat`. |

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
| `/gcalc`, `/gedit`, `/gfiles`, `/gterm`, `/gsysmon`, `/gabout`, `/glaunch`, `/gusb` | 🧪 | GUI apps using `libauragui` v2.0; `/gusb` is the USB Manager for hotplug/storage status via `/usb`. `/gtheme` customizes window colors. |

## Known low-priority limitations

- **SMP scheduling is deliberately conservative.** APs are online, have CPU-local state and LAPIC timers, and enter the idle scheduler loop; normal user scheduling remains BSP-only until per-CPU run queues/TLB shootdown policy are completed.
- **TCP is intentionally minimal.** TCP receive uses timed waits over the IRQ-capable e1000 driver and has basic one-segment fixed-RTO retransmission for SYN/data/FIN, but the stack still supports a small fixed number of streams and lacks production features such as congestion control, sliding windows and richer packet queues.
- **Advanced filesystems are prototypes.** `ext4`, `btrfs`, `f2fs`, `ntfs` and `exfat` are scaffolding/experimental readers, not robust general-purpose filesystem implementations; timestamp support is currently best covered in VFS/tmpfs/diskfs/FAT32/ext2.
- **Hardware coverage is mostly virtualized.** Integration coverage is QEMU-first, with some VirtualBox/VMware-oriented device IDs; broad real-hardware validation is still pending.

## Highest-priority gaps

1. Harden address-space teardown for future SMP/TLB-shootdown support.
2. Audit remaining kernel-internal callers and expand fault-recovering uaccess tests.
3. Complete USB bulk/control transfer paths across OHCI/EHCI/xHCI.
4. Make scheduling SMP-aware or explicitly keep APs disabled in normal configs.
5. Grow `mmap` into lazy/shared VMAs and add broader file-backed sharing tests.
6. Tighten FD inheritance/lifetime semantics around `fork`, `execve` and process exit.
7. Expand shared/lazy VMAs and add guard pages around additional heap/stack regions.
