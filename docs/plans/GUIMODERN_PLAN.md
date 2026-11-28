# GUIMODERN_PLAN.md

**Status (2026-11-28): G0 audit is established; G1a implementation is integrated, the x86_64 kernel build passes, and targeted tests for the G1a changes pass. The broader test suite currently encounters an unrelated FS-stage failure during parallel filesystem development.** Do not mark unrelated FS failures as G1a regressions.

This plan tracks the migration of AuraLite-OS GUI ownership from kernel space to userspace. It is intentionally incremental: preserve working rendering code and public client APIs where possible, move ownership boundaries first, and remove `kernel/gui/` only after the userspace replacement is demonstrably complete.

## 1. Target architecture

```text
User applications
   ├── RSBR-GUI       Rust/no_std client API
   └── libauragui     C client API
          │
          │ IPC control messages + shared surfaces
          ▼
       agfxd           userspace GUI server process
          │
          │ uses
          ▼
       libagfx         Rust GUI backend library
          ├── Window Manager / WindowStore
          ├── compositor and dirty-region tracking
          ├── desktop, wallpaper, cursor, theme
          ├── input routing and focus policy
          └── rendering backends

Kernel (mechanisms only)
   ├── address spaces, VMAs and shared-memory mappings
   ├── IPC transport and capability validation
   ├── input event delivery
   ├── display/GPU device interfaces and ownership
   ├── scheduler, timers, process lifecycle
   └── no desktop policy, window manager or GUI compositor
```

`libagfx` is a library, not a daemon. `agfxd` is the process that owns the display session and instantiates `libagfx`. `RSBR-GUI` and `libauragui` remain client-facing libraries; their public APIs should remain stable where practical while their implementation changes from privileged GUI syscalls to IPC and shared mappings.

## 2. Baseline and known source facts

- The current GUI has a C ownership/control layer in `kernel/gui/gui.c` and related syscall code, plus Rust rendering/composition/desktop code.
- The C side currently owns window lifecycle, focus, z-order, parenting, hit-testing, event queues, surface allocation and compositor orchestration.
- Rust already contains useful rendering work: compositor snapshots, dirty regions, drawing, desktop code and MSDF text. These are migration candidates, not automatic rewrite targets.
- Current C window surfaces are backed by kernel-managed physical frames/HHDM mappings. Userspace cannot safely inherit that implementation by changing pointer types; it needs kernel-managed shared objects and explicit mappings into both client and compositor address spaces.
- SysV SHM (`kernel/ipc/sysvipc.c`) and anonymous shared memory (`kernel/mm/shmem.c`) are separate systems. Do not turn SysV IPC into a GUI-specific API.
- The VMA list and lock currently live in each TCB. `CLONE_VM` copies the list pointer but not a shared lock/list owner. This remains a known correctness issue and is not solved by the current G1a work.
- GUI migration must remain portable at the ABI/protocol level across x86_64, RV64 and AArch64. Architecture-specific mapping and device operations stay behind kernel interfaces.

## 3. Decisions and invariants

1. **Userspace owns GUI policy.** Window management, focus, z-order, desktop composition and client protocol semantics belong in `agfxd`/`libagfx`.
2. **Kernel owns mechanisms.** The kernel maps pages, authenticates IPC senders, validates capabilities, delivers input and exposes display/GPU mechanisms. It does not decide window policy.
3. **Shared pixels do not travel through syscall copies.** Surface storage is a kernel-managed shared-memory object mapped into the client and `agfxd`; control messages carry metadata and synchronization, not full pixel buffers.
4. **Identity is kernel-authenticated.** Do not trust a caller-supplied PID. Derive the sender from the active task and validate object capabilities.
5. **Handles must resist reuse.** Window/surface capabilities need generation or equivalent stale-handle protection and revocation on process exit.
6. **Preserve ABI semantics.** Keep event numeric values, window flags and existing client API shape where possible; write compatibility adapters instead of forcing an immediate application rewrite.
7. **No premature removal.** Keep the existing GUI path until userspace owns the equivalent lifecycle, input, surface and display paths and passes the migration gate.

## 4. Phases

### G0 — Source and ownership audit

**Status: substantially complete; maintain as new code is inspected.**

Deliverables:
- Map every current GUI responsibility to its owner and call path.
- Inventory `kernel/gui`, GUI syscalls, GPU syscalls, input producers, framebuffer/display paths and client libraries.
- Identify shared-memory, address-space, IPC and capability primitives already present.

Exit gate:
- Every GUI responsibility has an explicit destination in `libagfx`, `agfxd`, client libraries or kernel mechanisms.
- Existing rendering components are classified as retain, adapt or replace.

### G1 — VM and shared-surface prerequisites

#### G1a — Backing-object lifetime and fork inheritance

**Status: implementation integrated; x86_64 kernel build and targeted G1a tests pass as of 2026-11-28. Broader test-suite execution is currently affected by unrelated FS-stage failures during parallel filesystem development.**

Deliverables:
- Refcount anonymous SHMEM objects independently of physical frame references.
- Give each VMA a backing-object reference; retain/release it on clone, split, unmap and free-all.
- Keep the SHMEM object alive across an in-flight page fault.
- Revalidate a VMA before installing a page so a concurrent unmap does not resurrect it.
- Make fault-path page installation conditional, preventing duplicate concurrent installs from leaking PMM references.
- Preserve `shmid` when cloning VMAs in `fork()`.
- Duplicate SysV SHM attachment records during `fork()` and increment each segment's `nattch`.
- Make `MAP_POPULATE` use the VMA fault path rather than bypassing shared backends with direct PMM allocation.

Verification:
- `fork()` of a process with `MAP_SHARED|MAP_ANONYMOUS`: parent and child see the same bytes after writes in either direction.
- `munmap()` in one process does not invalidate the other's mapping.
- After both VMAs disappear, the SHMEM object and cached frame references are released.
- Concurrent first-touch of the same VMA page does not leak a frame or overwrite a mapping.
- `MAP_POPULATE` on anonymous shared and file-shared mappings uses the same backing as ordinary faults.
- SysV `shmat()` followed by `fork()`: both parent and child can access the segment; `IPC_RMID` remains pending until the final inherited attachment detaches.
- Targeted G1a tests must be evaluated independently from unrelated filesystem test failures.
- Filesystem-stage failures caused by concurrent FS development are tracked separately and must not be treated as G1a regressions without evidence of a dependency.

#### G1b — Address-space metadata ownership and VM edge cases

**Status: pending.**

Deliverables:
- Move VMA list and VMA lock ownership from TCB into a refcounted address-space metadata object shared by `CLONE_VM` threads.
- Ensure every sibling uses the same list head and same lock.
- Audit exec/exit/reap ordering for address-space metadata and VMA lifetime.
- Audit mixed-range `MAP_FIXED`, partial `munmap`, `mprotect`, `MADV_DONTNEED`, page-cache writeback and SysV SHM unmap rules.

Exit gate:
- `CLONE_VM` insert/remove/fault operations are race-safe across CPUs.
- One thread cannot mutate a private copy of a supposedly shared VMA-list head.
- VM regression suite passes under SMP and memory-pressure testing.

#### G1c — Kernel-managed shared surface object

**Status: pending.**

Deliverables:
- Define generic shared-memory object creation, mapping, rights and destruction independent of SysV SHM.
- Support mapping the same object into a client address space and the `agfxd` address space.
- Ensure mapping teardown, process exit and capability revocation are ordered safely.
- Add page-aligned surface layouts, stride/format metadata and synchronization primitives.

Exit gate:
- Two unprivileged processes can share a writable surface with no pixel-copy syscall.
- Destroying or revoking the object prevents new mappings while existing mappings are torn down safely.

### G2 — Kernel IPC and capability boundary

**Status: pending.**

Deliverables:
- Choose or extend a generic IPC transport; retain existing SysV IPC semantics separately.
- Define authenticated sender identity and capability transfer/validation.
- Add owner-bound window/surface capabilities with generation-safe handles.
- Define event/message size limits, queue backpressure, blocking and cancellation semantics.

Exit gate:
- A client cannot address another client's windows or surfaces by guessing IDs.
- Process exit revokes its capabilities and releases associated objects without leaking mappings.

### G3 — `agfxd` protocol and lifecycle

**Status: pending.**

Deliverables:
- Specify create/destroy/update/show/hide/focus/resize/commit operations.
- Specify input and window-event messages while preserving existing event numeric values where possible.
- Implement the server-side window registry and lifecycle in userspace.
- Keep the protocol independent of x86_64 syscall numbers.

Exit gate:
- A test client can create, update, resize and destroy a window through IPC only.
- Server restart/failure has a defined surface and client-notification policy.

### G4 — `libagfx` compositor and desktop migration

**Status: pending.**

Deliverables:
- Move WindowStore/Window Manager and compositor orchestration to `libagfx`.
- Reuse current Rust drawing, dirty-rect, MSDF and desktop code where the ownership model permits.
- Move wallpaper, cursor, taskbar and desktop policy to userspace.
- Add deterministic tests for z-order, clipping, damage tracking, focus and resize.

Exit gate:
- `agfxd` composes the desktop without invoking `kernel/gui` policy code.
- Existing visual features remain functional and measured performance is acceptable on target weak hardware.

### G5 — Input and display ownership

**Status: pending.**

Deliverables:
- Route keyboard, pointer and future HID events to `agfxd` through a bounded kernel event mechanism.
- Define display-session ownership and safe handoff/recovery.
- Replace privileged PID checks such as legacy GUI render/cursor ownership with capabilities.
- Keep GPU submission/resource validation in the kernel/device boundary, not in desktop policy.

Exit gate:
- Input is delivered to the correct session and focused window.
- Non-owner processes cannot change the active display or cursor state.

### G6 — Client library migration

**Status: pending.**

Deliverables:
- Keep RSBR-GUI's public Rust API while switching its internals to IPC/shared surfaces.
- Keep libauragui's public C API while switching its internals to the same protocol.
- Add compatibility coverage for existing application behavior.

Exit gate:
- Existing RSBR-GUI and libauragui applications work with `agfxd` and no kernel GUI syscalls.

### G7 — Remove kernel GUI

**Status: pending; last phase only.**

Deliverables:
- Remove old GUI syscalls and their dispatch arms after all users migrate.
- Delete `kernel/gui/` and its build integration.
- Remove obsolete C/Rust bridge functions and kernel-owned GUI state.
- Update architecture docs, syscall ABI docs, test harnesses and boot configuration.

Exit gate:
- Clean build contains no `kernel/gui` objects or unresolved GUI syscall paths.
- GUI smoke tests pass on x86_64, RV64 and AArch64 targets where display support exists.

## 5. Dependency graph

```text
G0 audit
  └── G1a backing lifetime + fork
        └── G1b address-space metadata + VM correctness
              └── G1c generic shared surfaces
                    ├── G2 IPC + capabilities
                    │     └── G3 agfxd protocol/lifecycle
                    │           ├── G4 compositor/desktop
                    │           ├── G5 input/display ownership
                    │           └── G6 client libraries
                    └───────────────────────────────┘
                                  G7 kernel/gui removal
```

G4 and G5 may be developed in parallel after the protocol and ownership contracts are stable. G6 can begin once the protocol's create/map/commit/event semantics are frozen.

## 6. Risks and deliberate non-goals

Risks:
- A stale VMA or SHMEM ID can turn a normal page fault into a use-after-free unless backing references are held across the entire fault operation.
- `CLONE_VM` currently lacks a single shared VMA-list owner/lock; fixing fork alone does not fix this.
- SysV SHM has its own attachment accounting and must not be conflated with anonymous SHMEM.
- Holding spinlocks across allocation, VFS I/O, PMM operations or other potentially blocking work can deadlock or cause excessive interrupt latency; new code should keep expensive work outside object/list locks.
- Shared-surface revocation and client exit must account for both server and client mappings.

Deliberate non-goals for G1a:
- Full `CLONE_VM`/address-space metadata redesign (G1b).
- New generic userspace IPC transport or GUI capability ABI (G2).
- Rewriting the current Rust renderer or desktop.
- Removing `kernel/gui/` before the userspace replacement passes the end-to-end gate.
- Claiming SMP correctness from a successful single-CPU boot alone.

## 7. Overall completion criteria

The migration is complete only when:
- Window Manager, compositor, desktop policy, focus, z-order and window lifecycle run in userspace.
- `agfxd` owns the display session through explicit capabilities.
- Clients render into shared surfaces without per-frame pixel copies through syscalls.
- Input and window events flow through the documented IPC protocol.
- RSBR-GUI and libauragui retain working client APIs.
- Kernel mechanisms are generic and contain no desktop/window policy.
- `kernel/gui/` and obsolete GUI syscalls are removed from the clean build.
- Cross-architecture and SMP regression gates pass.
