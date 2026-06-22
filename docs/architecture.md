# AuraLite OS Architecture

This document explains the kernel architecture and boot-time subsystem order.
It originated during the 14-phase bring-up plan, but the current tree also
contains post-phase extensions: per-process address spaces, DHCP/DNS/TCP,
USB/AHCI scaffolding, Bluetooth/Wi-Fi protocol layers, a full GUI demo and a
3D renderer. For a precise feature-completeness table, see
[`status.md`](status.md).

## Boot flow

```
SeaBIOS (QEMU) → Limine stage1/stage2 (BIOS)
   │  VBE: set 1280x800x32 framebuffer @ 0xfd000000
   │  Parse /boot/limine/limine.conf
   │  Load boot():/boot/kernel.elf + initrd.tar module
   │  Map PT_LOAD segments by permission; set up HHDM + memmap
   │  Scan marker-delimited request list; fill .response pointers
   │  Jump to ELF entry (_start) in 64-bit long mode, ring 0
   ▼
_start (boot.asm)
   │  cli; rsp := &stack_top (64 KiB); zero .bss
   ▼
kmain (kernel.c)
   ├── uart_init()           COM1 @ 115200 baud
   ├── fb_init()             console on the Limine framebuffer (8×8 font)
   ├── gdt_init()            7-entry GDT (kernel/user segments + TSS)
   ├── idt_init()            256-entry IDT + LIDT
   ├── pic_init()            8259A remap (IRQ 0-15 -> 32-47)
   ├── sti                   enable maskable interrupts
   ├── tss_init()            TSS with RSP0 + IST1 (#DF stack)
   ├── syscall_init()        SYSCALL/SYSRET MSRs (STAR, LSTAR, SFMASK, EFER.SCE)
   ├── pmm_init()            bitmap from Limine memmap (via HHDM)
   ├── paging_init()         read PML4 from CR3; enable EFER.NXE
   ├── kheap_init()          on-demand heap (16 MiB, first-fit)
   ├── smp_init()            wake APs via Limine MP (up to 4 CPUs)
   ├── pit_init(100)         100 Hz timer (IRQ 0)
   ├── sched_init()          round-robin scheduler + idle thread
   ├── vfs_init() + initrd   USTAR initrd at /, devfs at /dev
   ├── net_init()            e1000 NIC + DHCP + ARP + ICMP + DNS + TCP tests
   ├── ahci_init()           AHCI controller/port detection (sector I/O disabled)
   ├── usb init              UHCI/OHCI/EHCI/xHCI + USB core + MSC protocol layer
   ├── bt_init()/wifi_init() Bluetooth HCI / 802.11 protocol frameworks
   ├── gfx_init()            double-buffered 2D graphics
   ├── keyboard_init()       PS/2 keyboard (IRQ 1)
   ├── mouse_init()          PS/2 mouse (IRQ 12)
   ├── wm_demo()             framebuffer window-manager demo
   ├── r3d_demo()            software 3D renderer demo
   ├── process_self_test()   spawn /hello in isolated address space
   ├── user_mode_self_test() load init.elf (shell) → Ring 3
   └── yield forever         shell runs interactively
```

## Interrupt handling

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
   ├── vector < 32  : exception
   │     if from USER mode (CS & 3 == 3): kill the thread, schedule()
   │     else: dump registers + stack trace + CR2 (if #PF), halt
   └── vector 32-47 : IRQ -> pic_eoi() BEFORE handler, then dispatch
```

The PIC EOI is sent **before** the handler so the timer can deliver the next
tick after a context switch inside the handler (preemptive scheduling).

The 256 vector stubs and their addresses are macro-generated; `isr_table[]`
(an address array in `.rodata`) is consumed by `idt_init()` to fill every gate.
Vectors that push an error code (8, 10–14, 17) use `ISR_ERR`; the rest push a
dummy zero so `registers_t` is always the same shape.

## Limine protocol bridge

`kernel/limine_requests.c` emits, in strict order inside the writable `.data`
segment:

1. `LIMINE_REQUESTS_START_MARKER` (4 qwords)
2. base revision (revision 3)
3. framebuffer request
4. memmap request
5. HHDM request
6. module request (initrd)
7. MP request (SMP)
8. `LIMINE_REQUESTS_END_MARKER` (2 qwords)

## Consoles

`kputchar()` is the single fan-out point: it writes to **both** the UART and the
framebuffer. `kprintf` is SMP-safe via a global print spinlock (cli/sti is
per-CPU under SMP). Phase 14 adds a separate double-buffered graphics layer for
2D rendering.

## GDT (7 entries)

| Index | Selector | Type              | DPL |
|-------|----------|-------------------|-----|
| 0     | 0x00     | null              | —   |
| 1     | 0x08     | kernel code (64b) | 0   |
| 2     | 0x10     | kernel data       | 0   |
| 3     | 0x18     | user data         | 3   |
| 4     | 0x20     | user code (64b)   | 3   |
| 5–6   | 0x28     | 64-bit TSS (16B)  | 0   |

User data is at index 3 and user code at index 4 (swapped from the conventional
order) so that SYSRET's formula (`CS = base+0x10`, `SS = base+0x08`) with
`STAR[63:48]=0x10` produces `CS=0x23` and `SS=0x1B` — both DPL-3 selectors.

## Physical memory management

```
Limine memmap  ──►  pmm_init()
                     │  highest usable addr → bitmap size
                     │  carve bitmap from bootloader-reclaimable RAM
                     │  reach it via HHDM (0xFFFF800000000000 + phys)
                     │  memset 0xFF (all used); clear USABLE regions
                     ▼
                 bitmap: bit SET = used, bit CLEAR = free
                     │
   pmm_alloc_frame()         first clear bit → phys addr (0 = OOM)
   pmm_alloc_contiguous(n)   first run of n clear bits → base phys
   pmm_free_frame(phys)      clear bit, double-free guarded
                     all serialised by an irqsave spinlock
```

The allocation algorithms live in the pure-C, kernel-independent
`kernel/lib/bitmap.h`, so the *same code* is unit-tested on the host
(`tests/unit/test_pmm.c`) and used in the kernel.

## Virtual memory

```
paging_init()
   │  read CR3 → PML4 (set up by Limine)
   │  set EFER.NXE (enable No-Execute bit in PTEs)
   ▼
paging_map(virt, phys, flags)
   │  walk_pte(virt, create=1)
   │     PML4 → PDPT → PD → PT   (allocate + zero missing tables via PMM)
   │     tables reached via HHDM (phys + 0xFFFF800000000000)
   │  *PTE = phys | flags
   │  invlpg(virt)
```

The VMM does **not** build paging from scratch — Limine already has long-mode
paging enabled. The VMM extends Limine's page tables, reaching
newly-allocated table frames through the HHDM. This avoids the classic
chicken-and-egg of "map a table to manage tables."

## Kernel heap

```
heap_alloc(size)
   │  first-fit search of the free list
   │  if a free block >= need: split (optional), mark used, remove from list
   │  else: expand() → map PMM frames into the heap region (VMM, NX set)
   │        → add the new span as one free block → retry search
   ▼
heap_free(ptr)
   │  mark free; coalesce NEXT neighbour (in range, via its header)
   │  coalesce PREVIOUS neighbour (via the boundary-tag footer before it)
   │  insert into the free list (unless absorbed into the previous block)
```

The allocator core (`heap.c`) is freestanding (only `<stdint.h>`), with
page-backed expansion injected as a callback. The *same object* is linked into
both the kernel and the host unit test.

## Multitasking

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
   │  pushfq                       (save RFLAGS including IF)
   │  old->rsp = RSP
   │  RSP = new->rsp
   │  popfq                        (restore RFLAGS)
   │  pop r15-r12, rbp, rbx
   │  ret                          (pops saved RIP → resumes new thread)
```

RFLAGS is saved/restored so the interrupt flag doesn't leak between threads
(critical: a thread running with IF=0 in a SYSCALL handler must not inherit
IF=1 from a thread it was switched from).

Each thread has its own 16 KiB kernel stack. Switching inside the timer IRQ
handler is safe because the interrupt frame lives on the *current thread's*
stack. The initial stack for a new thread includes an RFLAGS slot (0x202 = IF
set) so new threads start with interrupts enabled.

## User mode and system calls

```
kthread_create(user_test_thread)
   │  ... scheduling ...
   ▼
user_test_thread (Ring 0)
   │  entry = elf_load(init_bin, init_bin_size)
   │     validate Ehdr (magic, 64-bit, x86_64)
   │     for each PT_LOAD: map pages (USER), copy file bytes, zero .bss
   │  map_user_stack()  → map 64 KiB near 0x7FFFF0000000
   │  tss_set_rsp0(kstack)
   ▼
jump_to_user_asm (user_entry.asm)
   │  push SS(0x1B), RSP(user), RFLAGS(IF=1), CS(0x23), RIP(entry)
   │  iretq  → Ring 3
   ▼
init shell (Ring 3, compiled C + libc)
   │  _start → main → write(1, "...") → syscall(SYS_WRITE) → SYSCALL
   ▼
syscall_entry (Ring 0, runs on the user stack)
   │  save RCX (user RIP), R11 (user RFLAGS) to globals
   │  remap C ABI args; call syscall_dispatch(num, ...)
   │  o64 sysret → Ring 3
```

### Implemented syscalls

The syscall table now includes console/file I/O, process-management helpers and
networking extensions: `read`, `write`, `open`, `close`, `getpid`, `fork`,
`execve`, `exit`, `wait4`, `listdir`, `spawn`, `dns`, `net_connect`,
`net_send`, `net_recv`, `net_close`, and `net_ping`.

Some of these are experimental and intentionally simplified. See
[`syscall_abi.md`](syscall_abi.md) for the exact numbers and caveats.

### Critical SYSCALL/SYSRET details

- **SYSCALL does not switch stacks.** The handler runs on the user's RSP. This
  is safe because the user stack is writable + user-accessible, and timer
  interrupts switch to the TSS.RSP0 kernel stack (a different stack).
- **`o64 sysret`** (not plain `sysret`): NASM's 32-bit-operand SYSRET sets
  `CS = STAR[63:48]`; the 64-bit version correctly sets
  `CS = (STAR[63:48] + 0x10) | RPL3 = 0x23`.

## SMP (Phase 12)

Limine's MP request enumerates all CPUs. Each AP is assigned a `goto_address`
function that switches to its own stack, loads the shared GDT/IDT, reports
online atomically, and enters an idle loop. The BSP skips its own entry in the
cpus[] array. Writes to `goto_address`/`extra_argument` go through volatile +
mfence for visibility.

## File system (Phase 10)

The VFS uses longest-prefix mount matching: `/dev/null` matches the `/dev`
mount and delegates `null` to devfs's lookup. The USTAR initrd is mounted at
`/` (read-only). DevFS provides `/dev/null` and `/dev/zero`.

## Networking (Phase 13)

PCI scan finds the e1000 NIC; its MMIO is explicitly mapped via paging (the
HHDM doesn't cover device MMIO). TX/RX descriptor rings and buffers are
PMM-allocated (physical frames for DMA). The network stack implements Ethernet
framing, ARP (with cache), IPv4 (RFC 1071 checksum), and ICMP echo. RX polls
the RDH MMIO register rather than the descriptor status byte.

## Graphics (Phase 14)

A double-buffered 2D library renders to an off-screen back buffer
(`kmalloc`-allocated) and flips it to the visible framebuffer via `memcpy`,
avoiding tearing. Provides pixel plotting, filled/outlined rectangles,
Bresenham lines, and bitmap-font text. A PS/2 keyboard driver (IRQ 1,
scan-code set 1) feeds a ring buffer of decoded ASCII characters.
