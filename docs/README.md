# AuraLite OS Documentation

This directory contains the technical documentation for AuraLite OS.

## Recommended reading order

1. [`build_and_run.md`](build_and_run.md) — how to build the ISO, run it in QEMU,
   VirtualBox or VMware, and troubleshoot toolchain issues.
2. [`status.md`](status.md) — what is implemented, what is experimental, and
   what is known to be incomplete.
3. [`architecture.md`](architecture.md) — boot flow and core kernel design.
4. [`memory_map.md`](memory_map.md) — virtual/physical address layout.
5. [`syscall_abi.md`](syscall_abi.md) — user/kernel syscall ABI.
6. [`driver_guide.md`](driver_guide.md) — driver inventory and implementation
   notes.
7. [`virtual_machines.md`](virtual_machines.md) — VirtualBox and VMware setup.
8. [`virtual_driver_matrix.md`](virtual_driver_matrix.md) — virtual hardware compatibility matrix.

## Other root-level docs

- [`../README.md`](../README.md) — project overview and quickstart.
- [`../PLAN.md`](../PLAN.md) — historical milestone plan.
- [`../TODO.md`](../TODO.md) — future work and known limitations.
- [`../CHANGELOG.md`](../CHANGELOG.md) — chronological changes.
- [`../PROJECT_STUDY.md`](../PROJECT_STUDY.md) — repository study notes created
  during review.

## Documentation conventions

- **Stable** means the feature is built by default and has either boot-time or
  host-side tests.
- **Experimental** means code exists and may be useful, but the path is not yet
  complete or not regularly exercised on all targets.
- **WIP** means the file contains scaffolding/protocol code but the full data
  path is intentionally unfinished.

When updating code, update the matching status table and driver notes in this
folder at the same time.
