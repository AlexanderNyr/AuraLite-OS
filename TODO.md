# AuraLite OS TODO

Open work, ordered by phase. Checked items live in [PLAN.md](PLAN.md).

## Phase 1 (cleanup)
- [ ] Decide on `limine.conf` verbosity policy per environment (dev vs release).
- [ ] Add a unit-test shim for host-side testing of `string.c` / `kprintf`.
- [ ] Replace the embedded 8x8 font with a PSF2 8x16 font for sharper text
      (deferred to the GUI phase).

## Phase 2 — DONE ✅ (2026-06-20)

## Phase 3 — DONE ✅ (2026-06-20)

## Phase 4 — DONE ✅ (2026-06-21)

## Phase 5 — DONE ✅ (2026-06-21)

## Phase 6 — DONE ✅ (2026-06-21)

## Phase 7 — DONE ✅ (2026-06-21)

## Phase 8 — DONE ✅ (2026-06-21)

## Phase 8 follow-ups
- [ ] Implement the ELF64 loader (parse Ehdr/Phdr, map PT_LOAD segments,
      jump to e_entry) so arbitrary user binaries can run.
- [ ] Create per-process address spaces via `paging_new_address_space()`
      (kernel half shared; user half per-process).
- [ ] Add a proper PCB (Process Control Block) wrapping TCB + address space.
- [ ] Implement user-space `sbrk`/`brk` for heap growth.
- [ ] Copy-on-write fork.
- [ ] Add IST for #PF and #SS so nested faults are robust.

## Phase 7 follow-ups
- [ ] Reap dead threads: free their TCB + stack (currently leaked).
- [ ] Add BLOCKED state + wait queues (for sleep/IO/wait).
- [ ] Implement `kthread_join(tid)` to wait for thread completion.
- [ ] Migrate from round-robin to CFS (Completely Fair Scheduler).
- [ ] Add per-CPU run queues + work stealing for SMP (Phase 12).
- [ ] Use TSS IST for the scheduler/timer interrupt stack.

## Phase 6 follow-ups
- [ ] LAPIC timer calibration using the PIT as reference (needed for SMP Phase 12).
- [ ] Detect HPET for higher-resolution timing.
- [ ] Add `timer_get_uptime_ms` and sub-tick precision via TSC.

## Phase 2 follow-ups
- [ ] Add a TSS + IST for the double-fault handler (#DF) so a kernel stack
      overflow cannot escalate to a triple fault. Needed before userspace.
- [ ] Wire up the PIT timer (IRQ 0) as a periodic heartbeat once the scheduler
      lands (depends on Phase 6).

## Phase 3 follow-ups
- [ ] ASSERT that the Limine memory regions are page-aligned (currently assumed
      — true for Limine, but a page-aligned bitmap base matters for correctness).
- [ ] Word-level (8-byte) bitmap scanning for faster `bm_first_free`.
- [ ] Track per-region provenance so `/proc/meminfo`-style reporting can break
      memory down by usable / ACPI / reserved.
- [ ] Add a PMM "used" region registry so the heap/VMM can claim named ranges.

## Phase 4 follow-ups
- [ ] Remap `.rodata` read-only + no-exec (currently RW in the data segment by
      Limine's initial mapping).
- [ ] Add IST/TSS for #DF before the stack can overflow into page tables.
- [ ] Implement `paging_map_range` (multi-page convenience) and large-page
      (2 MiB / 1 GiB) support for performance-critical regions (HHDM, heap).
- [ ] Make the page-table walk host-testable by injecting alloc/HHDM callbacks.
- [ ] Reference-count shared page tables when `paging_new_address_space()` is
      used (kernel tables are shared via copy, not COW yet).

## Phase 5 follow-ups
- [ ] Slab layer for common fixed sizes (task structs, page tables, buffers).
- [ ] Guard pages around the heap region for overflow detection.
- [ ] Allocation tracking/quota per-subsystem; poison freed memory in debug.
- [ ] Make `krealloc` try in-place grow into a free neighbour before allocating.

## Known issues
- No IST/TSS: a kernel stack overflow would triple-fault. Tracked above.
- Dead threads' TCBs + stacks are leaked (no reaper yet). Tracked above.
