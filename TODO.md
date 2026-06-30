# AuraLite OS TODO

The historical 14-phase roadmap is complete. This file tracks the **current**
known limitations and future work for the post-phase tree. See
[`PLAN.md`](PLAN.md) for milestone history and [`docs/status.md`](docs/status.md)
for the feature matrix.

---

## Current Known Limitations

### Kernel / CPU / scheduling

- **SMP scheduling is conservative.** Application processors are online, load
  CPU-local state and enter the idle scheduler loop; normal user scheduling remains
  BSP-only until per-CPU run queues and TLB shootdown policy are completed.
- ~~**Address-space reaping is incomplete.**~~ **Done (H2):** dead TCBs,
  kernel stacks, process FDs, and user page-table/address-space frames are
  deferred-reaped from a safe stack via `thread_reap_zombies()` and
  `paging_free_address_space()`.
- ~~**Blocking model is primitive.**~~ **Done (H4):** wait queues back
  blocking pipes, futex waits, `select()`, and `nanosleep()`.
- **Interrupt model is transitional.** LAPIC enable and per-CPU LAPIC timers
  are implemented; IOAPIC routing and fully interrupt-driven device paths remain
  future work.

#### P10 / POSIX follow-ups

- **`MAP_SHARED` is not truly shared.** Anonymous `MAP_SHARED` is accepted but
  degraded to a private mapping (no cross-process shared page-cache VMAs yet),
  and file-backed `MAP_SHARED` returns `-ENOSYS`. Implement real shared VMAs
  with write-back before relying on `mmap`-based IPC.
- **execve passes argv/envp but no real auxv.** The initial process stack is
  built per the System V AMD64 ABI (argc/argv/NULL/envp/NULL) but the auxiliary
  vector contains only an `AT_NULL` terminator. Add `AT_PAGESZ`/`AT_RANDOM`/etc.
  when a dynamic loader needs them.
- **No `execvpe`/`fexecve`/`posix_spawn`.** Only `execve`/`execv`/`execvp`
  wrappers exist; `execvp` honours `PATH` (default `/bin`) with no per-segment
  `EACCES` retry semantics.
- **`epoll` is not implemented** (low priority): `poll()` is provided in libc on
  top of `select()`; `epoll_create1`/`epoll_ctl`/`epoll_wait` are unimplemented.

### Security / syscall robustness

- **User pointer validation is basic.** Syscall dispatch now uses
  `validate_user_range`, `copy_from_user` and `copy_to_user`, but AuraLite still
  lacks a fault-recovering uaccess mechanism and a full audit of every future
  user-pointer path.
- ~~**No `errno` or structured negative error codes.** Most failures return
  `-1`.~~ **Done (P1):** in-band negative-errno ABI; kernel returns `-EXXX`,
  libc decodes to `errno`/`-1`. See `docs/syscall_abi.md`.

#### errno follow-ups (discovered during P1)
- ~~**errno granularity is dispatch-layer, not native.**~~ **Done for `vfs.c`
  + tmpfs/initrd/procfs:** they return specific `-Exxx` (EBADF/ENOENT/EMFILE/
  ENOTDIR/EXDEV/ENOSYS/EROFS/ENOSPC/…); a `vfs_wrap_err()` helper maps any
  remaining generic `-1`. Still outstanding: push native errno into the **disk
  FS drivers** (fat32/ext2/diskfs — hundreds of internal block-I/O `-1`s,
  mostly EIO) and into `process.c`.

#### P2 / open-flags follow-ups
- ~~**Per-FD status flags, not shared OFDs.**~~ **Done (P3):** ref-counted
  `struct ofd` now holds offset + status flags, shared across dup/dup2/F_DUPFD/
  fork; FD_CLOEXEC stays per-fd.

#### P4 / signal follow-ups
- ~~alarm/pause/sigsuspend~~ **Done:** SYS_ALARM/PAUSE/SIGSUSPEND implemented
  (alarm via the PIT tick + signal_tick()).
- ~~SIGCHLD on child exit~~ **Done:** posted to a living parent in
  thread_exit_with_code (still wants a dedicated fork-based gate once fork is
  robust against the SYSCALL-save-area race).
- ~~SIGPIPE / -EINTR on blocking reads~~ **Done:** pipe-with-no-readers posts
  SIGPIPE + -EPIPE; stdin/pipe yield loops abort with -EINTR (or partial count).
- ~~**No full SA_RESTART rewind.**~~ **Done (H7):** restartable blocking
  syscalls save restart metadata on `-EINTR` and `sigreturn` re-dispatches them
  when the handler was installed with `SA_RESTART`.
- **SA_SIGINFO siginfo_t** not populated (handler gets signo only; rsi/rdx = 0).
- ~~**Ctrl+C/Ctrl+Z/Ctrl+\\ → SIGINT/SIGTSTP/SIGQUIT**~~ **Done (P5):** the
  console stdin path and /dev/tty0 line discipline generate these via ISIG and
  the tty->fg_pgid indirection. Full per-process-group routing arrives in P6.

#### P6 / job-control follow-ups
- **Interactive shell job control not implemented.** The kernel mechanism
  (pgid/sid, setpgid/tcsetpgrp, group signals, waitpid(WNOHANG)) is complete and
  tested; the userspace `init` shell rewrite (`cmd &`, `jobs`, `fg`, `bg`,
  setpgid+tcsetpgrp per spawned child, restore fg on exit) is deferred — high
  regression risk to the interactive shell, wants a QEMU boot to validate.
- **No stopped state / WUNTRACED.** SIGSTOP/SIGTSTP currently terminate (no
  THREAD_STOPPED state, no SIGCONT resume); WUNTRACED is accepted but never
  reports a stopped child. Needs a stopped scheduler state.
- **n_children is fork/spawn-tracked but not perfectly precise** across orphan
  adoption (a parent that exits without waiting leaves its count stale, but it
  is dead so this is benign). Revisit if a reparent-to-init policy is added.

#### P5 / TTY + stdio follow-ups
- **scanf/fscanf** not implemented (fgets + manual parsing only).
- **readline() line editor** (arrows/history, raw-mode shell input) deferred.
- **/dev/ttyS0** (UART serial tty) not registered; only /dev/tty0 exists.
- **init not rewired to /dev/tty0** — kept the existing fd-0 console path to
  avoid shell regressions; the line discipline runs only for programs that open
  /dev/tty0 directly. ISIG/Ctrl+C is bridged into the fd-0 path manually.
- **printf now line-buffered via stdout FILE*** — programs that print a prompt
  without a trailing newline must fflush(stdout) (POSIX-correct, but a behavior
  change worth a QEMU smoke test).
- **No column tracking** for ECHOE/tab-expansion/multi-column ^X erase; VERASE
  of a tab or control char erases a fixed 1–2 columns, not the true width.
- **VMIN/VTIME timers** are approximated (the syscall layer's yield loop honors
  VMIN counts; VTIME deciseconds timing is not yet wired to the PIT).
- ~~**FP/SSE state is not saved in the signal frame.**~~ **Done (H7):**
  signal delivery saves a 512-byte FXSAVE frame and `sigreturn` restores it.
- **Signal state is single-CPU safe only** (guarded by IF-disabled return
  boundaries); SMP needs atomic sig_pending updates and locking.
- **Job-control STOP** is treated as terminate for now (no stopped state until P6).
- **alarm()/signal_tick() scan all threads each tick** (O(threads)); fine at
  current scale, revisit with a timer wheel if thread counts grow.

#### P3 / OFD follow-ups
- **OFD refcounts are non-atomic.** Plain `int refcount`, safe only under the
  single-threaded VFS. SMP/preemptive FS access needs atomic dec-and-test
  (release/acquire ordering) plus a strict lock hierarchy (files-table → OFD
  offset → vnode) and an fget/fput temporary reference held across blocking I/O
  to avoid use-after-free.
- **`close_process_fds()` assumes the exiting thread is current** (vfs_close
  operates on `current_fd_table()`). True today (called with `self`); would be
  wrong if ever invoked on a non-current zombie — add a table-explicit close.
- **fork() FD-sharing integration test deferred.** Needs fork robust against the
  per-thread SYSCALL-save-area race; dup() sharing in test_lseek validates the
  same OFD mechanism meanwhile.
- **O_APPEND atomicity** currently relies on the single-threaded VFS; needs a
  per-vnode write lock once FS access becomes preemptible/SMP.
- **mkdir() still takes only a path** (no `mode_t`); POSIX `mkdir(path, mode)`
  and `umask` arrive in P7. `sys/stat.h` deliberately omits the mkdir prototype.
- **O_NONBLOCK** is honored for pipes (EAGAIN); devices/sockets that can block
  are not yet wired to it.
- **Socket/net syscalls return bare `-1`.** `SYS_SOCKET*` / `SYS_NET_*` failure
  paths propagate the layer's `-1`, which libc currently decodes as `EPERM`.
  Give them real errno values (`EBADF`, `ENOTCONN`, `ECONNREFUSED`, …).
- ~~**P1 libc headers still missing.**~~ **Done:** `limits.h`, `stdbool.h`,
  `assert.h`, `ctype.h` (+impl), `math.h` (+impl). `stdint.h`/`stdarg.h` use the
  freestanding compiler headers.
- **libm accuracy is series-based (~1e-9), not last-ULP**, and only covers the
  ten functions listed in `math.h`; no `tan/asin/atan2/fmod/modf/frexp`, no
  `float` variants, no errno/`HUGE_VAL` domain-error reporting. Revisit in P10.
- **`errno` is a single global, not thread-local.** Safe while single-threaded;
  must move behind TLS in `__errno_location()` during P9 (pthreads).
- **SYSCALL state uses globals.** Saved user `RCX/R11/RSP` state is not designed
  for true concurrent SMP syscalls.
- ~~**ELF final permissions are simplified.**~~ **Done (N4):** user
  ELFs now use page-aligned RX/R/NX/RW/NX `PT_LOAD` segments, and the kernel
  maps `PF_W`/`PF_X` into exact writable/NX PTE flags. User stacks are NX.

### Processes and file descriptors

- **FD semantics still need final POSIX precision.** FD numbers are per-process
  and `dup`, `pipe`, close-on-exec and OFD sharing exist, but exact shared-open-
  file-description semantics across every `fork`/`exec` edge case still need audit.
- **`fork`, `execve`, `wait4` are simplified.** They are sufficient for the
  bundled demos/tests, but not POSIX-complete.
- **User VM is still eager/simple.** `brk`, `mmap`, and `munmap` exist, but
  lazy VMAs and true file-backed `MAP_SHARED` remain future work.
- **IPC primitives are partial.** Pipes, signals, futexes, wait queues, baseline
  in-memory named FIFOs (`mkfifo`) and baseline in-memory symbolic links
  (`symlink`/`readlink`/`lstat`) exist; shared memory, hard links (`link`),
  persistent per-filesystem FIFO/symlink storage and full symlink path-component
  following remain future work.

### Storage / filesystems

- **AHCI is QEMU-focused.** DMA read/write passes the integration tests on QEMU
  AHCI disks, but broad hardware/hypervisor coverage is still experimental.
- **`/disk` is intentionally tiny.** Flat namespace, 8 files maximum, 4 KiB per
  file.
- **FAT32/ext2 are hobby implementations.** FAT32 supports subdirs/LFN and FAT date/time stat decoding, and ext2 supports Linux-mkfs images plus in-kernel mkfs with inode timestamps. Crash consistency, journaling, full permission semantics and extensive fsck-style recovery are out of scope.

### USB / devices

- **USB class support is still intentionally narrow.** UHCI/OHCI/EHCI/xHCI now
  have QEMU-tested paths for the supported HID/MSC cases, and hub downstream
  enumeration works including xHCI route strings. HID and MSC runtime
  attach/read/detach are QEMU-tested through the polling hotplug monitor, and
  active media is exposed at `/usb` through usbfs. FAT32 superfloppy/partition
  root files are auto-detected read-only under `/usb/fat`; writable FAT32, ext2
  hotplug automount, isochronous devices and broader hardware recovery paths are
  still future work.
- **USB HID generic support is partial.** Boot keyboard/mouse works through UHCI,
  OHCI, high-speed EHCI and xHCI; generic keyboard and mouse/tablet report
  descriptors are parsed for common QEMU-tested layouts. Full HID collections/usages
  and EHCI full/low-speed split transactions remain future work.
- **Bluetooth and Wi-Fi are protocol frameworks.** No complete lower-level
  chipset/transport driver is registered by default.

### Networking

- **Network I/O transition in progress.** e1000 now has an INTx IRQ-capable RX/TX core and software RX queue; TCP, ARP, DHCP, ICMP, and kernel UDP/DNS receive waits use bounded IRQ-backed NIC waits. UDP user sockets and a basic fixed-RTO TCP retransmission path are implemented; remaining N2 work is deeper socket blocking edge cases and production TCP features.
- **NIC backend abstraction (netdev).** The IP stack now talks to an active NIC through `kernel/net/netdev.{h,c}` rather than calling a driver directly. e1000 is the default; modern virtio-net (`drivers/virtio_net/`) is a fully working fallback (DHCP/ICMP/DNS/TCP validated under QEMU `-device virtio-net-pci`). virtio-net is currently a polling data path with no IRQ.
- **Minimal TCP.** User space now has process-owned socket-style handles, and
  the underlying TCP transport now has a one-segment fixed-RTO retransmission
  strategy for SYN/data/FIN. It still lacks congestion control, sliding windows
  and production-grade packet queues.
- **DHCP can fall back in QEMU SLIRP.** Integration tests tolerate fallback
  static addressing for deterministic boots.

### Graphics / GUI

- **GPU acceleration is early.** Limine framebuffer remains the primary GUI
  surface, while virtio-gpu 2D mirroring and the VirGL command transport are
  present as experimental acceleration paths. The VirGL path now completes a
  present pipeline (fenced SUBMIT_3D -> TRANSFER_TO_HOST_3D -> SET_SCANOUT ->
  RESOURCE_FLUSH) to scan a 3D render target out to the display, falling back to
  software rendering when no virgl-capable host GPU is attached. A full
  OpenGL/Gallium state tracker and userspace 3D API remain future work.
- **GUI is educational.** The kernel compositor, GUI syscalls and `libauragui`
  are functional in tests, and windows are cleaned up on client exit, but it is
  not yet a protected multi-client production desktop.

---

## Future Enhancements

### Memory management

- [x] Strict per-segment user ELF permissions and NX for user data/stack.
- [x] Basic user pointer validation and safe copy helpers for syscall dispatch.
- [ ] Fault-recovering user access and continued audits for new syscall paths.
- [x] Copy-on-write `fork`.
- [x] User `mmap` / `munmap` / `brk` baseline syscalls (eager private mappings; true lazy/shared VMAs remain future work).
- [x] Slab allocator for common fixed-size kernel objects.
- [x] Guard pages around kernel/user stacks, with explicit overflow diagnosis in the `#PF` handler (`kernel/proc/guard.c`; kernel-stack hit is fatal, user-stack hit → SIGSEGV). Heap-region guard pages remain future work.
- [ ] Large-page support for selected kernel mappings.
- [x] `paging_free_address_space()` walker (user half) with PMM accounting.
- [ ] Wire it on for all reaped zombies once TLB shootdown + per-PML4 refcounting land.

### Scheduling and processes

- [x] SMP-aware scheduler baseline: CPU-local current/idle state, global scheduler lock, AP idle scheduler loop. Per-CPU run queues remain future work.
- [x] Deferred TCB/kernel-stack reaper and missed-wakeup-safe wait notifications.
- [x] Full address-space/page-table reaping via `paging_free_address_space()` in `thread_reap_zombies()`.
- [x] BLOCKED state, wait queues and sleepable kernel primitives baseline.
- [ ] Real parent/child process table and precise `waitpid` semantics.
- [x] Basic per-process FD tables.
- [x] `dup`, `dup2`, `pipe`, `fcntl(F_GETFD/F_SETFD/FD_CLOEXEC)` syscalls + `execve` honouring `FD_CLOEXEC`.
- [x] `waitpid(pid, *exit_code)` with real exit-code propagation and zombie collection on wait.
- [ ] Precise POSIX shared-open-file description semantics across `fork`.
- [x] Signals and process notification baseline (`kill`, alarms, terminal signals, SIGCHLD, SA_RESTART).

### Filesystems and storage

- [ ] Broaden AHCI compatibility beyond the QEMU test path.
- [ ] Add fsck/recovery tooling or defensive consistency checks for FAT32/ext2.
- [x] Add baseline file timestamps for VFS stat plus tmpfs, diskfs, ext2 and FAT32. Remaining permission-mode persistence/audit across every experimental FS stays future work.
- [ ] Add symbolic links and richer path handling where useful.
- [ ] Add block cache and writeback policy instead of direct synchronous writes.
- [ ] Add virtio-blk / virtio-scsi or NVMe as modern virtual storage targets.

### Networking

- [x] Interrupt-capable e1000 RX/TX driver core (INTx, RX software queue, wait queues).
- [x] Rewire TCP receive waits to timed IRQ-backed NIC waits.
- [x] Rewire ARP/DHCP/ICMP and kernel UDP/DNS boot paths to bounded IRQ-backed NIC waits.
- [x] Add AF_INET/SOCK_DGRAM user sockets with `sendto(44)` / `recvfrom(45)`.
- [ ] Make remaining socket edge cases fully blocking.
- [x] Process-owned socket-style client handles (`socket/connect/send/recv/close`).
- [x] Per-connection TCP state (`tcp_handle_t`, up to `TCP_MAX_CONNS=8`).  Legacy `SYS_NET_*` syscalls are now a thin shim over the per-connection layer and are formally **deprecated**.
- [x] Full BSD socket ABI baseline including `sockaddr`, `bind`, `listen` and `accept` for AF_INET/SOCK_STREAM.
- [x] UDP user sockets.
- [x] Basic one-segment TCP retransmission and fixed RTO for SYN/data/FIN. Better packet queues, congestion control and sliding windows remain future work.
- [x] netdev NIC abstraction with boot-time backend selection (e1000 default, virtio-net fallback).
- [x] virtio-net modern data-path driver (RX/TX virtqueues, 12-byte hdr, MAC from device cfg).
- [ ] virtio-net IRQ-driven RX (currently polling); vmxnet3 / e1000e data-path drivers.

### USB and wireless

- [x] Add stable OHCI/EHCI/xHCI control/bulk backend API hooks into `usb_core`.
- [ ] Complete OHCI ED/TD transfer scheduling.
- [ ] Complete EHCI async/control/bulk qTD transfers and MSC backend.
- [ ] Complete xHCI command/event/transfer rings, slot addressing and endpoint contexts.
- [x] USB HID keyboard/mouse class drivers for UHCI Boot Protocol devices.
- [ ] Generic HID report parsing and OHCI/EHCI/xHCI HID transport.
- [ ] Real Bluetooth USB transport and at least one tested HCI controller path.
- [ ] Real Wi-Fi chipset driver backend for the existing 802.11 MAC layer.

### GUI and userspace

- [x] GUI dirty-rect compositor partial redraw (`compositor_render_dirty()` + `gfx_flip_rect()`).
- [x] Clean up GUI windows when the owning process exits.
- [x] Basic GUI process ownership enforcement for user-facing window syscalls.
- [x] Audit every GUI sub-op for out-of-range/negative wid and bad userspace pointers (with integration test).
- [ ] Stronger compositor/client isolation and permission model for GUI internals.
- [ ] More complete text input, clipboard and focus behavior.
- [ ] Persisted user settings/theme.
- [x] USB Manager GUI (`/gusb`) wired to `/usb` hotplug/storage status.
- [ ] More GUI apps and richer file editor/terminal behavior.
- [ ] Dynamic user-space allocation once `brk`/`mmap` exist.

### Infrastructure and docs

- [x] Sync `docs/status.md`, `TODO.md`, and `CHANGELOG.md` for completed H1–H8 hardening work.
- [ ] Keep `README.md`, `docs/status.md`, `docs/syscall_abi.md` and
      `docs/driver_guide.md` in sync with every future feature change.
- [ ] Add GDB helper scripts / pretty-printers for kernel structures.
- [ ] Reduce integration-test timing flakiness around process spawn and serial
      input pacing.
- [x] Add fsck-style FAT32/ext2 churn + reboot regression test case (`test_fs_stress.sh`).
- [x] Add integration cases for GUI bad-pointer hardening, process-exit GUI cleanup, FD lifecycle.
- [ ] Add CI artifacts for screenshots and QEMU serial logs on every failure.
