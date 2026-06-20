# NovOS Architecture

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
   └── test_exception_handling()  divide-by-zero -> dump -> halt
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
