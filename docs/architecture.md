# AuraLite OS Architecture

This document explains the kernel architecture and boot-time subsystem order.
It originated during the 14-phase bring-up plan, but the current tree also
contains post-phase extensions: per-process address spaces, DHCP/DNS/TCP,
working QEMU AHCI sector I/O, FAT32/ext2 filesystems, UHCI-backed USB Mass
Storage, Bluetooth/Wi-Fi protocol layers, a GUI syscall layer, bundled GUI
applications and a 3D renderer. For a precise feature-completeness table, see
[`status.md`](status.md).

## Boot flow

AuraLite boots through its own BIOS and UEFI loaders (`boot/`); there is no
third-party bootloader anywhere in the chain. Both paths converge on the same
contract: fill a `boot_info_t`, put its address in RDI, jump to the kernel ELF
entry in 64-bit long mode. See `docs/BL{1..8}_REPORT.md` for each phase and
`docs/BOOTLOADER_ROADMAP.md` for the index.

```
BIOS path                                  UEFI path
─────────────────────────────────          ─────────────────────────────────
SeaBIOS loads the 512-byte MBR             OVMF loads BOOTX64.EFI from the ESP
  boot/bios/stage1/mbr_dual.asm              boot/uefi/efi_main.c
   │  INT 13h LBA read of Stage 2            │  locate kernel.elf + initrd.tar
   ▼                                         │  efi_elf.c: map PT_LOAD segments
[BL3] Stage 2 (boot/bios/stage2/)            │  efi_paging.c: build page tables
   │  E820 memory map      (e820.inc)        │  efi_acpi.c: RSDP from cfg table
   │  A20 gate             (a20.inc)         │  ExitBootServices()
   │  unreal mode          (unreal.inc)      │
   │  ACPI RSDP + MADT     (acpi.inc)        │
   │  [BL4] FAT32 BPB read (fat.inc)         │
   │  load kernel.elf      (elf.inc)         │
   │  load initrd.tar                        │
   │  build page tables    (paging.inc)      │
   │  enter long mode      (longmode.inc)    │
   └───────────────┬─────────────────────────┘
                   ▼
        boot_info_t in RDI, magic 0x4155524142544c44
        (framebuffer, memmap, initrd phys+size, HHDM, ACPI, boot path)
                   ▼
_start (kernel/arch/x86_64/boot.asm)
   │  cli; zero .bss; rsp := &stack_top (64 KiB, in .bss)
   ▼
kmain (kernel/kernel.c)
   ├── boot_info_init()      latch RDI handoff; halt on bad magic
   ├── uart_init()           COM1 @ 115200 baud
   ├── stack_protector_init()
   ├── fb_init()             console on the boot-provided framebuffer (8×8 font)
   ├── gdt_init()            7-entry GDT (kernel/user segments + TSS)
   ├── idt_init()            256-entry IDT + LIDT
   ├── pic_init()            8259A remap (IRQ 0-15 -> 32-47)
   ├── syscall_init()        SYSCALL/SYSRET MSRs (STAR, LSTAR, SFMASK, EFER.SCE)
   ├── sti                   enable maskable interrupts
   ├── pmm_init()            bitmap + refcount array from the boot memmap (via HHDM)
   ├── paging_init()         adopt the loader's PML4 from CR3; enable EFER.NXE
   ├── kheap_init()          on-demand heap (64 MiB region, first-fit)
   ├── slab_init()           tcb_cache, ofd_cache, vnode_cache
   ├── tss_init()            TSS with RSP0 + IST1 (#DF stack), per CPU
   ├── smp_init()            wake APs via ACPI MADT + ap_trampoline.asm
   ├── ioapic_init()         I/O APIC bring-up; base cross-checked against the
   │                          ACPI MADT since RESIDUE R11 (`[ioapic] base ...
   │                          (MADT agree)`); legacy PIC/PIT remain available
   ├── pit_init(100)         100 Hz timer (IRQ 0)
   ├── rng_init()            ChaCha20 CSPRNG; RDSEED/RDRAND or IRQ-jitter pool
   ├── sched_init()          round-robin scheduler + idle thread
   ├── virtual_drivers_init() PCI catalog of known QEMU/VBox/VMware devices
   ├── audio_init()          PC speaker + AC97 backends
   ├── vfs_init() + initrd   USTAR initrd at /, then devfs, procfs, tmpfs, usbfs
   ├── net_init()            NIC via netdev (e1000, else virtio-net, else rtl8139)
   │                         + DHCP + ARP + ICMP + DNS + IPv6 + TCP self-tests
   ├── ahci_init()           AHCI controller/port detection + DMA read/write test
   ├── diskfs_init()         tiny persistent AHCI filesystem at /disk
   ├── fat32_init()          FAT32 at /fat, /fat/AURALOG.TXT logging
   ├── virtio_blk_init()     virtio block backend
   ├── ext2_init()           /ext2 when a second AHCI disk is present
   ├── bc_init()             buffer cache, then exfat/ext4/btrfs/f2fs/ntfs
   │                         on further disks when present
   ├── usb init              UHCI/OHCI/EHCI/xHCI + core, string, isoc, hub
   │                         + MSC, CDC ACM, USB audio, USB printer
   ├── bt_init()/wifi_init() Bluetooth HCI / 802.11 protocol frameworks
   ├── gfx_init()            double-buffered 2D graphics
   ├── keyboard_init()       PS/2 keyboard (IRQ 1, rich key-event ring)
   ├── mouse_init()          PS/2 mouse (IRQ 12, scroll-wheel event support)
   ├── wm_demo()             framebuffer window-manager demo
   ├── r3d_demo()            software 3D renderer demo
   ├── gui_init()            kernel GUI compositor (100 FPS thread) + GUI syscalls
   ├── process_self_test()   spawn /hello in an isolated address space
   ├── user_mode_self_test() load init.elf (shell) → Ring 3
   └── yield forever         shell runs interactively
```

Almost every subsystem above prints a `PASS`/`SKIP` line from a self-test that
runs on every boot, so a serial log is a live status report rather than a trace.

## The i386 boot flow (I386_PLAN)

The same Stage 2 serves a second kernel.  After the unreal-mode
self-test it asks the CPU (`lmcheck.inc`: EFLAGS.ID toggle → CPUID
extended leaf → `LM` bit) and branches; a CPU below the i686 floor
(PSE/CX8/CMOV) gets a two-console refusal instead of either kernel.

```
Stage 2 (BL3, shared with the 64-bit path up to here)
   │  E820, A20, unreal mode, ACPI
   │  check_long_mode (lmcheck.inc)
   ├── long mode ────────────► KERNEL.ELF path (diagram above)
   ▼  no long mode
check_i686                     refuse below the floor (D1)
   │  hhdm_offset := 0xC0000000 in boot_info_t
   │  FAT32: load KERNEL32.ELF   (elf32.inc, refuses ELFCLASS64)
   │  enter_prot32 (pmode32.inc): flat GDT, CR0.PE, ESP scratch
   ▼
        boot_info_t in ESI (the 32-bit register sibling of RDI)
                   ▼
_start (kernel/arch/i386/boot32.asm, .boot section, VMA = LMA)
   │  build PSE page directory: identity + direct map at 0xC0000000
   │  CR4.PSE, CR0.PG|WP ; jump higher-half ; zero .bss ; stack
   ▼
kmain32 (kernel/arch/i386/main32.c)
   ├── uart32/vga32           two-sink console (COM1 + 80x25 text VGA)
   ├── gdt_init()             flat Ring 0/3 + 32-bit TSS (esp0 = ring transition)
   ├── idt_init()             256 gates (isr_stubs32.asm)
   ├── pic32/pit32            8259A remap, 100 Hz
   ├── paging32/pmm32/kheap32 identity window dropped; [pmm]/[vmm]/[heap] PASS
   ├── kbd32_init()           PS/2 set 1 + polled UART RX, one input ring
   ├── ata32/net32            PIO sector I/O self-test; DHCP+ARP+ICMP self-test
   ├── sched32_init()         round-robin, BSP-only (D5), post-EOI preemption
   ├── syscall32_init()       int 0x80, DPL=3, AuraLite numbers (D4)
   ├── user32_selftest()      Ring 3 round-trip + #GP containment
   ├── initrd32 + init32      shared initrd.tar, ELF32 from /bin32
   └── shell32                auralite# on the merged console
```

The two kernels never share a binary artefact, only contracts: the
`boot_info_t` layout (offset-checked across widths by
`tests/unit/test_boot_info_width.c`), the syscall number table, and the
portable sources that migrate in through `kernel/arch/arch.h` under the
width-sweep ratchets.

## The riscv64 boot flow (RISCV_PLAN)

The third architecture has no Stage 2 of ours at all: OpenSBI (the
M-mode platform firmware QEMU bundles — plan D2, the "BIOS role") loads
`kernelrv.elf`'s segments and jumps to the payload **base**,
`0x80200000` — not the ELF entry point (measured in V0; `.text.boot`'s
first-byte placement in `kernelrv.ld` is load-bearing).  There is no
loader of ours to fill `boot_info_t`, so the kernel's own FDT shim is
the struct's third producer.

```
OpenSBI (M-mode, qemu -machine virt -kernel build/kernelrv.elf)
   │  a0 = boot hartid, a1 = DTB physical address, satp = 0
   ▼
_start (kernel/arch/riscv64/boot.S, .text.boot @ 0x80200000, VMA = LMA)
   │  hart lottery (amoswap.d) — losers park in wfi (D5)
   │  satp := early root table (assembly-time gigapage constants:
   │          identity @2 for two fetches, HHDM @256..259)
   │  literal-pool long jump high (medany cannot span the HHDM gap)
   ▼
_start_high (higher half at HHDM + phys)         zero .bss ; stack
   ▼
kmain_rv (kernel/arch/riscv64/main_rv.c)
   ├── sbi_console_init()     DBCN probe, legacy fallback (output day 0)
   ├── fdt_parse()            DTB → boot_info_t (magic LAST) + platform
   ├── trap_init()            stvec, 16 named scause codes, 100 Hz SBI timer
   ├── plic_init()            S-context; line proven with a real THRE irq
   ├── pmm/paging/kheap       final Sv39 tables: W^X REAL, identity dropped,
   │                          [pmm]/[vmm]/[heap] PASS + 3 fault probes
   ├── sched_rv_init()        round-robin, boot hart only, post-re-arm preempt
   ├── user_rv_selftest()     U-mode ecall round trip + contained csrr fault
   ├── uart_rv_init()         16550 RX → PLIC IRQ 10 → cons ring
   ├── vblk/vnet (virtio-mmio) ata32-shaped blk gate; shared-miniproto net gate
   ├── initrd_rv + initrv     shared initrd.tar, ELF64/EM_RISCV from /binrv
   └── smallsh                auralite# — the SAME source the i386 shell runs
```

Three kernels, no shared binary artefacts — only contracts:
`boot_info_t` (offset-checked at all three widths), the one syscall
number table (D4: `SYSCALL`, `int 0x80`, `ecall`), the shared initrd
with per-arch tenants (`/bin`, `/bin32`, `/binrv`, `e_machine`-audited
at pack time), and the portable sources under the four ratchets.

## The aarch64 boot flow (ARM64_PLAN)

The fourth architecture boots two ways, one kernel: QEMU's ELF
`-kernel` loader enters `_start` at EL1 with the DTB parked at the
RAM base and `x0` measured NOT to be the DTB pointer (plan Fact 2) —
or the arm64 **Image** boot protocol (the header lives in `boot.S`:
`code0` jumps it, `text_offset` places the Image at the ELF link
address so one set of symbols serves both paths), where `x0` IS the
DTB and is trusted only after its magic reads back.  Only the Image
path carries `-initrd` on this board (measured in A5a).

```
QEMU -machine virt (-kernel kernela64.elf | kernela64.img -initrd initrd.tar)
   │  EL = EL1 (D1: CurrentEL != EL1 refuses with a banner; EL2 parks in wfi)
   ▼
_start (kernel/arch/aarch64/boot.S)
   │  x0 stashed; DAIF masked; sp set; .bss zeroed
   │  MAIR/TCR programmed; early TTBR0 (identity) + TTBR1 (HHDM blocks)
   │  MMU on -- literal-pool long jump high (the medany lesson's sibling)
   ▼
_start_high (higher half at HHDM + phys)
   ▼
kmain_a64 (kernel/arch/aarch64/main_a64.c)
   ├── pl011 banner            day-0 console (QEMU reset state transmits)
   ├── DTB source chosen       x0 magic-verified (Image) | RAM base (ELF)
   ├── fdt_parse()             the SHARED walker -> boot_info_t + platform
   │                           (INTIDs normalised HERE: SPI+32/PPI+16 once)
   ├── trap_init_a64()         VBAR vectors, CNTV 100 Hz from CNTFRQ_EL0, GICv2
   ├── pmm/paging/kheap        final TTBR1 tables: W^X via PXN+UXN, TTBR0
   │                           blanked, both alignment polarities measured
   ├── sched + EL0 self-test   round-robin, eager FPU; svc round trip,
   │                           privileged-op negative control contained
   ├── pl011_rx_init()         PL011 RX -> GIC INTID 33 -> cons ring;
   │                           TX through the O3 uart_ring core [AMEND-3]
   ├── vblk/vnet (virtio-mmio) the PROMOTED transport (kernel/drivers/),
   │                           attach refused over non-Device mappings
   ├── initrd_a64 + inita64    shared initrd.tar, ELF64/EM_AARCH64 from /bina64
   └── smallsh                 auralite# — the SAME source, fourth build;
                               ends by PSCI SYSTEM_OFF, never by timeout
```

Four kernels, no shared binary artefacts — only contracts:
`boot_info_t` (offset-checked at all four widths — x86_64, i686 with
`-malign-double`, rv64, a64), the one syscall number table (D4:
`SYSCALL`, `int 0x80`, `ecall`, `svc #0`), the shared initrd with
per-arch tenants (`/bin`, `/bin32`, `/binrv`, `/bina64`,
`e_machine`-audited at pack time and mutually exec-refused at boot),
the promoted single-source files (`kernel/dt/fdt.c`,
`kernel/net/miniproto.c`, `kernel/drivers/virtio_mmio.c`,
`drivers/uart/uart_ring.h`, `userspace/system/smallsh/smallsh.c`),
and the portable sources under the four width-sweep ratchets.

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
   │     if from USER mode (CS & 3 == 3): POSIX signal / kill the thread
   │     else: lock-free [diag] dump, then blue STOP screen, halt
   └── vector 32-47 : IRQ -> EOI BEFORE handler, then dispatch
                   (pic_eoi() while the 8259 is the attached controller,
                    lapic_eoi() once the BSP runs on I/O APIC IRQs — M2)
```

Kernel-mode stops are documented in [`bsod.md`](bsod.md). User-mode faults
never become a STOP: they stay POSIX signals.

The EOI is sent **before** the handler so the timer can deliver the next
tick after a context switch inside the handler (preemptive scheduling).
Since MATURITY M2 the BSP runs on I/O APIC IRQs: the 8259 is masked/decoupled
and `pic_eoi()` would poke a disconnected PIC, so the early EOI goes to the
Local APIC instead (`irq.c` chooses per attach state).

The 256 vector stubs and their addresses are macro-generated; `isr_table[]`
(an address array in `.rodata`) is consumed by `idt_init()` to fill every gate.
Vectors that push an error code (8, 10–14, 17) use `ISR_ERR`; the rest push a
dummy zero so `registers_t` is always the same shape.

## Bootloader handoff bridge

`kernel/boot_info.c` (which replaced the old bootloader-specific request table)
latches the `boot_info_t*` the loader passes in RDI. The struct is defined once
in `boot/shared/boot_info.h` and is written by **both** loaders, so the kernel
has a single handoff shape regardless of firmware:

| Field | Filled by | Consumed by |
|---|---|---|
| `magic` (`0x4155524142544c44`) | both | `boot_info_init()`, halts on mismatch |
| framebuffer (phys base, pitch, w/h/bpp) | both | `fb_init()`, `gfx_init()` |
| memmap array + count | BIOS E820 / UEFI memory map | `pmm_init()` |
| initrd phys base + size | both | `initrd_init()` |
| HHDM offset | both | every physical access |
| ACPI RSDP | `acpi.inc` / `efi_acpi.c` | `smp_init()`, `ioapic_init()` |
| boot path (BIOS or UEFI) | both | boot banner, `boot_from_uefi` |

Accessors return zero-equivalents rather than dereferencing NULL if called
before `boot_info_init()`. Note the main API difference from the old bridge:
`boot_get_memmap()` returns an array of `boot_mmap_entry_t` **values**, not an
array of pointers.


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
boot_info_t memmap ──►  pmm_init()
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
   │  read CR3 → PML4 (set up by the BL loader)
   │  set EFER.NXE (enable No-Execute bit in PTEs)
   ▼
paging_map(virt, phys, flags)
   │  walk_pte(virt, create=1)
   │     PML4 → PDPT → PD → PT   (allocate + zero missing tables via PMM)
   │     tables reached via HHDM (phys + 0xFFFF800000000000)
   │  *PTE = phys | flags
   │  invlpg(virt)
```

The VMM does **not** build paging from scratch — the bootloader has already
enabled long-mode paging (`boot/bios/stage2/paging.inc` on the BIOS path,
`boot/uefi/efi_paging.c` on the UEFI path). The VMM adopts and extends those
tables, reaching newly-allocated table frames through the HHDM. This avoids the
classic chicken-and-egg of "map a table to manage tables."

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

Exited threads are not freed on their own stack. `thread_exit()` marks the TCB
`THREAD_DEAD`, cleans up GUI windows owned by the exiting process, closes its
per-process FDs, records a wait notification for the parent and links the TCB
onto a zombie list. Later `thread_reap_zombies()` runs
from another thread's stack and frees the TCB plus kernel stack. Full user
address-space/page-table freeing is still future work.

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

The syscall table includes console/file I/O, process-management helpers, VFS
path operations, networking extensions and GUI calls: `read`, `write`, `open`,
`close`, `getpid`, `fork`, `execve`, `exit`, `wait4`, `listdir`, `spawn`,
`dns`, `net_connect`, `net_send`, `net_recv`, `net_close`, `net_ping`,
`mkdir`, `rmdir`, `unlink`, `rename`, `truncate`, `stat`, `SYS_GUI_CALL`, and
`SYS_GUI_EVENT`.

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

`smp_init()` enumerates CPUs from the ACPI MADT that the bootloader recorded in
`boot_info_t`, then wakes each AP with an INIT-SIPI-SIPI sequence pointed at
`boot/smp/ap_trampoline.asm`. The trampoline brings the AP from real mode up to
long mode, switches to its own stack, loads the shared GDT/IDT, reports online
atomically, and enters an idle loop. The BSP skips its own entry in the
cpus[] array. Writes to `goto_address`/`extra_argument` go through volatile +
mfence for visibility.

## File system (Phase 10)

The VFS uses longest-prefix mount matching: `/dev/null` matches the `/dev`
mount and delegates `null` to devfs's lookup. The USTAR initrd is mounted at
`/` (read-only). DevFS provides `/dev/null` and `/dev/zero`; tmpfs is mounted at
`/tmp`; AHCI-backed writable filesystems are mounted at `/disk`, `/fat`, and,
when a second AHCI disk is present, `/ext2`. FAT32 supports subdirectories and
VFAT long names; ext2 supports Linux-mkfs images and in-kernel formatting for
blank test disks.

## Networking (Phase 13)

PCI scan finds the e1000 NIC; its MMIO is explicitly mapped via paging (the
HHDM doesn't cover device MMIO). TX/RX descriptor rings and buffers are
PMM-allocated (physical frames for DMA). The network stack implements Ethernet
framing, ARP (with cache), IPv4 (RFC 1071 checksum), and ICMP echo. RX polls
the RDH MMIO register rather than the descriptor status byte.

## Graphics and GUI (Phase 14+)

A double-buffered 2D library renders to an off-screen back buffer
(`kmalloc`-allocated) and flips it to the visible framebuffer via `memcpy`,
avoiding tearing. Provides pixel plotting, filled/outlined rectangles,
Bresenham lines, and bitmap/PSF-font text.

The legacy framebuffer window-manager demo remains for compatibility tests. The
newer kernel GUI layer (`kernel/gui/`) manages windows, Z-order, focus,
drag/resize/minimize/maximize/close state, per-window event rings, cursor shapes,
and a cooperative compositor thread (`gui_compositor_thread`).

### GUI Anti-Freeze Architecture (Windows 10 / QEMU)
To prevent QEMU and Windows display throttling or freezing, the compositor architecture incorporates two key mechanisms:
1. **Guaranteed 100 FPS Updates:** `dirty = 1` is forcibly set on every compositor tick, guaranteeing that the frame buffer is composited 100 times per second.
2. **Cooperative Sleeping:** `gui_compositor_thread` uses a cooperative sleep loop (`while (timer_get_ticks() < target) sched_yield();`) instead of `hlt` spin-locking. This prevents the compositor from monopolizing the 50ms scheduler quantum, drastically improving UI responsiveness and event handling for userspace apps.

User GUI applications talk to it through `SYS_GUI_CALL`
and `SYS_GUI_EVENT`, wrapped by `libauragui` widgets and drawing helpers.

Keyboard input now has both ASCII and rich event paths (modifiers, navigation
keys, function keys). The PS/2 mouse driver reports movement, buttons and wheel
events for the compositor.
