# AuraLite OS — Systems Hardening Plan

**Status: ALL H1–H8 COMPLETE; DOCUMENTATION SYNC COMPLETE** (`docs/status.md`, `TODO.md`, and `CHANGELOG.md` now match commit `db021e4`, 2026-06-30)

All 10 POSIX.1-2017 phases are complete (commit `35e58e0`, 2026-06-28).

This document tracks the 8 post-POSIX hardening milestones (H1–H8).

---

## H1 — GUI Dirty-Rect Compositor ✅ COMPLETE

**Objective:** Replace unconditional full-screen blit every frame with partial
redraws, cutting idle GPU bandwidth from ~300 MB/s to near zero.

### Changes made

| File | Change |
|---|---|
| `drivers/framebuffer/graphics.h` | Added `gfx_flip_rect()` declaration |
| `drivers/framebuffer/graphics.c` | Implemented `gfx_flip_rect()` — copies only the clipped sub-rectangle |
| `kernel/gui/gui.c` | Added `compute_dirty_union()` helper |
| `kernel/gui/gui.c` | Added `compositor_render_dirty()` — composites full back buffer, flips only dirty bbox |
| `kernel/gui/gui.c` | `gui_compositor_tick()`: removed forced `full_dirty=1`; added cursor movement tracking (prev+next position); idle frames now skip all work |
| `kernel/gui/gui.c` | Exported `SORT_ARR` as single file-scope macro (was duplicated in both render functions) |

### Gate criteria — verified

| Criterion | Result |
|---|---|
| Idle GUI (no events) → zero memcpy bytes/sec | ✅ Idle frames fall through the `else` branch, no `gfx_flip` called |
| Cursor movement → prev+next rect marked dirty | ✅ `gui_compositor_tick()` tracks `prev_mx/prev_my` |
| Window drag → old+new position dirty | ✅ `gui_move_window()` calls `mark_window_dirty()` twice (before+after) |
| GUI self-test passes | ✅ `[gui] PASS: GUI v2.0 subsystem rendered initial composition` in boot log |
| `make all` → 0 errors | ✅ Clean build |
| Unit tests → no regressions | ✅ All host-side unit tests pass |

### Definition of Done

- [x] Idle GUI → 0 memcpy bytes per second
- [x] Cursor movement tracked (prev+next rect dirty)
- [x] Single window drag → old+new dirty
- [x] `make all` zero warnings/errors
- [x] Boot GUI self-test PASS
- [x] `make test-unit` passes

---

## H2 — Address-Space Reaping Verification & Fix ✅ COMPLETE

**Status: VERIFIED & COMPLETE — already implemented**

`paging_free_address_space()` is fully implemented and called from
`thread_reap_zombies()` (kernel/proc/thread.c:420). Boot log shows:
```
[thread] reaped '/hello' (tid 6, 35 frames)
[thread] reaped '/execve_child' (tid 7, 38 frames)
```

**`docs/status.md` updated** — line 27 (`Thread/process reaping`) and
line 36 (`Copy-on-write`) have been updated to reflect full support.

### Definition of Done

- [x] Verified `paging_free_address_space()` called in `thread_reap_zombies()`
- [x] Boot log confirms `/hello` reaped (35 frames)
- [x] Integration test `test_memory_reaping.sh` passes successfully
- [x] Update `docs/status.md` line 27 (Thread/process reaping → ✅)
- [x] Update `docs/status.md` line 36 (Copy-on-write → ✅)

---

## H3 — Copy-on-Write fork() ✅ COMPLETE

**Status: VERIFIED & COMPLETE — already implemented**

`paging_clone_user_space()` performs mark-and-share COW fork (kernel/arch/x86_64/paging.c:210-271).
`paging_handle_cow_fault()` copies on first write.

**`docs/status.md` updated** — COW entry is verified as ✅.

### Definition of Done

- [x] Verified `paging_clone_user_space()` mark-and-share COW fork logic
- [x] Verified `paging_handle_cow_fault()` copy-on-write page fault handling
- [x] Added host unit test `tests/unit/test_cow.c` verifying flag modification and refcounting
- [x] Added QEMU integration test `tests/integration/cases/test_fork_cow.sh`
- [x] Verified clean build and successful test execution
- [x] `docs/status.md` COW entry verified as ✅

---

## H4 — True Blocking I/O (Wait Queues) ✅ COMPLETE

**Status: COMPLETE**

`wait_queue` structure and support functions implemented. `futex_wait()`, `nanosleep()`,
`pipe`, and `select()` converted from yield-polling loops to true blocking wait queues.

### New files required

- `kernel/proc/wait_queue.c` / `wait_queue.h`

### Tasks

- [x] `struct wait_queue` + `wq_wait()`, `wq_wake_one()`, `wq_wake_all()`
- [x] Pipe: replace `sched_yield()` poll with `wq_wait()` on `read_wq`/`write_wq`
- [x] `futex_wait()`: remove `sched_yield()` loop
- [x] `nanosleep()`: use `sleep_deadline` TCB field + PIT wake in `signal_tick()`
- [x] `select()`: rewrite to use per-OFD `read_wq`/`write_wq`

---

## H5 — TCP Server (bind / listen / accept) ✅ COMPLETE

**Status: COMPLETE**

Server-side TCP socket API implemented across kernel, syscalls, and libc.
Created minimal HTTP server in userspace (`/tcpserver`).

### New files / changes

- [x] `kernel/net/tcp.c`: add `TCP_LISTEN`, `tcp_listen()`, `tcp_accept()`
- [x] `kernel/arch/x86_64/syscall.c`: dispatch `SYS_SOCKET_BIND`/`SYS_SOCKET_LISTEN`/`SYS_SOCKET_ACCEPT`
- [x] `libc/src/libc.c`: `bind()`, `listen()`, `accept()` wrappers
- [x] `userspace/tcpserver/tcpserver.c`: minimal HTTP echo server
- [x] `tests/integration/cases/test_tcp_server.sh`: QEMU integration test passes

---

## H6 — Slab Allocator ✅ COMPLETE

**Status: COMPLETE**

Slab allocator implemented in `kernel/mm/slab.c`. Global caches created for `tcb_t`,
`struct ofd`, and `struct vnode`. Converted all kernel allocations to slab caches.

### New files required

- `kernel/mm/slab.c` / `kernel/mm/slab.h`
- `tests/unit/test_slab.c`

### Tasks

- [x] `slab_create()`, `slab_alloc()`, `slab_free()`
- [x] Global caches: `tcb_cache`, `ofd_cache`, `vnode_cache`
- [x] Replace `kmalloc(sizeof(tcb_t))` → `slab_alloc(tcb_cache)` in scheduler/thread
- [x] Host unit test: 10000 alloc/free → zero OOM

---

## H7 — SA_RESTART & Signal Frame FPU State ✅ COMPLETE

**Status: COMPLETE**

Implemented automatic restarting of interruptible syscalls via `SA_RESTART` and
preservation of FPU/SSE state in the user signal frame via `FXSAVE`/`FXRSTOR`.

### Tasks

- [x] `syscall_restart_num`, `syscall_restart_args[6]` in `tcb_t`
- [x] `is_restartable()` helper
- [x] `signal_deliver()`: save restart info on `-EINTR` + `SA_RESTART`
- [x] `sigreturn`: re-dispatch if restart pending
- [x] `fxsave_area[512]` in `signal_frame` (16-byte aligned)
- [x] `FXSAVE` on signal delivery, `FXRSTOR` on sigreturn

---

## H8 — SMP-Safe Scheduler ✅ COMPLETE

**Status: COMPLETE**

SMP-safe scheduler implemented with CPU-local data structures via `MSR_GS_BASE`,
run queue spinlock protection, and LAPIC initialization for BSP and APs.

### New files required

- `kernel/arch/x86_64/lapic.c` / `lapic.h`
- `kernel/arch/x86_64/cpu_local.h` / `cpu_local.c`

### Tasks

- [x] `struct cpu_local` with `GS:0` self-pointer
- [x] `cpu_local_init(cpu_id)` via `WRMSR 0xC0000101`
- [x] `sched_lock` spinlock on run queue
- [x] Replace `static current_thread` with `cpu_local()->current`
- [x] `lapic_enable()`, `lapic_timer_start(hz)`, `lapic_eoi()`
- [x] AP main: call `lapic_enable()` + `sched_idle()`

---

## N5.2–N5.3 — FIFO + symlinks ✅ COMPLETE

**Objective:** Add baseline POSIX-style named FIFO and symbolic-link support
without using the reserved GUI syscall range.

### Design

| Area | Rule |
|---|---|
| FIFO ABI | Add `mkfifo(path, mode)` as `SYS_MKFIFO=106`; existing anonymous `pipe`/`pipe2` remain unchanged. |
| FIFO storage | Keep named FIFO nodes in an in-memory VFS registry keyed by absolute path; data flow reuses the existing pipe ring and wait queues. |
| FIFO semantics | `stat/lstat` report FIFO type, `open` returns a normal fd backed by the shared FIFO ring, `read/write` honour blocking and `O_NONBLOCK`, and `lseek` returns `ESPIPE`. |
| Symlink ABI | Use existing `SYS_SYMLINK=88`, `SYS_READLINK=89`, `SYS_LSTAT=6`; `stat/open` follow final symlinks and `lstat/readlink` inspect the link itself. |
| Symlink storage | Keep baseline symlinks in an in-memory registry keyed by absolute link path, with bounded follow depth to avoid loops. |

### Tasks

- [x] Read affected docs, VFS, symlink stubs, syscall dispatch, libc and integration harness.
- [x] Add VFS FIFO type, named FIFO registry and `vfs_mkfifo()`.
- [x] Wire symlink create/readlink/lstat and VFS final-component following.
- [x] Add syscall dispatch/user-copy handling and `docs/syscall_abi.md` entry for `mkfifo`.
- [x] Add libc wrappers and stat type exports.
- [x] Add userspace probe plus integration gate.
- [x] Build, run QEMU FIFO/symlink gate, `make test-unit`, and smoke boot.
- [x] Update docs/status, TODO and CHANGELOG.

---

## N1 — VirGL Pipeline ✅ COMPLETE

**Objective:** Complete the VirGL/3D command path so a fenced SUBMIT_3D that renders
into a 3D render-target resource is actually presented to a display scanout, and add
host-side validation of the command-stream builder.

### Design

| Area | Rule |
|---|---|
| Present path | After a fenced `SUBMIT_3D`, issue `TRANSFER_TO_HOST_3D` for the render-target box, then `SET_SCANOUT` binding that 3D resource to scanout 0, then `RESOURCE_FLUSH`. |
| New transport ops | Add `virtio_gpu_set_scanout_resource()` and `virtio_gpu_flush_resource()` so any resource id (not just the fixed 2D mirror resource) can be scanned out/flushed. |
| VirGL helper | Add `virgl_present_render_target()` that drives transfer+scanout+flush for the default render target, and call it from the clear/triangle demos. |
| Safety | All new paths are no-ops returning `-1` when virtio-gpu/VirGL is unavailable; nothing runs in IRQ context and there is no allocation in IRQ context. |
| Validation | Add a host unit test for the command-stream builder (`virgl_cmd_*`) that checks packet opcode/length encoding and dword payloads without needing a GPU. |

### Tasks

- [x] Read virtio-gpu/VirGL driver, render3d demo wiring, and GPU tests.
- [x] Add `virtio_gpu_set_scanout_resource()` / `virtio_gpu_flush_resource()` transport ops.
- [x] Add `virgl_present_render_target()` and present after the demo submits.
- [x] Add a host unit test for the VirGL command-stream builder.
- [x] Build, run QEMU 3D gate, `make test-unit`, and smoke boot.
- [x] Update docs/status, TODO and CHANGELOG.

---

## N3 — virtio-net ✅ COMPLETE

**Objective:** Add a working modern virtio-net data path and a small netdev
abstraction so the IPv4/ARP/DHCP/UDP/TCP stack can run over either the existing
e1000 NIC (default) or virtio-net, selected at boot.

### Design

| Area | Rule |
|---|---|
| netdev abstraction | Add `kernel/net/netdev.{h,c}` with a `struct netdev` (`send`, `recv`, `recv_wait`, `get_mac`, `link_up`, `name`).  The stack calls `netdev_*` wrappers instead of `e1000_*` directly. |
| Backend registration | `e1000` registers itself as the default netdev; `virtio-net` registers when its modern PCI device (`1af4:1041`/`1af4:1000`) is present.  `net_init()` prefers e1000, falling back to virtio-net if e1000 is absent. |
| virtio-net driver | New `drivers/virtio_net/virtio_net.{h,c}`: modern virtio PCI discovery (common/notify/device cfg), feature negotiation (`VIRTIO_F_VERSION_1`, MAC), RX queue (0) prefilled with buffers, TX queue (1), `virtio_net_hdr` prepended on TX and stripped on RX. |
| virtio_net_hdr size | **12 bytes.** Under `VIRTIO_F_VERSION_1` the header always carries `num_buffers` regardless of `MRG_RXBUF` (which is *not* negotiated).  A 10-byte header shifted every transmitted frame by 2 bytes on the wire; the host unit test pins this at 12. |
| RX/TX | Non-blocking `recv` pops one completed buffer from the used ring, strips the header, copies the frame out, and recycles the descriptor; `recv_wait` bounds the wait via the PIT tick like e1000.  TX waits for used-ring completion.  No allocation or protocol parsing in IRQ context; the driver polls the used ring (consistent with the rest of the boot-time stack) and may add an IRQ later. |
| Safety | virtio-net is inert unless its PCI device is present; all new paths return errors gracefully when unavailable.  GUI syscall range untouched. |
| Validation | QEMU gate boots with `-device virtio-net,netdev=...` and asserts DHCP/ping/DNS over virtio-net; existing e1000 gates must still pass. |

### Tasks

- [x] Read net stack, e1000 driver, virtio PCI plumbing, and PCI helpers.
- [x] Add `kernel/net/netdev.{h,c}` abstraction and register e1000 as default.
- [x] Route `net.c` and `tcp.c` send/recv through `netdev_*`.
- [x] Implement `drivers/virtio_net/virtio_net.{h,c}` modern virtio-net data path.
- [x] Register virtio-net netdev and wire backend selection in `net_init()`.
- [x] Add a QEMU integration gate for virtio-net plus a host unit test.
- [x] Build, run e1000 + virtio-net gates, `make test-unit`, and smoke boot.
- [x] Update docs/status, TODO, CHANGELOG.

---

## N5.4 — Stack guard pages ✅ COMPLETE

**Objective:** Guarantee that a stack overflow (kernel or user) takes a page
fault on an unmapped guard page and is *diagnosed explicitly* as a stack
overflow, instead of silently corrupting adjacent memory or being lost in a
generic page-fault dump.

### Design

| Area | Rule |
|---|---|
| Mechanism | Already present: each kernel-thread stack slot is laid out `[low guard page][16 KiB usable][high guard page]` (`THREAD_STACK_GUARD_PAGES` each side); user stacks have one unmapped guard page below the lowest mapped byte. This task adds *classification/diagnosis* on top of that mechanism. |
| Classifier | New `kernel/proc/guard.{h,c}` exposes `guard_classify_fault(cr2, from_user, &desc)` returning `GUARD_FAULT_{NONE,KERNEL_STACK_LOW,KERNEL_STACK_HIGH,USER_STACK}`. Kernel detection is exact (current thread's mapped slot bounds); user detection uses the fixed high-VA stack window (`USER_STACK_TOP`, size, entropy slack, guard page). |
| #PF handler | `isr.c` calls the classifier on every `#PF` (after COW/uaccess recovery so recoverable faults aren't misreported). A hit prints `[GUARD] <desc>: CR2/RIP`. Kernel-stack guard hit → dump + `kernel_halt()` (fatal). User-stack guard hit → fall through to the normal SIGSEGV path (kill/handler), with the `[GUARD]` line recording the cause. |
| Safety | No new syscalls; GUI range untouched. Classifier is read-only address math, runs in fault context with no allocation. Does not alter the existing ELF-permission fault path (those addresses are outside the stack windows → `GUARD_FAULT_NONE`). |
| Validation | Userspace `/stackguard` overflows its stack; QEMU gate asserts `[GUARD] user stack overflow` + USER-mode `#PF` + shell survival + no bypass/panic. Host unit test pins the classification boundaries. |

### Tasks

- [x] Read kernel/user stack allocators, TCB stack fields, and the `#PF` handler.
- [x] Add `kernel/proc/guard.{h,c}` classifier (kernel exact + user window).
- [x] Wire classification into the `isr.c` `#PF` path (kernel fatal, user SIGSEGV).
- [x] Add `userspace/stackguard/` overflow probe + initrd packaging.
- [x] Add `tests/integration/cases/test_stack_guard.sh` and a host unit test.
- [x] Build, run the new gate + fault-path regressions, `make test-unit`, smoke boot.
- [x] Update docs/status, TODO, CHANGELOG.

---

## New Files Summary

```
kernel/
├── arch/x86_64/
│   ├── lapic.c / lapic.h          # H8: LAPIC enable + timer
│   └── cpu_local.h                # H8: per-CPU data + GS base
├── proc/
│   └── wait_queue.c / wait_queue.h # H4: blocking wait primitives
└── mm/
    └── slab.c / slab.h            # H6: slab allocator
tests/
├── unit/
│   ├── test_slab.c                # H6: host unit test
│   └── test_cow.c                 # H3: host unit test
└── integration/cases/
    ├── test_gui_dirty_rect.sh     # H1
    ├── test_memory_reaping.sh     # H2
    ├── test_fork_cow.sh           # H3
    └── test_tcp_server.sh         # H5
userspace/
└── tcpserver/
    └── tcpserver.c                # H5: minimal HTTP server
```


---

## N4 — Strict ELF Permissions & NX ✅ COMPLETE

**Objective:** Enforce exact user ELF segment permissions and non-executable
user data/stack mappings.

### Design

| Area | Rule |
|---|---|
| PT_LOAD segment flags | Start with `PAGE_FLAG_PRESENT | PAGE_FLAG_USER`. Add `PAGE_FLAG_WRITABLE` only when `PF_W` is present. Add `PAGE_FLAG_NO_EXEC` when `PF_X` is absent. |
| Overlapping PT_LOAD pages | Merge permissions conservatively: writable if any overlapping segment needs write; executable if any overlapping segment needs execute. |
| User stack | Map writable user stack pages with `PAGE_FLAG_NO_EXEC`; leave guard pages unmapped. |
| Fault behavior | User write-to-text or execute-from-data must raise #PF/#GP mapped to `SIGSEGV`, terminate only the faulting process, and keep the shell/kernel alive. |

### Tasks

- [x] Read affected files (`kernel/proc/elf.c`, `kernel/proc/process.c`, paging, ISR/signal path, Makefile, integration harness).
- [x] Confirm kernel ELF loader already derives final PTE permissions from `p_flags`.
- [x] Confirm user stack mappings are NX.
- [x] Update `libc/user.ld` to emit split page-aligned PT_LOAD segments.
- [x] Add dedicated `/elfperm` userspace probe.
- [x] Add `tests/integration/cases/test_elf_permissions.sh`.
- [x] Build with `make clean && make all`.
- [x] Run host unit tests (`make test-unit` passes).
- [x] Run N4 integration test (`test_elf_permissions`).
- [x] Update `docs/status.md`, `TODO.md`, and `CHANGELOG.md`.


---

## N2.1 — Interrupt-Capable e1000 RX/TX ✅ COMPLETE

**Objective:** Replace pure e1000 polling with an interrupt-capable driver core while preserving existing synchronous boot/network paths.

### Design

| Area | Rule |
|---|---|
| IRQ mode | Use legacy PCI INTx first. Read PCI interrupt line from config offset `0x3C`, register with `irq_register_handler()`, and enable RX/TX interrupt causes in `IMS`. |
| IRQ handler | No allocation and no protocol parsing in IRQ context. Read `ICR`, drain completed RX descriptors into a preallocated software RX ring, reclaim TX completions, and wake wait queues. |
| Compatibility | Keep `e1000_recv()` non-blocking so DHCP/ARP/ICMP/TCP polling loops keep working. Add `e1000_recv_blocking()` for future socket/TCP blocking receive paths. |
| RX queueing | Introduce a fixed software RX ring of packet buffers inside the driver. IRQ and opportunistic callers drain hardware into it; `e1000_recv()` pops from it. |
| TX behavior | Keep existing bounded TX wait as fallback, but add TX wait queue/reclaim hooks so future send paths can block instead of busy-spinning. |

### Tasks

- [x] Read affected files (`drivers/e1000`, PCI, IRQ/PIC, net/TCP/socket, wait queues, integration harness).
- [x] Add PCI interrupt-line helper.
- [x] Add e1000 IRQ registers, causes, handler, and wait queues.
- [x] Add e1000 software RX ring and hardware drain helper.
- [x] Preserve non-blocking `e1000_recv()` and add `e1000_recv_blocking()`.
- [x] Keep TX timeout fallback while adding TX wake support.
- [x] Add/update integration coverage for IRQ-capable networking.
- [x] Build/test full ISO and run the e1000 IRQ integration gate (`test_e1000_irq`).
- [x] Update docs/status, TODO, CHANGELOG.


---

## N2.2 — Timed NIC waits for TCP receive paths ✅ COMPLETE

**Objective:** Move TCP receive waits from tight CPU polling to IRQ/wait-queue-backed timed waits while keeping boot-critical DHCP/ARP compatibility paths stable.

### Design

| Area | Rule |
|---|---|
| NIC wait API | Add `e1000_recv_wait(buf, size, timeout_ticks)`: nonzero timeout sleeps on `e1000_rx_wq` with `sleep_deadline`; zero timeout blocks indefinitely. |
| TCP receive | Replace `TCP_RECV_POLLS` busy loops in `tcp_recv_segment()` and `tcp_recv_syn()` with deadline-based calls to `e1000_recv_wait()`. |
| Compatibility | Preserve existing timeout/fallback behavior in TCP server integration fallback and TCP connect/send/recv semantics. Leave DHCP/ARP/ICMP polling-compatible loops unchanged in this subphase. |
| Safety | No allocation in IRQ context; no protocol parsing in IRQ context; timed waits only from schedulable context. |

### Tasks

- [x] Read TCP/socket/syscall wait paths and existing timer wake mechanism.
- [x] Add `e1000_recv_wait()` API.
- [x] Convert TCP receive loops to timed NIC waits.
- [x] Syntax-check changed files and run unit tests.
- [x] Update docs/status, TODO, CHANGELOG.

---

## N2.4 — TCP retransmission buffer / RTO ✅ COMPLETE

**Objective:** Add a minimal one-segment TCP retransmission buffer and fixed RTO
for SYN, data, and FIN segments so the stack no longer relies entirely on a
single successful transmit/ACK round-trip.

### Design

| Area | Rule |
|---|---|
| RTO | Use fixed `TCP_RTO_TICKS=20` and `TCP_MAX_RETRIES=3` for the current one-segment-in-flight stack. |
| Retransmission buffer | Add one fixed `TCP_MSS`-sized retransmission slot per `tcp_conn_t`; no heap allocation. |
| Send path | Record SYN/data/FIN segments with explicit seq/ack and retransmit the saved bytes when RTO expires. |
| Receive path | Add a timed TCP receive helper so RTO waits do not consume the older broad TCP receive timeout. |
| Safety | No IRQ-context work, no allocation, no protocol parsing in the NIC IRQ handler. |

### Tasks

- [x] Read TCP/e1000/networking tests and docs before writing code.
- [x] Add per-connection fixed retransmission slot and helpers.
- [x] Apply RTO retries to SYN, data ACK wait, and FIN close.
- [x] Syntax-check changed TCP files.
- [x] Build, run QEMU networking/TCP gates, and unit tests.
- [x] Update docs/status, TODO, CHANGELOG.

---

## N2.3b — UDP user sockets ✅ COMPLETE

**Objective:** Add process-owned AF_INET/SOCK_DGRAM sockets using the IRQ-backed
UDP receive path and standard `sendto`/`recvfrom` syscalls.

### Design

| Area | Rule |
|---|---|
| Syscall ABI | Use roadmap numbers `SYS_SENDTO=44` and `SYS_RECVFROM=45`; these are free in the current syscall table. |
| UDP primitives | Export `net_udp_sendto()` and `net_udp_recvfrom()` from `kernel/net/net.c`; keep protocol parsing outside IRQ context. |
| Socket state | Allow `AF_INET/SOCK_DGRAM`, track bound local port, auto-bind an ephemeral source port on first `sendto()`. |
| User ABI | Add POSIX-shaped libc wrappers taking `struct sockaddr_in` and `socklen_t`. |
| Gate | Add `/udptest`, which performs a DNS A query using UDP sockets and expects a response from QEMU SLIRP DNS. |

### Tasks

- [x] Read syscall/socket/net/libc/userspace/integration files and verify `44/45` are free.
- [x] Export kernel UDP send/receive primitives.
- [x] Add socket-layer `sendto`/`recvfrom` for SOCK_DGRAM.
- [x] Wire syscall dispatch and usercopy validation.
- [x] Add libc declarations/wrappers and syscall ABI docs.
- [x] Add `/udptest` and an integration gate.
- [x] Build, run QEMU gates, run unit tests.
- [x] Update docs/status, TODO, CHANGELOG.

---

## N5.1 — File timestamps ✅ COMPLETE

**Objective:** Populate and update `stat()` `mtime`/`ctime`/`atime` across the
VFS and primary writable filesystems using the existing realtime clock support.

### Design

| Area | Rule |
|---|---|
| ABI | Keep the existing `struct vfs_stat` / userspace `struct stat` layout; no new syscall. |
| VFS time source | Add `vfs_now()` as a seconds-resolution wrapper around `kernel_time(NULL)`. |
| Vnode metadata | Add `mtime`, `ctime`, and `atime` to `struct vnode`; default `vfs_stat()` exports them. |
| Mutation rules | Creation sets all three timestamps; reads update `atime`; writes/truncate/chmod/chown update `mtime`/`ctime`; unlink/removal metadata updates stay filesystem-local where possible. |
| Filesystems | Wire tmpfs, diskfs, ext2 and FAT32 stat/update paths first; read-only/synthetic FS may report zero when unknown. |
| Tests | Add a userspace timestamp probe and QEMU integration gate covering create/write/read/truncate/stat ordering. |

### Tasks

- [x] Read VFS/stat, time, libc stat ABI, tmpfs, diskfs, FAT32, ext2 and integration harness before writing code.
- [x] Add VFS timestamp helpers and vnode fields.
- [x] Wire tmpfs timestamps.
- [x] Wire diskfs persistent timestamps.
- [x] Wire ext2 inode timestamps.
- [x] Wire FAT32 stat timestamp decoding/update coverage.
- [x] Add userspace/integration timestamp gate.
- [x] Build, run QEMU timestamp gate, and unit tests.
- [x] Update docs/status, TODO, CHANGELOG.

---

## N2.3a — IRQ-backed waits for ARP/DHCP/ICMP/UDP boot paths ✅ COMPLETE

**Objective:** Remove remaining busy receive loops in the boot-critical ARP,
DHCP, ICMP, and kernel UDP/DNS paths by routing bounded waits through the
IRQ-backed e1000 RX wait queue while preserving existing timeout/fallback
semantics.

### Design

| Area | Rule |
|---|---|
| Shared receive helper | Add a local `net_recv_wait_until(buf, size, deadline_ticks)` helper in `kernel/net/net.c`. It computes the remaining timeout and calls `e1000_recv_wait()`. |
| Protocol parsing | Keep all ARP/DHCP/ICMP/UDP parsing in normal kernel context, never in the NIC IRQ handler. |
| Timeouts | Preserve existing 1s/2s tick deadlines so DHCP fallback behavior and network self-test branching stay unchanged. |
| ABI | No new syscall and no libc/userspace ABI change in this subphase. |

### Tasks

- [x] Read affected files (`kernel/net/net.c`, `net.h`, `drivers/e1000`, integration networking gates, docs).
- [x] Add the shared bounded NIC wait helper.
- [x] Convert ARP, ICMP, kernel UDP/DNS, and DHCP receive loops to timed NIC waits.
- [x] Syntax-check changed networking files.
- [x] Run `make clean && make all`.
- [x] Run QEMU networking/e1000/ELF gates and `make test-unit`.
- [x] Update docs/status, TODO, CHANGELOG.

---

## Workflow (mandatory for every phase)

```
1. READ    — read ALL affected files before writing anything
2. PLAN    — update HARDENING_PLAN.md: Status → IN PROGRESS
3. DESIGN  — show struct/API changes, list callers
4. IMPL    — kernel → syscall dispatch → libc → userspace → tests
             compile after every file; fix warnings immediately
5. BUILD   — make clean && make all  (zero warnings, -Wall -Wextra -Werror)
6. TEST    — make run + gate criterion; make test-unit
             run ALL existing integration tests (must not regress)
7. DOCS    — update HARDENING_PLAN.md [x], CHANGELOG.md, docs/status.md
```
