# AuraLite OS Development Plan

## Status: ALL 14 PHASES COMPLETE ✅

AuraLite OS is a from-scratch x86_64 operating system that boots from a Limine
BIOS ISO and runs an interactive shell in Ring 3. Every phase was built
incrementally, verified in QEMU, and documented before moving on.

---

## Phase Summary

| Phase | Name | Gate Criterion | Date |
|-------|------|----------------|------|
| 0 | Bootstrap | QEMU boots without triple fault | 2026-06-20 |
| 1 | Hello Kernel | "Hello" on serial + framebuffer | 2026-06-20 |
| 2 | Interrupts | Division-by-zero → formatted dump (no reboot) | 2026-06-20 |
| 3 | PMM | 1000 unique frames, no collision | 2026-06-20 |
| 4 | Paging | Map/unmap + clean #PF with CR2 | 2026-06-21 |
| 5 | Heap | 10 000 alloc/free cycles, no leak | 2026-06-21 |
| 6 | Timer | 1-second delay ±5% accurate | 2026-06-21 |
| 7 | Scheduler | Two threads interleave correctly | 2026-06-21 |
| 8 | User Mode | Ring 3 entry + #GP recovery | 2026-06-21 |
| 9 | Syscalls | Compiled ELF: `write(1,"hello\n",6)` | 2026-06-21 |
| 10 | VFS | `vfs_open("/init")` reads initrd | 2026-06-21 |
| 11 | Shell | Interactive shell; `ls /` lists files | 2026-06-21 |
| 12 | SMP | `-smp 4` → 4 CPUs online | 2026-06-21 |
| 13 | Networking | `ping 10.0.2.2` gets ICMP echo reply | 2026-06-21 |
| 14 | GUI | Double-buffered framebuffer graphics + keyboard | 2026-06-21 |

---

## Phase Details

### Phase 0 — Bootstrap

**Objective:** Cross-compilation environment + a minimal bootable binary.

- Vendored Limine 12.3.3 (binary release + matching `limine.h` from the
  v12.3.3 source tag).
- `kernel.ld`: higher-half linker script with page-separated text/rodata/data
  PHDRs. Limine refuses to map two differently-permissioned segments onto the
  same page, so every boundary is page-aligned. The Limine request structs live
  in the writable `.data` segment (Limine writes response pointers into them).
- Limine protocol requests: framebuffer, memmap, HHDM, base revision.
- `boot.asm`: 64 KiB stack, `.bss` zero, call `kmain`.

**Bug fixed:** Limine v12 searches for `limine.conf`, not `.cfg`.

### Phase 1 — Hello Kernel

**Objective:** Print a banner from 64-bit kernel code on serial and screen.

- GDT (flat 64-bit segments) loaded via `gdt_flush.asm` with a far-return CS reload.
- UART (COM1) driver @ 115200 baud.
- Linear framebuffer console with a public-domain 8×8 font.
- `kprintf` supporting `%s %d %u %x %X %c %p %%` with width + zero-padding.
- Freestanding `string.c` (memset/memcpy/memmove/memcmp/strlen).

**Bug fixed:** Data PHDR was accidentally flagged `R E` instead of `R W`.

### Phase 2 — Interrupts

**Objective:** CPU exceptions produce a readable dump, not a silent reset.

- 256-entry IDT + LIDT. Macro-generated 256 ISR stubs (`isr_stubs.asm`) with
  separate `ISR_NOERR`/`ISR_ERR` macros for a uniform stack frame.
- Full GPR + RIP/CS/RFLAGS/RSP/SS register dump + bounded frame-pointer stack
  trace + CR2 for page faults.
- 8259A PIC remap (IRQ 0–15 → vectors 32–47), mask/unmask, EOI, dispatch table.

### Phase 3 — Physical Memory Manager

**Objective:** Track all physical RAM with a bitmap over 4 KiB frames.

- `bitmap.h`: pure-C, header-only bit ops + first-free + contiguous-run search
  (host-testable).
- `spinlock.{c,h}`: LOCK CMPXCHG spinlock with irqsave variant.
- PMM sizes the bitmap from the highest usable address, carves it from
  bootloader-reclaimable RAM, and initialises from the Limine memmap.
- Verified: `free_frames == usable_frames` at boot (zero usable RAM consumed).

### Phase 4 — Paging

**Objective:** 4-level paging with map/unmap, NX, and per-process spaces.

- `cpu.h`: consolidated CR0/2/3/4, MSR read/write, `invlpg`.
- VMM reads PML4 from CR3, enables NX via EFER.NXE, walks/extends Limine's
  page tables via the HHDM (avoids the table-to-map-table chicken-and-egg).
- `paging_map` / `paging_unmap` (with `invlpg`) / `paging_get_phys`.
- `paging_new_address_space()` (copies kernel half for future processes).

### Phase 5 — Kernel Heap

**Objective:** `kmalloc`/`kfree`/`krealloc` backed by PMM + VMM.

- Generic freestanding allocator (`heap.c`) with first-fit free list,
  boundary-tag (header+footer) coalescing, and splitting. Expansion is injected
  as a callback → the *same object* is host-unit-tested.
- Kernel wrapper (`kheap.c`): 16 MiB region at `0xFFFFFFFF88000000`, maps PMM
  frames on demand, NX on heap pages.

### Phase 6 — Timer

**Objective:** Periodic timer interrupt + monotonic counter.

- 8254 PIT channel 0 in mode 3 (square wave) at ~100 Hz.
- IRQ 0 handler bumps a `volatile` tick counter; `timer_sleep_ms` spins with `hlt`.

### Phase 7 — Multitasking

**Objective:** Preemptive kernel threads.

- `context.asm`: `context_switch(old, new)` — saves/restores callee-saved
  registers + RSP + RFLAGS (the latter prevents IF leakage between threads).
- TCB with thread states; `kthread_create` crafts the initial stack frame so
  the first switch lands at the `thread_entry` trampoline.
- Round-robin scheduler (FIFO queue), `sched_yield`/`sched_tick`, idle fallback.
- PIC EOI sent *before* the handler so the timer can fire after a context switch.
- `kprintf` made atomic (print spinlock) for SMP-safe output.

### Phase 8 — User Mode

**Objective:** Run code in Ring 3.

- Expanded GDT: user code/data segments (DPL=3) + 16-byte TSS descriptor.
- TSS with RSP0 (Ring 3→0 stack) and IST1 (#DF handler stack).
- `iretq` to Ring 3 (pure assembly — inline asm is too fragile for the iret frame).
- SYSCALL/SYSRET via MSRs (STAR, LSTAR, SFMASK, EFER.SCE).
- Exception handler detects Ring-3 origin (CS & 3) and recovers by killing the
  faulting user thread.

**Bugs fixed:** TSS descriptor needed the upper 32 bits of the higher-half base.
LSTAR truncated to 32 bits (`xor edx,edx` zeroed high half). `sysretq` is not
a NASM mnemonic (use `sysret`). User data must be at GDT index 3 and user code
at index 4 so SYSRET's `SS=base+8, CS=base+16` formula produces DPL-3 selectors.

### Phase 9 — System Calls

**Objective:** A compiled C program runs in Ring 3 via an ELF loader.

- Minimal libc: crt0 (`_start`→`main`→`_exit`), generic syscall wrapper
  (`syscall.asm`), C wrappers (write/read/_exit/getpid).
- ELF64 loader: validates Ehdr, maps PT_LOAD segments with USER perms, skips
  already-mapped pages (co-located text/rodata), zero-fills `.bss`.
- Binary embedding: `gen_user_binary.py` converts the compiled ELF to a C array.
- `o64 sysret` (64-bit operand SYSRET) for correct CS loading.

### Phase 10 — VFS

**Objective:** Virtual file system with a USTAR initrd.

- VFS layer: mount table (longest-prefix matching), vnode abstraction, FD table.
- USTAR parser: 512-byte headers, octal sizes, `./` prefix stripping.
- DevFS: `/dev/null` (EOF on read, discards writes), `/dev/zero` (zero-filled reads).
- Limine module request to receive the initrd as a boot module.
- `tools/mkinitrd.sh` packs userspace binaries into a USTAR tarball.

### Phase 11 — Shell

**Objective:** Interactive shell with built-in commands.

- Expanded syscalls: SYS_OPEN, SYS_CLOSE, serial-input SYS_READ (fd=0 polls UART
  with sched_yield), SYS_LISTDIR.
- Expanded libc: printf, puts, strtok, strcmp, strncmp, strcpy, memset, memcpy.
- `init.c`: prompt loop, tokenizer, dispatch (ls, cat, echo, pwd, uname, free,
  help, exit).
- Two user ELFs: init.elf (shell, embedded) and hello.elf (in initrd).

### Phase 12 — SMP

**Objective:** Wake application processors.

- Limine MP request enumerates CPUs. Each AP gets a `goto_address` function.
- AP lifecycle: switch stack → load GDT/IDT → print online → idle (hlt).
- BSP filters out its own entry in the cpus[] array (Limine includes the BSP).
- Volatile + mfence for goto_address writes (Limine polls these fields).
- Global print spinlock for SMP-safe kprintf (cli/sti is per-CPU under SMP).

### Phase 13 — Networking

**Objective:** `ping 10.0.2.2` gets an ICMP echo reply.

- PCI config-space access (0xCF8/0xCFC), bus scan, BAR read, bus-master enable.
- e1000 NIC driver: MMIO register access via paging-mapped HHDM offset,
  legacy TX/RX descriptor rings (PMM-allocated for DMA), polling-based send/recv.
- Ethernet framing, ARP (with cache), IPv4 (RFC 1071 checksum), ICMP echo.
- RX polls the RDH MMIO register (descriptor status byte unreliable through HHDM).

### Phase 14 — GUI

**Objective:** Double-buffered framebuffer graphics + keyboard input.

- 2D graphics library: pixel plotting, filled/outlined rectangles, Bresenham
  line, bitmap-font text, back-buffer flip (avoids tearing).
- PS/2 keyboard driver: IRQ 1, scan-code set 1, ring buffer, ASCII translation.
- Boot screen demo: title bar, coloured rectangles, diagonal line, info text.

---

## Key Bugs Solved Across Phases

| Bug | Phase | Fix |
|-----|-------|-----|
| Limine searches `.conf` not `.cfg` | 0 | Renamed config |
| Data PHDR flagged R+E not R+W | 1 | Fixed flag bits |
| `.c`/`.asm` object-path collision | 2 | Renamed stubs (`isr_stubs.asm`) |
| kmain never resumed after thread exit | 7 | Set state=READY before schedule() |
| TSS descriptor upper base zeroed | 8 | Write bits 32–63 of higher-half addr |
| LSTAR truncated to 32 bits | 8 | `mov rdx,rax; shr rdx,32` |
| `sysretq` not a NASM mnemonic | 8 | Use `sysret` / `o64 sysret` |
| SYSRET SS DPL mismatch | 11 | Swap user code/data, STAR[63:48]=0x10 |
| IF leakage in context_switch | 11 | pushfq/popfq in context.asm |
| ELF co-located segments overwrite | 9 | Skip already-mapped pages |
| e1000 MMIO beyond HHDM range | 13 | Map 128 KiB via paging |
| RX descriptor status not visible | 13 | Poll RDH MMIO register |

---

## Testing Discipline

Every phase was verified at three levels:

1. **Host unit tests** (`make test-unit`): PMM bitmap algorithms and heap
   allocator compiled with the host compiler and stress-tested.
2. **In-kernel self-tests**: each subsystem runs a self-test at boot
   (PMM 1000 frames, VMM map/unmap, heap 10 000 cycles, timer accuracy,
   scheduler interleaving, VFS open/read, networking ping).
3. **CI integration gate** (`scripts/ci_test.sh`): boots the ISO in QEMU with
   `-smp 4` and e1000 networking, sends shell commands, and asserts every
   phase's PASS line appears on the serial console.
