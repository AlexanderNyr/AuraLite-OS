# AuraLite OS Documentation

This directory contains the technical documentation for AuraLite OS.

## Recommended reading order

1. [`build_and_run.md`](build_and_run.md) — how to build the ISO, run it in QEMU,
   VirtualBox or VMware, run unit/integration tests, and troubleshoot toolchain
   issues.
2. [`status.md`](status.md) — what is implemented, what is experimental, and
   what is known to be incomplete.
3. [`architecture.md`](architecture.md) — boot flow and core kernel design.
4. [`memory_map.md`](memory_map.md) — virtual/physical address layout.
5. [`syscall_abi.md`](syscall_abi.md) — user/kernel syscall ABI.
6. [`driver_guide.md`](driver_guide.md) — driver inventory and implementation
   notes.
7. [`virtual_machines.md`](virtual_machines.md) — VirtualBox and VMware setup.
8. [`virtual_driver_matrix.md`](virtual_driver_matrix.md) — virtual hardware compatibility matrix.
9. [`rust_application_guide.md`](rust_application_guide.md) — how to write, build, and troubleshoot freestanding Rust applications (`#![no_std]`), dynamic memory (`alloc`), FFI C-string conventions, and AuraGUI v2.0 integration.
10. [`gbrowser.md`](gbrowser.md) — the GUI browser (`/apps/gbrowser`, formerly `/apps/webview`): what it renders, its deliberate limitations, and the measured presentation budget.

11. [`win32.md`](win32.md) — the Win32 personality: what it supports and,
    more usefully, which behaviours are approximations (SEH is not
    table-driven unwinding; TLS is per-process).

## Other root-level docs

- [`../README.md`](../README.md) — project overview and quickstart.
- [`../PLAN.md`](../PLAN.md) — historical milestone plan.
- [`../TODO.md`](../TODO.md) — fine-grained known limitations, kept in full
  and annotated against the residue ledger (R12).
- [`../CHANGELOG.md`](../CHANGELOG.md) — chronological changes.
- [`../tls.md`](../tls.md) — the TLS 1.3 stack: capabilities, limitations,
  security properties.
- [`../WIN32_PLAN.md`](../WIN32_PLAN.md) — the Win32 personality plan, its
  legal grounding, and phase-by-phase status.

## Specialised references in this directory

- [`residue_ledger.md`](residue_ledger.md) — the machine-checked debt ledger
  (48 rows, checker-enforced arithmetic; the RESIDUE series' terminal state).
- [`metal_receipts.md`](metal_receipts.md) — the real-hardware receipt
  package (RESIDUE R11): nine paste-back slots, the WHPX PCID block.
- [`seams.md`](seams.md) — seam decision notes for the pinned driver
  couplings (the TIME seam landed @R6; the USB seam pin stands).
- [`usb.md`](usb.md) — the USB stack with its honest approximation table.
- [`filesystem.md`](filesystem.md) — filesystem layout and drivers.
- [`opengl.md`](opengl.md) — the software GL stack.
- [`trust_store.md`](trust_store.md) — shipped roots and lifecycle decision.
- [`posix2024_compliance.md`](posix2024_compliance.md) — the conformance
  matrix (partial rows allowlist measured EMPTY at R12).
- [`rsbr_app_doc.md`](rsbr_app_doc.md) — pure-Rust applications over the
  RSBR bridge (three ISAs since R8).
- [`bsod.md`](bsod.md) — STOP codes painted on the fatal blue screen.

## Documentation conventions

- **Stable** means the feature is built by default and has either boot-time or
  host-side tests.
- **Experimental** means code exists and may be useful, but the path is not yet
  complete or not regularly exercised on all targets.
- **WIP** means the file contains scaffolding/protocol code but the full data
  path is intentionally unfinished.
- **Full integration suite** means the QEMU black-box tests in
  `tests/integration/` currently pass in the documented toolchain environment; it
  does not imply production hardware support.

When updating code, update the matching status table and driver notes in this
folder at the same time.
