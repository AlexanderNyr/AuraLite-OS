# Changelog

All notable changes to NovOS. Dates are ISO 8601 (Europe/Moscow local).

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
