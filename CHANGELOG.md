# Changelog

All notable changes to NovOS. Dates are ISO 8601 (Europe/Moscow local).

## [Phase 3 — Physical Memory Manager] 2026-06-20

### Added
- Limine bridge: `limine_get_memmap()` exposes the full memory-map entry list.
- `kernel/lib/bitmap.h`: header-only, pure-C bitmap with single-bit ops,
  byte-granular `bm_first_free`, and a linear `bm_find_contiguous` run search.
- `kernel/lib/spinlock.{c,h}`: test-and-set (LOCK CMPXCHG) spinlock with a
  `pause`-yielding slow path and an irqsave acquire/restore variant.
- `kernel/mm/pmm.{c,h}`: bitmap physical memory manager.
  - Sizes the bitmap from the highest usable address.
  - Places it in bootloader-reclaimable memory (usable as fallback) and reaches
    it via the Limine HHDM — consuming zero usable RAM.
  - `pmm_alloc_frame` / `pmm_alloc_contiguous` / `pmm_free_frame`, serialised by
    an irqsave spinlock; double-free / bad-address detection.
  - `pmm_dump_stats` + `pmm_get_free_frames` / `pmm_get_usable_frames`.
  - In-kernel self-test: 1000 unique frames, no leak, contiguous alloc.
- `tests/unit/test_pmm.c` host unit test + `make test-unit` Makefile target.

### Changed
- Removed the Phase 2 deliberate divide-by-zero from the boot path (it halts and
  would block later phases); the IDT remains installed and active.
- CI gate now asserts PMM initialisation + self-test PASS (Phase 2 exception
  checks relaxed to structural IDT/PIC assertions).

## [Phase 2 — Interrupts & Exceptions] 2026-06-20

### Added
- 256-entry Interrupt Descriptor Table (`idt.c`/`idt.h`) with LIDT load.
- Macro-generated 256 ISR stubs (`isr_stubs.asm`): separate `ISR_NOERR` and
  `ISR_ERR` macros for the uniform stack frame, plus an `isr_table[]` address
  table that drives IDT population.
- Top-level dispatcher (`isr.c`/`isr.h`): exception classification, full
  GPR + RIP/CS/RFLAGS/RSP/SS register dump, a bounded frame-pointer stack
  trace, and CR2 reporting for page faults.
- 8259A PIC driver (`irq.c`/`irq.h`): remap IRQ 0-15 -> vectors 32-47, per-IRQ
  mask/unmask, End-Of-Interrupt, and a handler dispatch table
  (`irq_register_handler`).
- Divide-by-zero self-test in `kmain` demonstrating the gate criterion.
- `-fno-omit-frame-pointer` for meaningful stack traces.

### Fixed
- Makefile object-path collision: `isr.c` and `isr.asm` both compiled to
  `isr.o` and double-linked. Renamed the stubs to `isr_stubs.asm` and
  documented the base-name-uniqueness constraint for `.c`/`.asm` pairs.

## [Phase 1 — Hello Kernel] 2026-06-20

### Added
- Limine boot-protocol bridge (`kernel/limine_requests.{c,h}`) issuing
  framebuffer, memmap, HHDM, and base-revision requests (v12 marker protocol).
- 64-bit entry point `boot.asm`: own 64 KiB stack, defensive `.bss` zero, C call.
- Flat long-mode GDT (`gdt.c` + `gdt_flush.asm`) with a far-return CS reload.
- 16550 UART driver (COM1, 115200 baud) — the reliable early console.
- Linear framebuffer console (`fb.c`) with a public-domain 8x8 font.
- `kprintf` supporting `%s %d %u %x %X %c %p %%` plus width and zero-padding.
- Freestanding `string.c` (memset/memcpy/memmove/memcmp/strlen).
- `kernel.ld` higher-half linker script, page-separated by permission.
- `Makefile` (Clang/LLD/NASM) and ISO pipeline (`mkisoimage.sh`, `run_qemu.sh`).
- Headless debug tooling: `boot_debug.py`, `analyze_screen.py`, `read_screen.py`.

### Fixed
- Renamed `limine.cfg` → `limine.conf` (Limine v12 only searches `.conf`).
- Resolved Limine panic "PHDRs with different permissions sharing the same
  memory page" by page-aligning all segment boundaries and folding the Limine
  request structs into the writable `.data` segment.
- Corrected the data PHDR flags from `R E` to `R W`.
- `kprintf` now parses width and zero-padding (was printing `%016llx` verbatim).

## [Phase 0 — Bootstrap] 2026-06-20

### Added
- Vendored Limine 12.3.3 (binary release + matching `limine.h`).
- Toolchain bring-up: Clang 19 (`--target=x86_64-elf`), LLD, NASM, QEMU, xorriso.
- Initial bootable ISO that Limine loads into the higher half without faulting.
