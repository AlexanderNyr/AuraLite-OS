# AuraLite OS — Systems Hardening Plan

**Status: H1 COMPLETE** (committed `H1: add gfx_flip_rect and dirty-rect compositor`, 2026-06-29)

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

## H2 — Address-Space Reaping Verification & Fix

**Status: VERIFIED — already implemented**

`paging_free_address_space()` is fully implemented and called from
`thread_reap_zombies()` (kernel/proc/thread.c:420). Boot log shows:
```
[thread] reaped '/hello' (tid 6, 35 frames)
[thread] reaped '/execve_child' (tid 7, 38 frames)
```

**`docs/status.md` needs update** — line 27 (`Thread/process reaping`) and
line 36 (`Copy-on-write`) are marked ❌ but both features are implemented.

### Required action

Update `docs/status.md`:
- Line 27: Thread/process reaping → ✅ (note: via `paging_free_address_space()` in `thread_reap_zombies()`)
- Line 36: Copy-on-write → ✅ (COW fork via `paging_clone_user_space()`, `paging_handle_cow_fault()`)

---

## H3 — Copy-on-Write fork()

**Status: VERIFIED — already implemented (contradicts status.md)**

`paging_clone_user_space()` performs mark-and-share COW fork (kernel/arch/x86_64/paging.c:210-271).
`paging_handle_cow_fault()` copies on first write. `status.md` ❌ entry is stale.

**Required action:** Update `docs/status.md` COW entry → ✅

---

## H4 — True Blocking I/O (Wait Queues)

**Status: TODO**

`wait_queue` does not exist. `futex_wait()` uses `THREAD_BLOCKED + sched_yield()`.
`select()`, pipe `read()` — yield-polling loops.

### New files required

- `kernel/proc/wait_queue.c` / `wait_queue.h`

### Tasks

- [ ] `struct wait_queue` + `wq_wait()`, `wq_wake_one()`, `wq_wake_all()`
- [ ] Pipe: replace `sched_yield()` poll with `wq_wait()` on `read_wq`/`write_wq`
- [ ] `futex_wait()`: remove `sched_yield()` loop
- [ ] `nanosleep()`: use `sleep_deadline` TCB field + PIT wake in `signal_tick()`
- [ ] `select()`: rewrite to use per-OFD `read_wq`/`write_wq`

---

## H5 — TCP Server (bind / listen / accept)

**Status: TODO**

No `tcp_listen/accept/bind`. No server-side socket API.

### New files / changes

- `kernel/net/tcp.c`: add `tcp_state_listen`, `tcp_listen()`, `tcp_accept()`
- `kernel/arch/x86_64/syscall.c`: dispatch SYS_BIND/SYS_LISTEN/SYS_ACCEPT
- `libc/src/libc.c`: `bind()`, `listen()`, `accept()` wrappers
- `userspace/tcpserver/tcpserver.c`: minimal HTTP echo server

---

## H6 — Slab Allocator

**Status: TODO**

`kmalloc(sizeof(tcb_t))` on every thread create. No slab cache.

### New files required

- `kernel/mm/slab.c` / `kernel/mm/slab.h`
- `tests/unit/test_slab.c`

### Tasks

- [ ] `slab_create()`, `slab_alloc()`, `slab_free()`
- [ ] Global caches: `tcb_cache`, `ofd_cache`, `vnode_cache`
- [ ] Replace `kmalloc(sizeof(tcb_t))` → `slab_alloc(tcb_cache)` in scheduler/thread
- [ ] Host unit test: 10000 alloc/free → zero OOM

---

## H7 — SA_RESTART & Signal Frame FPU State

**Status: TODO**

`#define SA_RESTART 0x10000000` exists but not acted upon. FXSAVE/FXRSTOR absent.

### Tasks

- [ ] `syscall_restart_num`, `syscall_restart_args[6]` in `tcb_t`
- [ ] `is_restartable()` helper
- [ ] `signal_deliver()`: save restart info on `-EINTR` + `SA_RESTART`
- [ ] `sigreturn`: re-dispatch if restart pending
- [ ] `fxsave_area[512]` in `signal_frame` (16-byte aligned)
- [ ] `FXSAVE` on signal delivery, `FXRSTOR` on sigreturn

---

## H8 — SMP-Safe Scheduler

**Status: TODO**

APs `hlt`-loop. No LAPIC/IOAPIC. Single global `current_thread` without spinlock.

### New files required

- `kernel/arch/x86_64/lapic.c` / `lapic.h`
- `kernel/arch/x86_64/cpu_local.h`

### Tasks

- [ ] `struct cpu_local` with `GS:0` self-pointer
- [ ] `cpu_local_init(cpu_id)` via `WRMSR 0xC0000101`
- [ ] `sched_lock` spinlock on run queue
- [ ] Replace `static current_thread` with `cpu_local()->current`
- [ ] `lapic_enable()`, `lapic_timer_start(hz)`, `lapic_eoi()`
- [ ] AP main: call `lapic_enable()` + `sched_idle()`

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