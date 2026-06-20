# NovOS Development Plan

## Current Phase: 2 — Interrupt & Exception Handling

### Status: COMPLETE ✅ (2026-06-20)

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
