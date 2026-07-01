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

## Bug 13 — `schedule()`: TSS/CR3 updated only on CPU 0 (scheduler.c) ⛔ CRITICAL SMP BUG

**Files:** `kernel/proc/scheduler.c`, `kernel/arch/x86_64/tss.{c,h}`  
**Severity:** Wrong kernel stack / wrong address space on APs  

The scheduler updated TSS.RSP0, the SYSCALL kernel stack, and CR3 only when
`cpu_id == 0`. That is safe only while APs stay in the idle loop. As soon as an
AP runs a user thread, Ring 3→0 transitions would use the wrong kernel stack and
user execution would continue in the wrong address space.

**Fix:** Removed the `cpu_id == 0` guards from `schedule()` so every CPU updates
its own active stack and CR3. Added `tss_set_rsp0_for_cpu(int cpu_id, uint64_t)`
and completed the TSS side with per-CPU TSS state plus `tss_load_for_cpu()` so
AP bring-up loads a CPU-local TSS instead of sharing only the BSP image.

---

## Bug 14 — `handle_user_page_fault()`: MAP_SHARED TOCTOU on page-cache miss (vma.c / page_cache.c) ⛔ CRITICAL

**Files:** `kernel/mm/vma.c`, `kernel/mm/page_cache.{c,h}`  
**Severity:** Double frame allocation / leaked or inconsistent shared pages on SMP  

`handle_user_page_fault()` did a `page_cache_get()` miss check, then allocated a
new frame, filled it, and finally did `page_cache_put()`. Two CPUs faulting the
same shared page concurrently could both allocate different frames and race to
publish them.

**Fix:** Added `page_cache_get_or_alloc()`, which performs lookup + allocation +
insertion as one cache-locked operation and returns the canonical shared frame
with its PMM refcount bumped. `handle_user_page_fault()` now uses that helper.

---

## Bug 15 — `do_select()`: large fixed arrays on 16 KiB kernel stack (select.c) 🔴 CRASH

**File:** `kernel/fs/select.c`  
**Severity:** Kernel-stack overflow risk  

The blocking `select()` path allocated four `FD_SETSIZE`-sized arrays directly on
its kernel stack, consuming ~3 KiB before any deeper call chain or interrupt
nesting.

**Fix:** Moved the wait-queue arrays to heap allocations sized by `nfds`, added
OOM handling, and freed them on exit. Also reset `ready` before the post-wakeup
re-scan so the return value matches the actual ready-set count.

---

## Bug 16 — `vma_remove_range()`: split-allocation OOM corrupts VMA layout (vma.c) 🟡 DATA LOSS / DOUBLE-FREE RISK

**File:** `kernel/mm/vma.c`  
**Severity:** Lost VMA coverage after partial `munmap()` under allocation failure  

The code removed the original VMA from the list before allocating left/right
split nodes. If one split allocation failed, the address-space description could
lose surviving regions entirely.

**Fix:** Pre-allocate the required split nodes first. If any required allocation
fails, the original VMA is left untouched and removal of that segment is skipped.

---

## Bug 17 — `vma_list` accessed without locking (thread.h / process.c / vma.c / syscall.c) 🟡 RACE CONDITION

**Files:** `kernel/proc/thread.h`, `kernel/proc/thread.c`, `kernel/proc/process.c`, `kernel/mm/vma.c`, `kernel/arch/x86_64/syscall.c`  
**Severity:** Concurrent VMA traversal/modification can race on SMP  

`vma_list` was read and modified from page faults, fork, mmap/munmap, and
mprotect with no per-process lock.

**Fix:** Added `spinlock_t vma_lock` to `tcb_t`, initialised it in
`kthread_create()`, and used IRQ-save locking around VMA traversal/modification
sites. The page-fault handler snapshots the matched VMA under the lock and then
continues fault resolution without holding it.

---

## Bug 18 — `fork()`: COW applied to already-mapped `VMA_SHARED` pages (paging.c) 🟡 MEMORY CORRUPTION

**Files:** `kernel/arch/x86_64/paging.c`, `kernel/proc/process.c`  
**Severity:** Shared mappings lose MAP_SHARED semantics across fork  

`paging_clone_user_space()` marked every writable user mapping copy-on-write,
including pages that belong to `VMA_SHARED` mappings and should remain shared.

**Fix:** During user-PTE cloning, reconstruct the virtual address, look up the
parent VMA under `vma_lock`, and skip the COW transformation for `VMA_SHARED`
pages while still incrementing their PMM refcount.

---

## Bug 19 — `munmap()`: freed frames before removing VMA metadata (syscall.c) 🟡 DOUBLE-FREE RISK

**File:** `kernel/arch/x86_64/syscall.c`  
**Severity:** Freed pages could remain described by a stale VMA if split/removal failed  

`munmap()` first unmapped/freed physical pages and only then called
`vma_remove_range()`. Combined with the old split-allocation bug, that could
leave stale VMAs describing already-freed pages.

**Fix:** Reverse the order: remove VMA metadata first under `vma_lock`, then walk
and unmap/free the physical pages.

---

## Bug 20 — `page_cache_flush()` cleared `dirty` even on write failure (page_cache.c) 🟡 DATA LOSS RISK

**File:** `kernel/mm/page_cache.c`  
**Severity:** Dirty data could be lost forever after a short/error write  

The flush path ignored the vnode write return value and always cleared the page's
`dirty` flag.

**Fix:** Clear `dirty` only after a full 4096-byte write. On error/short write,
log the failure and keep the page dirty so a later flush can retry.

---

## Bug 21 — NXE enablement not verified before relying on NX faults (paging.c) ⚠️ HARDENING

**File:** `kernel/arch/x86_64/paging.c`  
**Severity:** Execution-disable protections depend on EFER.NXE  

The kernel was already setting EFER.NXE, but there was no visibility when it had
not yet been enabled on entry.

**Fix:** `paging_init()` now explicitly checks EFER.NXE and logs a warning before
forcing it on, making the NX dependency visible during boot diagnostics.

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

---

## Bug 22 — `page_cache_get_or_alloc()`: `kmalloc` inside spinlock (M1) 🔴 DEADLOCK RISK

**Files:** `kernel/mm/page_cache.c`, `tests/unit/test_page_cache.c`  
**Fix:** Reworked the page-cache miss path into a lockless pre-allocation + locked recheck/insert flow so both `kmalloc()` and `pmm_alloc_frame()` happen outside the cache spinlock. The same lock discipline was applied to adjacent page-cache insert/invalidate paths so no heap allocation/free now occurs while `cache_lock` is held.

## Bug 23 — `page_cache_get_or_alloc()`: page published before fill (M2) 🔴 DATA CORRUPTION

**Files:** `kernel/mm/page_cache.c`, `tests/unit/test_page_cache.c`  
**Fix:** Added a `ready` flag to cache entries. New pages are inserted as `ready=0`, filled outside the lock, then published with a release-store to `ready=1`. Readers wait until `ready` is observed before returning the shared frame.

## Bug 24 — `tss_load_for_cpu()`: race on global `gdt[]` under SMP (M3) 🔴 CORRUPTION

**Files:** `kernel/arch/x86_64/gdt.{c,h}`, `kernel/arch/x86_64/tss.c`, `tests/unit/test_gdt_tss.c`  
**Fix:** Added `gdt_set_tss_in()` to encode a TSS descriptor into an arbitrary GDT buffer. `tss_load_for_cpu()` now patches the per-CPU GDT directly instead of transiently mutating the global `gdt[]`.

## Bug 25 — `mprotect()`: no TLB invalidation after PTE change (M4) 🟡 SECURITY

**Files:** `kernel/arch/x86_64/syscall.c`, `kernel/arch/x86_64/mprotect.{c,h}`, `kernel/arch/x86_64/lapic.h`, `tests/unit/test_mprotect.c`  
**Fix:** Moved the page-table reprotection path into a dedicated helper that remaps every present page with the new flags, performs `invlpg` on the local CPU for each updated VA, and issues a TLB-shootdown IPI for other CPUs after the batch.

## Bug 26 — `mprotect()`: only handles single-VMA ranges (M5) 🟡 POSIX NON-COMPLIANCE

**Files:** `kernel/arch/x86_64/syscall.c`, `kernel/arch/x86_64/mprotect.{c,h}`, `tests/unit/test_mprotect.c`  
**Fix:** Replaced the single-`vma_find()` check with a full coverage walk across adjacent VMAs and updated every VMA intersecting the protected span before remapping present PTEs.

## Bug 27 — `ap_entry()`: `tss_load_for_cpu` called before `cpu_local_init` (M6) 🟡 NULL DEREF

**Files:** `kernel/arch/x86_64/smp.c`, `tests/integration/cases/test_smp_init_order.sh`, `tests/integration/cases/test_smp_tss.sh`  
**Fix:** Reordered AP bring-up so `cpu_local_init()` runs immediately after GDT/IDT load and before any per-CPU TSS/logging path that can touch GS-based CPU-local state.
