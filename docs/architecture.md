# AuraLite OS Architecture

## Boot flow

```
SeaBIOS (QEMU) → Limine stage1/stage2 (BIOS)
   │  VBE: set 1280x800x32 framebuffer @ 0xfd000000
   │  Parse /boot/limine/limine.conf
   │  Load boot():/boot/kernel.elf into higher half
   │  Map PT_LOAD segments by permission; set up HHDM + memmap
   │  Scan marker-delimited request list; fill .response pointers
   │  Jump to ELF entry (_start) in 64-bit long mode, ring 0
   ▼
_start (boot.asm)
   │  cli; rsp := &stack_top (64 KiB); zero .bss
   ▼
kmain (kernel.c)
   ├── uart_init()        COM1 @ 115200 baud
   ├── fb_init()          console on the Limine framebuffer (8x8 font)
   ├── gdt_init()         flat GDT + segment reload (far return)
   ├── idt_init()         256-entry IDT + LIDT
   ├── pic_init()         8259A remap (IRQ 0-15 -> 32-47), all masked
   ├── sti                enable maskable interrupts
   ├── kprintf(...)       banner + diagnostics -> UART + framebuffer
   ├── pmm_init()         build bitmap from Limine memmap (via HHDM)
   ├── pmm_self_test()    alloc 1000 frames, free, verify no leak
   ├── paging_init()      read PML4 from CR3; enable EFER.NXE
   ├── paging_self_test() map/unmap test (safe; no deliberate fault)
   ├── kheap_init() + kheap_self_test()  on-demand heap; 10k cycles
   ├── pit_init(100) + timer_self_test()  100 Hz tick; 1s accuracy check
   ├── smp_init()          wake APs via Limine MP (4 CPUs online)
   ├── sched_init() + scheduler_self_test()  round-robin; 2 interleaving threads
   └── user_mode_self_test()  ELF loader + compiled hello in Ring 3

   # Phase 10:
   ├── vfs_init()           mount table + FD table
   ├── initrd_init()        parse USTAR module -> mount at "/"
   ├── devfs_init()         /dev/null, /dev/zero -> mount at "/dev"
   ├── vfs_self_test()      exercise /dev + /init
   ├── gfx_init() + keyboard_init()  2D graphics + PS/2 keyboard
   ├── gfx demo            draw boot screen (rectangles, lines, text)
   └── user_mode_self_test()  load init.elf (shell) -> Ring 3
      kmain then yields forever, letting the shell run interactively
```

## Interrupt handling (Phase 2)

```
CPU raises exception/IRQ
   │  pushes SS,RSP,RFLAGS,CS,RIP (+ error code where applicable)
   ▼
isrNN (isr_stubs.asm)        vector stub chosen by the IDT gate
   │  push dummy error code (if NOERR) ; push vector number
   │  jmp isr_common_stub
   ▼
isr_common_stub
   │  push rax..r15 (15 GPRs)        -> uniform registers_t on the stack
   │  mov rdi, rsp ; cld ; call isr_handler
   ▼
isr_handler (isr.c)
   ├── vector < 32  : exception -> dump registers + stack trace + CR2 (if #PF)
   │                              -> kernel_halt()
   └── vector 32-47 : IRQ -> irq_dispatch(irq) -> registered handler + EOI
```

The 256 vector stubs and their addresses are macro-generated; `isr_table[]`
(an address array in `.rodata`) is consumed by `idt_init()` to fill every gate.
Vectors that push an error code (8, 10-14, 17) use `ISR_ERR`; the rest push a
dummy zero so `registers_t` is always the same shape.

## Limine protocol bridge

`kernel/limine_requests.c` emits, in strict order inside the writable `.data`
segment:

1. `LIMINE_REQUESTS_START_MARKER` (4 qwords)
2. base revision (revision 3)
3. framebuffer request
4. memmap request
5. HHDM request
6. `LIMINE_REQUESTS_END_MARKER` (2 qwords)

At load time Limine walks the loaded image in 8-byte strides between the
markers; every aligned qword pair beginning with `LIMINE_COMMON_MAGIC` is a
request, into whose `.response` field Limine writes a pointer to the resolved
data. `volatile` + `__attribute__((used))` keep these objects alive; `KEEP()`
in `kernel.ld` prevents stripping.

## Consoles

`kputchar()` is the single fan-out point: it writes to **both** the UART and the
framebuffer, so `kprintf` output appears identically on serial and on screen.
The framebuffer path becomes a no-op if no 32-bpp RGB framebuffer was provided.

## GDT

A flat table is built in C and loaded by `gdt_flush.asm`, which reloads every
segment register and switches `CS` via a far return. Phase 8 expanded it to
7 entries: null, kernel code/data (Ring 0), user code/data (Ring 3, DPL=3), and
a 16-byte 64-bit TSS descriptor (2 slots). The TSS provides RSP0 (the kernel
stack loaded on Ring 3→0 transitions) and IST1 (a dedicated #DF stack).

## Why these choices

- **Limine** over GRUB/multiboot2: long-mode + higher-half + HHDM + framebuffer
  handed to us directly, minimising hand-written boot assembly.
- **Framebuffer** over VGA text mode: Limine programs a VBE graphics mode, so
  the `0xB8000` text buffer is no longer scanned for display.
- **Clang `--target=x86_64-elf`** over a hand-built cross-GCC: same-arch host,
  correct freestanding output, no lengthy binutils+gcc build.

## Physical memory management (Phase 3)

```
Limine memmap  ──►  pmm_init()
                     │  highest usable addr -> bitmap size
                     │  carve bitmap from bootloader-reclaimable RAM
                     │  reach it via HHDM (0xFFFF800000000000 + phys)
                     │  memset 0xFF (all used); clear USABLE regions
                     ▼
                 bitmap: bit SET = used, bit CLEAR = free
                     │
   pmm_alloc_frame()         first clear bit -> phys addr (0 = OOM)
   pmm_alloc_contiguous(n)   first run of n clear bits -> base phys
   pmm_free_frame(phys)      clear bit, double-free guarded
                     all serialised by an irqsave spinlock
```

The allocation algorithms (first-free bit, contiguous-run search) live in the
pure-C, kernel-independent `kernel/lib/bitmap.h`, so the *same code* is unit
tested on the host (`tests/unit/test_pmm.c`) and used in the kernel.

## Virtual memory (Phase 4)

```
paging_init()
   │  read CR3 -> PML4 (set up by Limine)
   │  set EFER.NXE (enable No-Execute bit in PTEs)
   ▼
paging_map(virt, phys, flags)
   │  walk_pte(virt, create=1)
   │     PML4 -> PDPT -> PD -> PT   (allocate + zero missing tables via PMM)
   │     tables reached via HHDM (phys + 0xFFFF800000000000)
   │  *PTE = phys | flags
   │  invlpg(virt)
   ▼
paging_unmap(virt)
   │  walk_pte(virt, create=0) -> *PTE = 0 ; invlpg(virt)
```

Key design point: the VMM does **not** build paging from scratch — Limine
already has long-mode paging enabled with the kernel mapped higher-half. The
VMM extends Limine's page tables, reaching newly-allocated table frames through
the HHDM. This avoids the classic chicken-and-egg of "map a table to manage
tables."

## Kernel heap (Phase 5)

```
heap_alloc(size)
   │  first-fit search of the free list
   │  if a free block >= need: split (optional), mark used, remove from list
   │  else: expand() -> map PMM frames into the heap region (VMM, NX set)
   │        -> add the new span as one free block -> retry search
   ▼
heap_free(ptr)
   │  mark free; coalesce NEXT neighbour (in range, via its header)
   │  coalesce PREVIOUS neighbour (via the boundary-tag footer before it)
   │  insert into the free list (unless absorbed into the previous block)
```

The allocator core (`heap.c`) is deliberately freestanding — it depends only on
`<stdint.h>`, with page-backed expansion injected as a callback. So the *same*
object file is linked into both the kernel (wrapped by `kheap.c`) and the host
unit test (`tests/unit/test_heap.c`), which pre-commits a buffer instead of
mapping pages. Every block carries a 32-byte header + 16-byte footer (boundary
tag); a distinct magic encodes used vs free, keeping payloads 16-aligned.

## Multitasking (Phase 7)

```
sched_tick (timer IRQ 0)
   │  current->quantum--
   │  if quantum == 0: current->state = READY; schedule()
   ▼
schedule()
   │  re-queue current if READY (skip idle / dead)
   │  next = dequeue() or idle if empty
   │  next->state = RUNNING
   │  context_switch(old, next)
   ▼
context_switch (context.asm)
   │  push rbx, rbp, r12-r15      (callee-saved)
   │  old->rsp = RSP
   │  RSP = new->rsp
   │  pop r15-r12, rbp, rbx
   │  ret                          (pops saved RIP → resumes new thread)
```

Each thread has its own 16 KiB kernel stack (allocated via `kmalloc`).
Switching inside the timer IRQ handler is safe because the interrupt frame lives
on the *current thread's* stack; when we switch back, `iretq` restores from that
frame. The PIC EOI is sent *before* the handler so the timer can fire again
after a context switch. For new threads, the initial stack is crafted with 6
callee-saved register slots + a return address pointing at the `thread_entry`
trampoline, plus alignment padding so the saved RSP is 16-byte aligned.

## User mode (Phase 8) and system calls (Phase 9)

```
kthread_create(user_test_thread)
   │  ... scheduling ...
   ▼
user_test_thread (Ring 0)
   │  entry = elf_load(hello_bin, hello_bin_size)
   │     validate Ehdr (magic, 64-bit, x86_64)
   │     for each PT_LOAD: map pages (USER), copy file bytes, zero .bss
   │     skip pages already mapped (co-located text/rodata segments)
   │  map_user_stack()  -> map USER_STACK_SIZE pages near 0x7FFFF0000000
   │  tss_set_rsp0(kstack) ; set_syscall_stack(kstack)
   ▼
jump_to_user_asm (user_entry.asm)
   │  push SS(0x23), RSP(user), RFLAGS(IF=1), CS(0x1B), RIP(entry)
   │  iretq  --> Ring 3
   ▼
hello (Ring 3, compiled C + libc)
   │  _start -> main -> write(1, "hello\n", 6)
   │     libc write() -> syscall(SYS_WRITE, ...) -> SYSCALL instruction
   ▼
syscall_entry (Ring 0, on the kernel stack via set_syscall_stack)
   │  save user RCX/R11/RSP ; load kernel stack
   │  remap C ABI args ; call syscall_dispatch(num, ...)
   │  SYS_WRITE: kputchar each byte -> console
   │  restore user RSP/RCX/R11 ; o64 sysret -> Ring 3
   ▼
hello returns from main -> _exit(0) -> SYS_EXIT -> thread_exit()
```

### Syscall ABI (Linux-compatible)

| Register | Role                                    |
|----------|-----------------------------------------|
| RAX      | syscall number (in) / return value (out)|
| RDI      | arg 1                                   |
| RSI      | arg 2                                   |
| RDX      | arg 3                                   |
| R10      | arg 4                                   |
| R8       | arg 5                                   |
| R9       | arg 6                                   |
| RCX      | saved user RIP (for SYSRET)             |
| R11      | saved user RFLAGS (for SYSRET)          |

Implemented syscalls: `read` (0), `write` (1), `getpid` (39), `exit` (60).

### ELF loading

The compiled `hello.elf` (built by `make user`) is embedded as a C array
(`hello_bin[]`, generated by `tools/gen_user_binary.py`) and `#include`d by the
kernel. `elf_load()` validates the ELF64 header, maps each PT_LOAD segment with
USER permissions, copies file bytes, and zero-fills `.bss`. Segments sharing a
page are handled by checking `paging_get_phys()` before mapping. The user
program shares the kernel's address space for now; per-process address spaces
are a follow-up.

### Critical SYSCALL/SYSRET details

- **SYSCALL does not switch stacks.** Unlike interrupts, the handler would run
  on the *user's* RSP, corrupting return addresses. We manually switch to a
  kernel stack (`set_syscall_stack`) at entry and restore the user RSP before
  SYSRET.
- **`o64 sysret`** (not plain `sysret`): NASM's 32-bit-operand SYSRET sets
  `CS = STAR[63:48] | 3`; the 64-bit version correctly sets
  `CS = (STAR[63:48] + 0x10) | 3 = 0x1B`.
