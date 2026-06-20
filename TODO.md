# NovOS TODO

Open work, ordered by phase. Checked items live in [PLAN.md](PLAN.md).

## Phase 1 (cleanup)
- [ ] Decide on `limine.conf` verbosity policy per environment (dev vs release).
- [ ] Add a unit-test shim for host-side testing of `string.c` / `kprintf`.
- [ ] Replace the embedded 8x8 font with a PSF2 8x16 font for sharper text
      (deferred to the GUI phase).

## Phase 2 — DONE ✅ (2026-06-20)

## Phase 3 — Physical Memory Manager
- [ ] Consume the Limine memmap (already requested) into a bitmap PMM.
- [ ] `pmm_alloc_frame` / `pmm_free_frame` / `pmm_alloc_contiguous`.
- [ ] Boot-time memory statistics.

## Phase 4 — Paging / VMM
- [ ] 4-level paging walker, `paging_map/unmap`, `invlpg`.
- [ ] Switch off Limine's initial identity mapping where appropriate.
- [ ] Enable NX (XD) for data pages; map `.rodata` read-only + no-exec.

## Phase 5–14
- Heap, timer/APIC, scheduler, Ring 3 + ELF loader, syscalls, VFS + initrd,
  init + shell, SMP, networking, GUI. See the master roadmap.

## Phase 2 follow-ups
- [ ] Add a TSS + IST for the double-fault handler (#DF) so a kernel stack
      overflow cannot escalate to a triple fault. Needed before userspace.
- [ ] Wire up the PIT timer (IRQ 0) as a periodic heartbeat once the scheduler
      lands (depends on Phase 6).
- [ ] Remove the divide-by-zero self-test from `kmain` once a scheduler exists.

## Known issues
- No IST/TSS: a kernel stack overflow would triple-fault. Tracked above.
- The kernel halts at the end of `kmain` by design (no scheduler yet).
