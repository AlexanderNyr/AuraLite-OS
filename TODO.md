# AuraLite OS TODO

The historical 14-phase roadmap is complete. This file tracks the **current**
known limitations and future work for the post-phase tree. See
[`PLAN.md`](PLAN.md) for milestone history and [`docs/status.md`](docs/status.md)
for the feature matrix.

---

## Current Known Limitations

### Kernel / CPU / scheduling

- **Scheduler is not SMP-safe.** Application processors are brought online and
  load the shared tables, but they idle instead of running the normal scheduler.
- **No thread/process reaping.** Dead TCBs, kernel stacks and some process
  resources are leaked after exit.
- **Blocking model is primitive.** Waits are mostly yield/poll loops; there are
  no general wait queues or sleepable locks.
- **PIC/PIT legacy interrupt model.** LAPIC/IOAPIC and per-CPU LAPIC timers are
  not implemented.

### Security / syscall robustness

- **No user pointer validation.** Syscalls trust pointers supplied by Ring 3.
  Robust `copy_from_user` / `copy_to_user` helpers are still needed.
- **No `errno` or structured negative error codes.** Most failures return `-1`.
- **SYSCALL state uses globals.** Saved user `RCX/R11/RSP` state is not designed
  for true concurrent SMP syscalls.
- **ELF final permissions are simplified.** User segments are loaded with broad
  writable/user mappings; strict per-segment R/W/X enforcement is future work.

### Processes and file descriptors

- **Global FD table.** File descriptors are system-wide, not per-process.
- **`fork`, `execve`, `wait4` are simplified.** They are sufficient for the
  bundled demos/tests, but not POSIX-complete.
- **No user heap management syscalls.** `mmap`, `munmap`, `brk` are missing.
- **No IPC primitives.** No pipes, signals, futexes or shared memory.

### Storage / filesystems

- **AHCI is QEMU-focused.** DMA read/write passes the integration tests on QEMU
  AHCI disks, but broad hardware/hypervisor coverage is still experimental.
- **`/disk` is intentionally tiny.** Flat namespace, 8 files maximum, 4 KiB per
  file.
- **FAT32/ext2 are hobby implementations.** FAT32 supports subdirs/LFN and ext2
  supports Linux-mkfs images plus in-kernel mkfs, but crash consistency,
  journaling, permissions and extensive fsck-style recovery are out of scope.

### USB / devices

- **USB MSC works through UHCI only.** OHCI/EHCI/xHCI have detection/bring-up
  scaffolding, but their transfer engines are not wired to class drivers.
- **No USB HID input path.** Keyboard/mouse input is currently PS/2 for normal
  interaction.
- **Bluetooth and Wi-Fi are protocol frameworks.** No complete lower-level
  chipset/transport driver is registered by default.

### Networking

- **Polling I/O.** e1000 RX/TX and the higher network stack are polling-based.
- **Minimal TCP.** Single client connection, no retransmission strategy,
  congestion control, sliding windows or BSD sockets.
- **DHCP can fall back in QEMU SLIRP.** Integration tests tolerate fallback
  static addressing for deterministic boots.

### Graphics / GUI

- **Framebuffer-only graphics.** VirtualBox/VMware/QEMU native GPU acceleration
  is not implemented; Limine's framebuffer is used.
- **GUI is educational.** The kernel compositor, GUI syscalls and `libauragui`
  are functional in tests, but not a protected multi-client production desktop.

---

## Future Enhancements

### Memory management

- [ ] Strict per-segment user ELF permissions and NX for user data/stack.
- [ ] User pointer validation and safe copy helpers.
- [ ] Copy-on-write `fork`.
- [ ] User `mmap` / `munmap` / `brk`.
- [ ] Slab allocator for common fixed-size kernel objects.
- [ ] Guard pages around kernel/user stacks and heap regions.
- [ ] Large-page support for selected kernel mappings.

### Scheduling and processes

- [ ] SMP-aware scheduler with per-CPU run queues or explicit AP-off normal mode.
- [ ] Thread/process reaper and resource lifetime management.
- [ ] BLOCKED state, wait queues and sleepable kernel primitives.
- [ ] Real parent/child process table and precise `waitpid` semantics.
- [ ] Per-process FD tables with close-on-exec, `dup`, pipes and inheritance.
- [ ] Signals or another process notification mechanism.

### Filesystems and storage

- [ ] Broaden AHCI compatibility beyond the QEMU test path.
- [ ] Add fsck/recovery tooling or defensive consistency checks for FAT32/ext2.
- [ ] Add file timestamps/permission handling consistently across all FS drivers.
- [ ] Add symbolic links and richer path handling where useful.
- [ ] Add block cache and writeback policy instead of direct synchronous writes.
- [ ] Add virtio-blk / virtio-scsi or NVMe as modern virtual storage targets.

### Networking

- [ ] Interrupt-driven e1000 RX/TX.
- [ ] Multiple TCP sockets/connections.
- [ ] BSD socket API (`socket`, `bind`, `listen`, `accept`, `connect`, `send`,
      `recv`).
- [ ] UDP user sockets.
- [ ] Retransmission, timeouts and better packet queues.
- [ ] virtio-net / vmxnet3 / e1000e data-path drivers.

### USB and wireless

- [ ] Complete OHCI transfer scheduling and wire it into `usb_core`.
- [ ] Complete EHCI async/control/bulk transfers and MSC backend.
- [ ] Complete xHCI command/event/transfer rings and endpoint contexts.
- [ ] USB HID keyboard/mouse class drivers.
- [ ] Real Bluetooth USB transport and at least one tested HCI controller path.
- [ ] Real Wi-Fi chipset driver backend for the existing 802.11 MAC layer.

### GUI and userspace

- [ ] Better process isolation for GUI clients and compositor state.
- [ ] More complete text input, clipboard and focus behavior.
- [ ] Persisted user settings/theme.
- [ ] More GUI apps and richer file editor/terminal behavior.
- [ ] Dynamic user-space allocation once `brk`/`mmap` exist.

### Infrastructure and docs

- [ ] Keep `README.md`, `docs/status.md`, `docs/syscall_abi.md` and
      `docs/driver_guide.md` in sync with every feature change.
- [ ] Add GDB helper scripts / pretty-printers for kernel structures.
- [ ] Reduce integration-test timing flakiness around process spawn and serial
      input pacing.
- [ ] Add CI artifacts for screenshots and QEMU serial logs on every failure.
