# AuraLite STOP codes (the blue screen)

When the **kernel** hits an unrecoverable error it paints a blue screen
and prints a `STOP` code.  This is the AuraLite equivalent of a Windows
bugcheck / Linux panic banner: the machine is halted on purpose so the
fault cannot silently corrupt disks or reset without a trace.

User-mode faults are **not** STOP codes.  A `#PF` / `#GP` / `#UD` from
Ring 3 becomes a POSIX signal (`SIGSEGV`, `SIGILL`, …) and only that
process dies.  The compositor and the shell keep running.

The serial console always gets the full dump first (`[diag] === KERNEL
EXCEPTION …` plus `[bsod] STOP=…`).  The blue screen is best-effort
after that: it must not take the `kprintf` lock and must not allocate.

See also: [`architecture.md`](architecture.md) (exception path),
[`status.md`](status.md).

---

## How to read the screen

```
:(

AuraLite OS
A problem has been detected and AuraLite has been shut down.

STOP: 0x0000000E  (PAGE_FAULT)
Page fault (#PF). EXTRA is CR2, the address that was not mapped or not writable.

CPU    0
RIP    0xFFFFFFFF8010ABCD
EXTRA  0x0000000000000000

This STOP code is documented in docs/bsod.md.  System halted.
```

| Field | Meaning |
|---|---|
| `STOP` | 32-bit code. Low byte of `0x000000xx` is the CPU exception vector. `0x00001xxx` is a software stop. |
| name in parentheses | Stable symbolic name from the table below. Greppable. |
| `CPU` | Which logical CPU took the fault (`diag_cpu_id()`). |
| `RIP` | Instruction pointer that faulted. 0 if the stop was not an exception. |
| `EXTRA` | For `#PF` (0x0E): **CR2**, the linear address that was accessed. Otherwise the CPU error code, or 0. |
| `DETAIL` | Optional extra sentence (`ASSERT` condition, `PANIC` format string, canary class). |

On a BIOS boot with no linear framebuffer the same text is written to
the 80×25 VGA console (white on blue, attribute `0x1F`).  On UEFI GOP
it is painted into the 32-bpp framebuffer.  After the paint, compositor
flips become no-ops so an AP cannot overwrite the screen.

---

## How to trigger one (for tests)

From the shell, same hooks as FIX_R0 / FIX_R1:

```text
write /proc/sysrq-trigger c     # STOP 0x0000000E  PAGE_FAULT   (NULL write)
write /proc/sysrq-trigger o     # STOP 0x00000008  DOUBLE_FAULT (stack overflow → #DF on IST1)
```

Serial must contain `[bsod] STOP=0x000000000000000e PAGE_FAULT` (the
hex printer always writes 16 digits) and the machine must **not**
reboot (one `Hello from AuraLite OS kernel!` banner).

---

## CPU exception stops (`0x00000000`–`0x0000001F`)

These are the Intel SDM Vol.3 §6.15 vectors, used unchanged so a number
you already know from a CPU manual is the number on the screen.

| STOP | Name | When it fires |
|---:|---|---|
| `0x00000000` | `DIVIDE_ERROR` | Integer division by zero or overflow (`#DE`). |
| `0x00000001` | `DEBUG` | Debug exception (`#DB`) in the kernel. |
| `0x00000002` | `NMI` | Non-maskable interrupt. |
| `0x00000003` | `BREAKPOINT` | `INT3` (`#BP`) in kernel mode. |
| `0x00000004` | `OVERFLOW` | `INTO` overflow (`#OF`). |
| `0x00000005` | `BOUND_RANGE` | `BOUND` range exceeded (`#BR`). |
| `0x00000006` | `INVALID_OPCODE` | Illegal instruction (`#UD`). |
| `0x00000007` | `DEVICE_NOT_AVAILABLE` | FPU/SSE used before it was enabled (`#NM`). |
| `0x00000008` | `DOUBLE_FAULT` | Exception while delivering another exception (`#DF`). Runs on IST1. |
| `0x00000009` | `COPROCESSOR_SEGMENT` | Legacy coprocessor overrun (unused in long mode). |
| `0x0000000A` | `INVALID_TSS` | Invalid TSS during a privilege change (`#TS`). |
| `0x0000000B` | `SEGMENT_NOT_PRESENT` | Not-present segment loaded (`#NP`). |
| `0x0000000C` | `STACK_FAULT` | Stack-segment fault (`#SS`). |
| `0x0000000D` | `GENERAL_PROTECTION` | Privileged or non-canonical access (`#GP`). |
| `0x0000000E` | `PAGE_FAULT` | Unmapped / not-writable / NX page (`#PF`). `EXTRA` = CR2. |
| `0x00000010` | `X87_FLOAT` | x87 floating-point exception (`#MF`). |
| `0x00000011` | `ALIGNMENT_CHECK` | Misaligned access with AC enabled (`#AC`). |
| `0x00000012` | `MACHINE_CHECK` | Uncorrectable CPU hardware error (`#MC`). |
| `0x00000013` | `SIMD_FLOAT` | SIMD floating-point exception (`#XM`). |
| `0x00000014` | `VIRTUALIZATION` | Virtualization exception (`#VE`). |
| `0x00000015` | `CONTROL_PROTECTION` | CET control-protection (`#CP`). |

Vectors 15 and 16–31 that are reserved by the SDM and have no table
row still produce a STOP of that number with the name `UNKNOWN`.

Recoverable `#PF`s never reach this table: COW, stale-TLB, demand
paging and `copy_*_user` fixups return to the faulting instruction.

---

## Software stops (`0x00001xxx`)

| STOP | Name | When it fires |
|---:|---|---|
| `0x00001001` | `KASSERT` | `ASSERT(cond)` failed. `DETAIL` is the condition text. |
| `0x00001002` | `KEXPLICIT` | An explicit kernel stop (`PANIC("…")`). `DETAIL` is the format string. |
| `0x00001003` | `KCANARY` | Stack-protector trip (`__stack_chk_fail`). `DETAIL` is the class (`CANARY-VALUE-MISMATCH` / `GENUINE-OVERFLOW`). `RIP` is the protected epilogue; `EXTRA` is `RSP`. |
| `0x00001004` | `KSTACK` | Kernel thread hit its unmapped guard page. Escalates to `#DF` (`0x08`) if the stack is already dead and IST1 takes over. |
| `0x00001005` | `KRECURSE` | A second fault occurred inside `diag_early_dump`. The first dump is already on the serial wire. |
| `0x000010FF` | `KHALT` | Reserved for a halt with no more specific code. Not used by the current call sites. |

---

## Source of truth

The table lives in `kernel/lib/bsod.c` (`k_stops[]`).  The host test
`tests/unit/test_bsod.c` compiles that file and checks the names.
If you add a stop code, add a row there, a line in this document, and
a `CHECK` in the host test, in the same commit.

Implementation:

| File | Role |
|---|---|
| `kernel/lib/bsod.h` | codes and API |
| `kernel/lib/bsod.c` | table, serial banner, line builder |
| `drivers/framebuffer/fb.c` `fb_bsod_paint()` | 32-bpp fill + PSF text, or VGA 80×25 |
| `drivers/framebuffer/graphics.c` `gfx_bsod_seize()` | compositor flips become no-ops |
| `kernel/arch/x86_64/isr.c` | kernel exceptions and `#DF` / guard overflow |
| `kernel/lib/assert.h` | `ASSERT` / `PANIC` |
| `kernel/lib/stack_protector.c` | canary trip |
