# Win32 personality — limitations and behaviour notes

**Status:** in progress. Phases W32-0 – W32-6 of [`WIN32_PLAN.md`](../WIN32_PLAN.md)
are implemented; the full API matrix and the SDK arrive with W32-8.

This document exists to record what the personality *does not* do. The
function list is discoverable from the headers and from `w32/src/w32_bind.c`;
what a reader cannot discover by grepping is which behaviours are
approximations, and those are what break programs in ways that look like
bugs somewhere else.

## Structured exception handling is not table-driven unwinding

`__try`/`__except` works: a fault inside a guarded region transfers control
to the handler, `GetExceptionCode()` reports the documented `EXCEPTION_*`
value, and nesting behaves (the innermost handler wins). It is implemented
on `sigsetjmp`/`siglongjmp` over AuraLite's existing signal delivery.

What real Win64 SEH does and this does not:

- **Frames between the fault and the handler are abandoned.** Real SEH walks
  `.pdata`/`.xdata` and runs cleanup for each frame it unwinds past. This
  shim jumps straight to the handler.
- **Destructors of live C++ objects do not run.** This follows directly from
  the point above and is the practical consequence to be aware of: a C++
  program that relies on RAII cleanup during exception propagation will leak
  whatever those destructors would have released.
- **`__finally` is not implemented.** It would need the same unwind
  machinery.
- **`RaiseException` is not implemented**, so software-raised exceptions —
  including C++ `throw` — are out of scope. Only faults the CPU raises
  synchronously are caught.
- **`EXCEPTION_CONTINUE_EXECUTION` is not supported.** Resuming at the
  faulting instruction requires a full machine context to return to, which
  the shim does not reconstruct.

Table-driven SEH is listed as an explicit non-goal (decision D8) rather than
as pending work: doing it properly means implementing the unwinder, and a
half-implemented unwinder is worse than an honest `longjmp`.

## Thread-local storage is per-process, not per-thread

TLS callbacks in a PE's TLS directory are executed at startup, in order, and
the TLS index is written so `__declspec(thread)` reads resolve. But there is
one TLS block per process, not one per thread.

The reason is specific and worth recording. On Windows, TLS is reached
through the TEB at `GS:[0x58]`. On AuraLite, `IA32_GS_BASE` holds the
kernel's per-CPU pointer, and the `SYSCALL` entry stub reads `[gs:...]`
directly **with no `swapgs`** — see `kernel/arch/x86_64/syscall_entry.asm`
and `cpu_local.c`. Giving user mode its own GS base would mean introducing
`swapgs` on every kernel entry and exit path, which is a kernel-wide change
well outside a personality phase. Until then, a multi-threaded Win32 program
would see one thread's TLS from all of its threads.

`w32run` is single-threaded today, so nothing currently observes the
difference — but it is the first thing that will break when threads arrive.

## Command-line parsing

`argc`/`argv` are built from the command line using the documented Microsoft
rules, including the backslash-run rules before a quote, the `""`
escape inside quotes, and the special handling of `argv[0]` (where
backslashes are always literal because it is a path). These are verified
against Microsoft's own published examples in `tests/unit/test_w32_argv.c`.

Unterminated quotes are accepted rather than rejected — everything to the end
of the line becomes the final argument, which is what Windows does.

## Static initialisers

`.CRT$XCA`/`.CRT$XCU`/`.CRT$XCZ` contributions are merged by the linker into
a single `.CRT` section, and the loader runs the function pointers in it,
skipping the NULL padding. This is enough for C++ global constructors and for
anything else the compiler routes through that table.

## What runs where

Import binding and the CRT startup sequence currently run in **user space**,
in `w32run`, not in the kernel exec path. A consequence is that the image is
mapped as one RW+X region rather than with per-section W^X, which is weaker
than what the kernel's PE loader does for a directly-executed `.exe`
(W32-3). Moving binding into the kernel exec path is tracked as remaining
W32-6 work in `WIN32_PLAN.md`; the hardened path is the kernel one.

## Not implemented at all

Registry, COM, .NET, DirectX, WinSock, WOW64 (32-bit programs),
`LoadLibrary`/`GetProcAddress` (W32-7), and `BitBlt`/off-screen device
contexts. The `W` (UTF-16) entry points exist for `KERNEL32` where the plan
required them and are otherwise deferred; `A` entry points are the primary
surface today, which is the reverse of decision D6 and is noted there.
