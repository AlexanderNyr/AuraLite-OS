# Win32 personality — limitations and behaviour notes

**Status:** complete. Phases W32-0 – W32-8 of
[`WIN32_PLAN.md`](../WIN32_PLAN.md) are implemented. A mingw-w64-built
`.exe` runs unmodified.

This document exists mostly to record what the personality *does not* do.
The function list below is generated, so it cannot drift; what a reader
cannot discover by grepping is which behaviours are approximations, and
those are what break programs in ways that look like bugs somewhere else.

**Disclaimer.** This is an independent reimplementation of a published
interface. It contains no Microsoft code and is not endorsed by or
affiliated with Microsoft; "Windows" and "Win32" are their owners'
trademarks, used here only to describe what the interface is. Declarations
came from published documentation and mingw-w64's public-domain headers, and
no Wine or ReactOS source was consulted — see
[`../w32/PROVENANCE.md`](../w32/PROVENANCE.md) and
[`../w32/LICENSING.md`](../w32/LICENSING.md).

## Building and running a Win32 program

Install `mingw-w64` on the build host, then:

    make w32-sdk                  # stages build/w32-sdk
    cd build/w32-sdk/examples/console-app
    make                          # produces hello.exe

Copy the `.exe` into the initrd and, on AuraLite:

    run hello.exe

The shell recognises a PE image by its magic number. A `.exe` that imports
Win32 DLLs is routed through `/apps/w32run`, which maps it, binds the
imports, runs the TLS callbacks and static initialisers, and enters it. An
`.exe` with **no** imports is spawned directly and taken by the kernel's PE
loader instead, because that path applies per-section W^X and is the
hardened one.

`make w32-sdk-check` builds every example against the staged SDK, the same
way `make sdk-check` does for the native SDK.

## The supported function table

<!-- BEGIN GENERATED: w32 export table -->

*44 functions across 3 modules. This table is generated from
`w32/src/w32_bind.c` by `tools/gen_w32_api_table.py`; edit the export table, not this list.*

**GDI32.dll** (7)

- `CreateSolidBrush` · `DeleteObject` · `LineTo`
- `MoveToEx` · `SetPixel` · `SetTextColor`
- `TextOutA`

**KERNEL32.dll** (20)

- `CloseHandle` · `CreateFileA` · `ExitProcess`
- `FreeLibrary` · `GetCommandLineA` · `GetLastError`
- `GetModuleHandleA` · `GetProcAddress` · `GetProcessHeap`
- `GetStdHandle` · `GetTickCount64` · `HeapAlloc`
- `HeapFree` · `LoadLibraryA` · `ReadFile`
- `SetLastError` · `Sleep` · `VirtualAlloc`
- `VirtualFree` · `WriteFile`

**USER32.dll** (17)

- `BeginPaint` · `CreateWindowExA` · `DefWindowProcA`
- `DestroyWindow` · `DispatchMessageA` · `EndPaint`
- `FillRect` · `GetClientRect` · `GetMessageA`
- `InvalidateRect` · `MessageBoxA` · `PeekMessageA`
- `PostQuitMessage` · `RegisterClassExA` · `ShowWindow`
- `TranslateMessage` · `UpdateWindow`

<!-- END GENERATED: w32 export table -->

A function absent from this list is absent from the personality: a binary
importing it fails at load with the name reported, rather than at the first
call. That is deliberate — see "one bounded import set" (decision D7).

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

## Dynamic loading

`LoadLibraryA`, `GetProcAddress` and `FreeLibrary` work, for both the
built-in modules (`kernel32`, `user32`, `gdi32` — which are not files, but
functions linked into the loader) and for real user-supplied PE DLLs, which
are mapped, relocated, import-bound, and entered through `DllMain`.

Modules are reference-counted, so loading the same path twice shares one
mapping rather than giving a program two copies of the DLL's state.
`HMODULE`s are minted from a table and are not mapped addresses, so a
fabricated handle is refused instead of dereferenced.

Refused explicitly, rather than half-supported:

- **Forwarder exports.** An export whose RVA points back into the export
  directory is a string naming another DLL. Returning that address would
  hand the caller a pointer to text they would then call. The refusal is per
  symbol — other exports of the same DLL still resolve.
- **Delay-load imports.** The directory is detected and the load refused.
- **Imports by ordinal**, matching the policy the static binder already
  applies.
- **`.exe` files.** `IMAGE_FILE_DLL` must be set; loading an executable
  would run its entry point under `DllMain`'s contract.
- **Images with relocations stripped** that cannot be placed at their
  preferred base. Note that an *empty* relocation table is fine — a fully
  position-independent DLL legitimately needs no fixups.

A DLL whose `DllMain` returns FALSE fails to load and its mapping is torn
down, rather than leaving a module a program believes it loaded.

**A loaded DLL can only import from the built-in modules.** Its import table
is bound against the same static export table the main image uses, so one
DLL cannot import from another DLL. A dependency chain of two user DLLs is
therefore not loadable — and, as a side effect, a two-DLL import cycle
cannot arise. Supporting it needs a recursive load with an in-progress set,
which is future work rather than a hidden bug.

## Not implemented at all

Registry, COM, .NET, DirectX, WinSock, WOW64 (32-bit programs), and
`BitBlt`/off-screen device contexts. The `W` (UTF-16) entry points exist for `KERNEL32` where the plan
required them and are otherwise deferred; `A` entry points are the primary
surface today, which is the reverse of decision D6 and is noted there.
