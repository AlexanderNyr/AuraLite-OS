# NovOS Development Plan

## Current Phase: 3 — Physical Memory Manager

### Status: COMPLETE ✅ (2026-06-20)

### Objective

Track all physical RAM with a bitmap over 4 KiB frames and provide a
single-frame / contiguous allocator plus boot-time statistics. Gate criterion:
allocate 1000 unique non-overlapping frames (verified both by a host unit test
and an in-kernel self-test in QEMU).

### Tasks

- [x] Extend the Limine bridge with `limine_get_memmap()` (full entry list)
- [x] `kernel/lib/bitmap.h`: pure-C bit ops + first-free + contiguous-run search
      (header-only, host-testable)
- [x] `kernel/lib/spinlock.{c,h}`: LOCK CMPXCHG spinlock with irqsave variant
- [x] `kernel/mm/pmm.{c,h}`: bitmap PMM
      - sizes the bitmap from the highest usable address
      - carves the bitmap out of bootloader-reclaimable RAM (preserving usable)
      - initialises from the Limine memmap; tracks free/usable frames
      - `pmm_alloc_frame` / `pmm_alloc_contiguous` / `pmm_free_frame`
      - `pmm_dump_stats` + live counters
- [x] In-kernel self-test: 1000 unique frames, no leak, contiguous alloc OK
- [x] Host unit test `tests/unit/test_pmm.c` + `make test-unit`
- [x] CI gate extended to assert the PMM PASS line

### Design notes

- **Bitmap storage via HHDM:** the bitmap size is data-dependent, so it cannot
  live in `.bss`. It is carved from memory and reached through Limine's
  higher-half direct map (`0xFFFF800000000000 + phys`).
- **Bootloader-reclaimable preference:** the bitmap is placed in BLR memory
  first, falling back to usable. Verified by `free_frames == usable_frames` at
  boot — i.e. zero usable RAM consumed by the bitmap.
- **OOM sentinel:** physical address 0 is never handed out (frame 0 / IVT is
  reserved), so a return of 0 unambiguously means out-of-memory.
- **Locking:** allocations serialised by an irqsave spinlock (defensive; only
  `kmain` calls the PMM today).
- Removed the Phase 2 deliberate divide-by-zero from the boot path (it halts);
  the IDT stays installed and active.

### Phases 0, 1, 2: COMPLETE ✅

### Definition of Done — Phase 3

- [x] `pmm_alloc_frame()` returns 1000 unique non-overlapping page-aligned addrs
- [x] `pmm_free_frame()` returns frames with no leak (free count restored)
- [x] Boot statistics printed (tracked/usable/free frames + MiB)
- [x] Host unit test passes; integration CI gate passes

### Next Phase

**Phase 4 — Virtual Memory & Paging**: 4-level paging walker, `paging_map/unmap`
with `invlpg`, per-process address spaces, and NX for data pages. Builds on the
PMM (allocating page tables) and the HHDM (which Limine already provides).

### Objective

Give the kernel a working Interrupt Descriptor Table so that CPU exceptions
produce a readable register dump instead of a silent reset, and lay the PIC
groundwork for hardware interrupts. Gate criterion: a divide-by-zero prints a
formatted exception dump and halts — not a reboot.

### Tasks

- [x] `idt.c`/`idt.h`: 256-entry IDT descriptor table + LIDT load
- [x] `isr_stubs.asm`: macro-generated 256 ISR stubs + uniform stack frame,
      plus an `isr_table[]` of handler addresses used to populate the IDT
- [x] Error-code vectors (8,10,11,12,13,14,17) use a separate `ISR_ERR` macro
- [x] `isr.c`/`isr.h`: top-level dispatcher, full register dump + stack trace,
      CR2 reporting for page faults
- [x] `irq.c`/`irq.h`: 8259A PIC remap (IRQ 0-15 -> vectors 32-47), mask/unmask,
      EOI, per-IRQ handler dispatch table
- [x] Verified: divide-by-zero (no-error-code path) → formatted dump, halt
- [x] Verified: page fault (error-code path) → formatted dump + correct CR2, halt
- [x] CI gate extended to assert the exception dump

### Design notes

- In 64-bit mode the CPU always pushes `SS, RSP, RFLAGS, CS, RIP` (+ optional
  error code), so `registers_t` is uniform regardless of privilege level.
- Stack alignment for the C call: CPU frame (6 qwords) + stub header (2) +
  15 GPRs = 22 qwords ≡ 0 mod 16 at `call`, satisfying the System V ABI.
- Stack trace is bounded + range-checked (kernel higher half only) so a bad
  pointer cannot fault inside the exception handler (which would triple-fault).
- No IST/TSS yet: a stack overflow would escalate to a triple fault. IST
  support arrives with the TSS in the process/scheduler phase.

### Phase 1 status: COMPLETE ✅ (2026-06-20)

### Phase 0 status: COMPLETE ✅

### Definition of Done — Phase 2

- [x] QEMU boots, divides by zero, and shows a full `[EXCEPTION]` register dump
- [x] No reboot / triple fault (QEMU keeps running until the test timeout)
- [x] Error-code path independently verified via a page-fault test (CR2 correct)

### Next Phase

**Phase 3 — Physical Memory Manager**: consume the Limine memory map (already
requested) into a bitmap allocator over 4 KiB frames, with `pmm_alloc_frame` /
`pmm_free_frame` / `pmm_alloc_contiguous` and boot-time memory statistics. Gate
criterion: allocate 1000 unique frames with no collision.

### Objective

Produce a bootable x86_64 kernel image that Limine loads into the higher half,
which then initialises the serial UART, the framebuffer console, and its own
GDT, and prints a banner on **both** the serial console and the screen.

### Tasks

- [x] Toolchain: Clang `--target=x86_64-elf` + LLD + NASM installed
- [x] Vendor Limine 12.3.3 binary release + matching `limine.h`
- [x] `kernel.ld`: higher-half, page-separated text/rodata/data PHDRs
- [x] Limine protocol requests (framebuffer, memmap, HHDM, base revision)
- [x] `boot.asm`: stack setup, `.bss` zero, call `kmain`
- [x] GDT load (`gdt.c` + `gdt_flush.asm`) with CS reload via far return
- [x] UART (COM1) driver @ 115200 baud
- [x] Linear framebuffer console + public-domain 8x8 font
- [x] `kprintf` (`%s %d %u %x %X %c %p %%`, width + zero-pad, 64-bit)
- [x] Freestanding `string.c` (memset/memcpy/memmove/memcmp/strlen)
- [x] Bootable hybrid BIOS/UEFI ISO via xorriso + `limine bios-install`
- [x] QEMU verification: boots, no triple fault, banner on serial **and** screen

### Phase 0 status: COMPLETE ✅

Cross-compilation environment + minimal bootable binary that does not triple
fault. Achieved together with Phase 1.

### Risks encountered and resolved

| Risk | Resolution |
|------|------------|
| Limine searches only for `limine.conf`, not `.cfg` | Renamed config file |
| Two differently-permissioned PHDRs sharing a page → Limine panic | Page-align every segment boundary; keep requests in writable `.data` |
| Data segment accidentally flagged `R E` instead of `R W` | Fixed PHDR flags (`PF_W|PF_R`) |
| `limine.h` version must match the bootloader binary exactly | Pulled header from the v12.3.3 source tag |
| No display in sandbox → cannot view framebuffer | Built a PNG decoder + ASCII-art reconstructor to read the screen |

### Definition of Done — Phase 1

- [x] QEMU shows "Hello from NovOS kernel!" on serial (`-serial stdio`)
- [x] Same text rendered to the on-screen framebuffer
- [x] No triple fault; clean halt at end of `kmain`

### Next Phase

**Phase 2 — Interrupt & Exception Handling**: 256-entry IDT, macro-generated ISR
stubs, CPU exception dumpers (0–31), PIC remap + IRQ dispatch, and a `PANIC()`
that prints file/line/message and halts. Gate criterion: a divide-by-zero prints
a formatted exception dump instead of silently rebooting.
