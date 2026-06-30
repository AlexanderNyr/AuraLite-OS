# AuraLite-OS — Bug Report & Fixes

## Summary

Found and fixed **12 bugs** across 7 source files. Issues range from
compilation-blocking errors to race conditions, memory leaks, POSIX
non-compliance, and data corruption.

---

## Bug 1 — `do_execve()`: redeclaration of `cur` (process.c) ⛔ CRITICAL

**File:** `kernel/proc/process.c`  
**Severity:** Compilation error / undefined behaviour  

`do_execve()` uses `cur` (via `sched_current()`) at step 2 to free the VMA
list, but `cur` is only declared at step 3 — a later scope. The variable
shadow/redeclaration means the step-2 reference either fails to compile or
(with implicit declaration) uses an uninitialised pointer.

**Fix:** Wrapped the step-2 VMA cleanup in its own block with a separate
local `cur_vma`, and moved the step-3 `tcb_t *cur` declaration to be the
single one in scope afterward.

---

## Bug 2 — `sched_steal_work()` declared `static` but used externally (scheduler_rq.c) ⛔ CRITICAL

**File:** `kernel/proc/scheduler_rq.c`  
**Severity:** Linker error  

`sched_steal_work()` was `static` in `scheduler_rq.c` but referenced via
`extern tcb_t *sched_steal_work(void)` in `scheduler.c`. The linker
cannot resolve the symbol.

**Fix:** Removed `static` from `sched_steal_work()`.

---

## Bug 3 — `sched_lock` used but never declared (scheduler.c) ⛔ CRITICAL

**File:** `kernel/proc/scheduler.c`  
**Severity:** Compilation error  

`sched_init()` calls `spinlock_init(&sched_lock)`, but `sched_lock` is never
declared in any header or source file.

**Fix:** Added `spinlock_t sched_lock = SPINLOCK_UNLOCKED;` at file scope.

---

## Bug 4 — `idle_loop` referenced but never defined (scheduler.c) ⛔ CRITICAL

**File:** `kernel/proc/scheduler.c`  
**Severity:** Linker error  

`setup_stack(idle, idle_loop, NULL)` is called in both `sched_init()` and
`sched_idle()`, but `idle_loop` has no definition anywhere in the project.

**Fix:** Added a minimal `idle_loop()` function that enables interrupts and
halts (`sti; hlt` loop), the standard idle pattern for x86.

---

## Bug 5 — `page_cache.c` missing `#include "kheap.h"` ⛔ CRITICAL

**File:** `kernel/mm/page_cache.c`  
**Severity:** Compilation error (implicit declaration of `kmalloc`/`kfree`)  

`page_cache.c` calls `kmalloc()` and `kfree()` but never includes `kheap.h`.

**Fix:** Added `#include "kernel/mm/kheap.h"`.

---

## Bug 6 — `page_cache_flush()` writes to fd 0 with a physical address (page_cache.c) 🔴 DATA CORRUPTION

**File:** `kernel/mm/page_cache.c`  
**Severity:** Data corruption / crash  

The original code:
```c
vfs_pwrite(0, (void*)(uintptr_t)(curr->phys), 4096, curr->offset);
```
- Uses **fd 0 (stdin)** instead of the actual file descriptor.
- Passes a **raw physical address** as a buffer pointer — this is not a valid
  virtual address and will crash or corrupt memory.

**Fix:** Replaced with a direct call through the OFD's vnode write op,
converting the physical address to a HHDM virtual pointer. Also added
`#include "kernel/limine_requests.h"` for `limine_get_hhdm_offset()`.

---

## Bug 7 — Duplicate `#include` directives (syscall.c) ⚠️ CODE QUALITY

**File:** `kernel/arch/x86_64/syscall.c`  
**Severity:** Unnecessary code / potential confusion  

`paging.h`, `pmm.h`, `kheap.h`, and `limine_requests.h` were each
`#include`d twice — once at the top and once partway through the file after a
stale comment. Header guards prevent actual breakage, but it obscures intent.

**Fix:** Removed the duplicate block.

---

## Bug 8 — `close_process_fds()` skips fds 0-2 (thread.c) 🟡 RESOURCE LEAK

**File:** `kernel/proc/thread.c`  
**Severity:** FD / OFD resource leak on process exit  

`close_process_fds()` starts its loop at `fd = 3`, leaking any OFDs held in
fds 0, 1, and 2 (stdin/stdout/stderr) when a user process exits.

**Fix:** Changed the loop to start at `fd = 0`.

---

## Bug 9 — `load_and_jump_args()` dereferences `cur` without NULL check (process.c) 🔴 NULL DEREF

**File:** `kernel/proc/process.c`  
**Severity:** Kernel NULL-pointer dereference / crash  

`load_and_jump_args()` calls `sched_current()` into `cur`, then immediately
dereferences `cur->brk` without checking for NULL. If the scheduler is not
ready, this crashes the kernel.

**Fix:** Added a NULL check with an error message and clean exit path.

---

## Bug 10 — `do_select()` returns original fd_sets instead of result (select.c) 🟡 POSIX NON-COMPLIANCE

**File:** `kernel/fs/select.c`  
**Severity:** Incorrect `select()` semantics  

POSIX `select()` must return fd_sets containing **only the ready** descriptors.
The original code copied the unmodified input sets back to userspace, meaning
every originally-set fd appeared "ready" regardless of actual state.

**Fix:** Introduced separate `r_out`/`w_out` result sets that are populated
only with actually-ready fds, and those are what's written back to userspace.

---

## Bug 11 — `do_execve()` uses `static struct exec_args` (process.c) 🔴 RACE CONDITION

**File:** `kernel/proc/process.c`  
**Severity:** Data corruption under concurrent execve  

`do_execve()` declared:
```c
static struct exec_args ea;
```
Because the struct is `static`, any concurrent `execve()` call (even from a
different process on a different CPU) would overwrite the same memory, corrupting
argv/envp for both callers.

**Fix:** Changed to a `kmalloc`'d allocation per call, with matching `kfree()`
on all exit paths (error and success).

---

## Bug 12 — Memory leak: `exec_args` struct not freed after use (process.c) 🟡 MEMORY LEAK

**File:** `kernel/proc/process.c`  
**Severity:** Kernel heap memory leak per execve  

After changing to heap-allocated `exec_args`, the `exec_args_free()` helper
only frees the individual string allocations inside the struct, but not the
struct itself. Every `execve` would leak `sizeof(struct exec_args)` bytes.

**Fix:** Added `kfree(ea)` after every `exec_args_free(ea)` call on all code
paths (error cleanup and successful handoff in `load_and_jump_args`).

---

## Files Modified

| File | Bugs Fixed |
|------|-----------|
| `kernel/proc/process.c` | #1, #9, #11, #12 |
| `kernel/proc/scheduler.c` | #3, #4 |
| `kernel/proc/scheduler_rq.c` | #2 |
| `kernel/proc/thread.c` | #8 |
| `kernel/mm/page_cache.c` | #5, #6 |
| `kernel/arch/x86_64/syscall.c` | #7 |
| `kernel/fs/select.c` | #10 |
