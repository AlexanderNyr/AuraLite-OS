# NovOS TODO

Open work, ordered by phase. Checked items live in [PLAN.md](PLAN.md).

## Phase 1 (cleanup)
- [ ] Decide on `limine.conf` verbosity policy per environment (dev vs release).
- [ ] Add a unit-test shim for host-side testing of `string.c` / `kprintf`.
- [ ] Replace the embedded 8x8 font with a PSF2 8x16 font for sharper text
      (deferred to the GUI phase).

## Phase 2 — DONE ✅ (2026-06-20)

## Phase 3 — DONE ✅ (2026-06-20)

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

## Phase 3 follow-ups
- [ ] ASSERT that the Limine memory regions are page-aligned (currently assumed
      — true for Limine, but a page-aligned bitmap base matters for correctness).
- [ ] Word-level (8-byte) bitmap scanning for faster `bm_first_free`.
- [ ] Track per-region provenance so `/proc/meminfo`-style reporting can break
      memory down by usable / ACPI / reserved.
- [ ] Add a PMM "used" region registry so the heap/VMM can claim named ranges.

## Known issues
- No IST/TSS: a kernel stack overflow would triple-fault. Tracked above.
- The kernel halts at the end of `kmain` by design (no scheduler yet).
