# AuraLite OS Development Plan

## Current Phase: 8 — Processes & User Mode

### Status: COMPLETE ✅ (2026-06-21)

### Objective

Run code in Ring 3 (userspace), with SYSCALL/SYSRET for kernel calls, and
recover gracefully from userspace faults. Gate criterion: a hardcoded user
binary runs in Ring 3; a privileged instruction (`cli`) causes a clean #GP that
the kernel recovers from by killing the user thread.

### Tasks

- [x] Expanded GDT: user code/data segments (DPL=3) + 64-bit TSS descriptor
- [x] `kernel/arch/x86_64/tss.{c,h}`: TSS with RSP0 (Ring 3→0 stack) + IST1 (#DF)
- [x] `gdt_set_tss()`: correctly encodes the 16-byte 64-bit TSS descriptor,
      including the upper 32 bits of the higher-half base
- [x] `kernel/arch/x86_64/syscall.{c,h}` + `syscall_entry.asm`:
      SYSCALL/SYSRET MSR config (STAR, LSTAR, SFMASK, EFER.SCE) + dispatch
- [x] `kernel/proc/user.{c,h}` + `user_entry.asm`: `iretq` to Ring 3,
      embedded user program (write + cli), gate test
- [x] Exception handler: detects Ring-3 origin (CS & 3), recovers by killing
      the faulting user thread instead of halting
- [x] SYS_WRITE (1) and SYS_EXIT (60) syscalls
- [x] Gate test: user writes "in user mode!" via syscall, then `cli` → #GP

### Design notes

- **User test runs as a kernel thread** (not directly from kmain) so that when
  #GP fires from Ring 3, the CPU lands on THIS thread's kernel stack (TSS.RSP0),
  and killing the thread cleanly removes it without disrupting kmain.
- **Ring 3 entry via `iretq`** (in pure assembly — inline asm is too fragile
  for the iret frame): push SS, RSP, RFLAGS, CS, RIP onto the kernel stack;
  `iretq` atomically drops to Ring 3.
- **TSS.RSP0** gives the CPU a known kernel stack when an interrupt/exception
  fires from Ring 3. Set per-thread before the iretq.
- **SYSCALL/SYSRET** via MSRs: STAR (segment bases), LSTAR (handler RIP),
  SFMASK (RFLAGS bits to clear), EFER.SCE (enable). Register convention is
  Linux-compatible (rax=sysno, rdi/rsi/rdx/r10/r8/r9=args).

### Bugs found and fixed

- **TSS descriptor #GP on LTR:** the 64-bit TSS descriptor is 16 bytes; the GDT
  needed 7 entries (6 + the upper half). Also `gdt_set_tss` was zeroing the
  upper base instead of writing bits 32-63 of the higher-half address.
- **LSTAR truncated to 32 bits:** `xor edx,edx` before WRMSR zeroed the high
  half of the syscall entry address. Fix: `mov rdx,rax; shr rdx,32`.
- **`sysretq` not a NASM mnemonic:** NASM uses `sysret` (→ `0F 07`); `sysretq`
  was silently treated as a label, emitting no instruction.
- **Syscall register remapping:** rewrote the entry to correctly insert the
  syscall number at the front of the C ABI argument list.
- **User program RIP-relative offset:** hand-computed the `lea rsi,[rip+N]`
  displacement to point at the embedded message string.

### Phases 0–7: COMPLETE ✅

### Definition of Done — Phase 8

- [x] User binary executes in Ring 3 (CS low bits = 3, verified)
- [x] SYSCALL from Ring 3 works (writes "in user mode!")
- [x] Privileged instruction (`cli`) in Ring 3 → #GP, detected as USER-mode
- [x] Kernel recovers by killing the user thread (does not halt)
- [x] kmain resumes and prints PASS

### Next Phase

**Phase 9 — System Calls**: a full POSIX-compatible syscall table (read, write,
open, close, mmap, brk, getpid, fork, execve, exit, wait4, pipe) with the libc
syscall wrappers, so userspace programs can do real I/O. Gate criterion:
`write(1, "hello\n", 6)` from userspace works.

### Tasks

- [x] `kernel/mm/heap.{c,h}`: generic, freestanding first-fit allocator
  - 32-byte header + 16-byte footer (boundary tag) per block
  - doubly-linked free list, O(1) coalescing both ways, splitting
  - `heap_alloc` / `heap_free` / `heap_realloc`
  - on-demand expansion via a callback (decouples allocator from paging)
- [x] `kernel/mm/kheap.{c,h}`: kernel wrapper
  - backs the allocator with PMM frames mapped on demand by the VMM
  - 16 MiB region at `0xFFFFFFFF88000000`, grows in 64 KiB chunks
  - `kmalloc` / `kfree` / `krealloc` / `kheap_dump`
- [x] In-kernel self-test: 10 000 alloc/free cycles + realloc round-trip
- [x] Host unit test `tests/unit/test_heap.c` (basic, alignment, coalescing,
      realloc, 10 000-cycle stress, leak check) + `make test-unit`
- [x] CI gate asserts the heap PASS line

### Design notes

- **Testability via decoupling:** the allocator core (`heap.c`) depends only on
  `<stdint.h>`; page-backed expansion is injected as a callback. So the *same*
  allocator object compiles into both the kernel and the host unit test.
- **Boundary tags:** every block carries a footer mirroring its size+magic, so
  freeing a block finds its previous neighbour in O(1) (read the footer just
  before its header) and coalesces both neighbours.
- **Magic-as-state:** used vs free is encoded in distinct magic values, keeping
  the header a clean 32 bytes / 16-aligned (payload always 16-aligned).
- **Lazy backing:** the heap region starts at 0 committed pages and maps frames
  from the PMM only as `heap_alloc` exhausts the free list. Verified: the test
  grew it to 1920 KiB then returned to `used 0 / free 1920 KiB` (no leak).
- **NX on heap pages:** heap frames are mapped No-Execute, so a wild jump into
  heap data triggers a clean #PF rather than executing attacker data.

### Phases 0–4: COMPLETE ✅

### Definition of Done — Phase 5

- [x] `kmalloc`/`kfree` survive 10 000 mixed-size cycles with no corruption
- [x] Freeing everything returns the whole region to one coalesced free block
- [x] `krealloc` preserves data on grow/shrink
- [x] Host unit tests pass; integration CI gate passes

### Next Phase

**Phase 6 — Timer & PIT/APIC**: configure the 8254 PIT (and later LAPIC timer)
for a periodic tick, a global monotonic counter, and `timer_sleep_ms`. Gate
criterion: a 1-second delay measured via the tick counter is accurate within ±5%.

### Tasks

- [x] `kernel/arch/x86_64/cpu.h`: consolidated CR/MSR/`invlpg` primitives
- [x] `kernel/arch/x86_64/paging.{c,h}`: VMM
  - reads PML4 from CR3; enables NX via EFER.NXE
  - `walk_pte()`: 4-level walker that creates intermediate tables from the PMM,
    reached through the HHDM (avoids table-to-map-table chicken-and-egg)
  - `paging_map` / `paging_unmap` / `paging_get_phys`
  - `paging_new_address_space()` (copies kernel half; untested until Phase 8)
- [x] Consolidated `read_cr2` from `isr.c` into `cpu.h`
- [x] In-kernel self-test: map → seed → read via virt → write via virt → verify
      via HHDM → unmap → confirm gone → deliberate #PF
- [x] CI gate asserts VMM PASS + CR2 match

### Design notes

- **Limine already enables paging.** The VMM does not build paging from scratch;
  it walks/extends Limine's PML4. Page-table frames are reached through the HHDM
  (`phys + 0xFFFF800000000000`), which Limine maps writable.
- **NX enabled:** `EFER.NXE` is set in `paging_init()` so `PAGE_FLAG_NO_EXEC`
  (bit 63) is honoured. Without NXE, setting bit 63 would cause a reserved-bit
  page fault.
- **Intermediate-table flags:** created entries get Present|Writable|User so the
  page is accessible from ring 0 now and ring 3 once the final PTE also carries
  User (preparing for Phase 8 userspace).
- **TLB discipline:** both `paging_map` and `paging_unmap` call `invlpg` to flush
  the specific TLB entry (essential for unmap; harmless for fresh maps).
- Page-table frames are tested in-kernel only (hardware-coupled; not host-testable
  without mocking the HHDM/PMM). The bitmap primitives used for allocation remain
  host-tested via `test_pmm.c`.

### Phases 0, 1, 2, 3: COMPLETE ✅

### Definition of Done — Phase 4

- [x] Map a page, read/write through both virtual address and HHDM (same data)
- [x] Unmap; translation reports "not present"
- [x] Access the unmapped address → #PF with CR2 = exact address, clean halt
- [x] NXE enabled; no reserved-bit faults

### Next Phase

**Phase 5 — Kernel Heap**: slab or first-fit `kmalloc`/`kfree`/`krealloc` over a
region of the kernel virtual address space, backed by the PMM + VMM. Gate
criterion: 10 000 alloc/free cycles with no corruption or leak.

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

- [x] QEMU shows "Hello from AuraLite OS kernel!" on serial (`-serial stdio`)
- [x] Same text rendered to the on-screen framebuffer
- [x] No triple fault; clean halt at end of `kmain`

### Next Phase

**Phase 2 — Interrupt & Exception Handling**: 256-entry IDT, macro-generated ISR
stubs, CPU exception dumpers (0–31), PIC remap + IRQ dispatch, and a `PANIC()`
that prints file/line/message and halts. Gate criterion: a divide-by-zero prints
a formatted exception dump instead of silently rebooting.
