# AuraLite OS — Systems Hardening Plan

**Status: ALL H1–H8 COMPLETE** (committed `H7: implement SA_RESTART syscall restarting and signal frame FPU state preservation`, 2026-06-29)

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