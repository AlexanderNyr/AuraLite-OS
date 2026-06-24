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
- **Address-space reaping is incomplete.** Dead TCBs and kernel stacks are now
  deferred-reaped from a safe stack, and process FDs are closed on exit, but
  user page-table/address-space frames are still leaked until the VMM grows a
  full address-space free walker.
- **Blocking model is primitive.** Waits are mostly yield/poll loops; there are
  no general wait queues or sleepable locks.
- **PIC/PIT legacy interrupt model.** LAPIC/IOAPIC and per-CPU LAPIC timers are
  not implemented.

### Security / syscall robustness

- **User pointer validation is basic.** Syscall dispatch now uses
  `validate_user_range`, `copy_from_user` and `copy_to_user`, but AuraLite still
  lacks a fault-recovering uaccess mechanism and a full audit of every future
  user-pointer path.
- **No `errno` or structured negative error codes.** Most failures return `-1`.
- **SYSCALL state uses globals.** Saved user `RCX/R11/RSP` state is not designed
  for true concurrent SMP syscalls.
- **ELF final permissions are simplified.** User segments are loaded with broad
  writable/user mappings; strict per-segment R/W/X enforcement is future work.

### Processes and file descriptors

- **FD semantics are simplified.** FD numbers are now per-process, but there is
  still no `dup`, `pipe`, close-on-exec or precise POSIX shared-open-file
  description behavior after `fork`.
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
- **Minimal TCP.** User space now has process-owned socket-style handles, but
  the underlying TCP transport still supports one active stream, with no
  retransmission strategy, congestion control or sliding windows.
- **DHCP can fall back in QEMU SLIRP.** Integration tests tolerate fallback
  static addressing for deterministic boots.

### Graphics / GUI

- **Framebuffer-only graphics.** VirtualBox/VMware/QEMU native GPU acceleration
  is not implemented; Limine's framebuffer is used.
- **GUI is educational.** The kernel compositor, GUI syscalls and `libauragui`
  are functional in tests, and windows are cleaned up on client exit, but it is
  not yet a protected multi-client production desktop.

---

## Future Enhancements

### Memory management

- [ ] Strict per-segment user ELF permissions and NX for user data/stack.
- [x] Basic user pointer validation and safe copy helpers for syscall dispatch.
- [ ] Fault-recovering user access and continued audits for new syscall paths.
- [ ] Copy-on-write `fork`.
- [ ] User `mmap` / `munmap` / `brk`.
- [ ] Slab allocator for common fixed-size kernel objects.
- [ ] Guard pages around kernel/user stacks and heap regions.
- [ ] Large-page support for selected kernel mappings.

### Scheduling and processes

- [ ] SMP-aware scheduler with per-CPU run queues or explicit AP-off normal mode.
- [x] Deferred TCB/kernel-stack reaper and missed-wakeup-safe wait notifications.
- [ ] Full address-space/page-table reaping and stronger parent/child lifetime management.
- [ ] BLOCKED state, wait queues and sleepable kernel primitives.
- [ ] Real parent/child process table and precise `waitpid` semantics.
- [x] Basic per-process FD tables.
- [ ] Close-on-exec, `dup`, pipes and precise FD inheritance/lifetime semantics.
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
- [x] Process-owned socket-style client handles (`socket/connect/send/recv/close`).
- [ ] Multiple simultaneous TCP connections in the transport layer.
- [ ] Full BSD socket ABI including `sockaddr`, `bind`, `listen` and `accept`.
- [ ] UDP user sockets.
- [ ] Retransmission, timeouts and better packet queues.
- [ ] virtio-net / vmxnet3 / e1000e data-path drivers.

### USB and wireless

- [x] Add stable OHCI/EHCI/xHCI control/bulk backend API hooks into `usb_core`.
- [ ] Complete OHCI ED/TD transfer scheduling.
- [ ] Complete EHCI async/control/bulk qTD transfers and MSC backend.
- [ ] Complete xHCI command/event/transfer rings, slot addressing and endpoint contexts.
- [ ] USB HID keyboard/mouse class drivers.
- [ ] Real Bluetooth USB transport and at least one tested HCI controller path.
- [ ] Real Wi-Fi chipset driver backend for the existing 802.11 MAC layer.

### GUI and userspace

- [x] Clean up GUI windows when the owning process exits.
- [x] Basic GUI process ownership enforcement for user-facing window syscalls.
- [ ] Stronger compositor/client isolation and permission model for GUI internals.
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
- [x] Add fsck-style FAT32/ext2 churn + reboot regression test case (`test_fs_stress.sh`).
- [ ] Add CI artifacts for screenshots and QEMU serial logs on every failure.
