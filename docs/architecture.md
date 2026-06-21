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
   └── kheap_init() + kheap_self_test()  on-demand heap; 10k cycles
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

A minimal flat table (null / 64-bit code / data) is built in C and loaded by
`gdt_flush.asm`, which reloads every segment register and switches `CS` via a
far return. This is prerequisite for our own IDT and syscall entries later.

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
