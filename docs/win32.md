# Win32 personality — limitations and behaviour notes

**Status:** complete. Phases W32-0 – W32-8 of
[`WIN32_PLAN.md`](plans/WIN32_PLAN.md) are implemented. A mingw-w64-built
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

*280 functions across 4 modules. This table is generated from
`w32/src/w32_bind.c` by `tools/gen_w32_api_table.py`; edit the export table, not this list.*

**GDI32.dll** (7)

- `CreateSolidBrush` · `DeleteObject` · `LineTo`
- `MoveToEx` · `SetPixel` · `SetTextColor`
- `TextOutA`

**KERNEL32.dll** (251)

- `AcquireSRWLockExclusive` · `Beep` · `CancelIo`
- `CloseHandle` · `CloseThreadpoolWork` · `CompareFileTime`
- `CompareStringEx` · `CompareStringW` · `ConnectNamedPipe`
- `CopyFileExW` · `CopyFileW` · `CreateDirectoryW`
- `CreateEventA` · `CreateEventW` · `CreateFileA`
- `CreateFileMappingA` · `CreateFileMappingW` · `CreateFileW`
- `CreateHardLinkW` · `CreateMutexA` · `CreateMutexW`
- `CreateNamedPipeA` · `CreatePipe` · `CreateProcessA`
- `CreateProcessW` · `CreateSemaphoreW` · `CreateThread`
- `CreateThreadpoolWork` · `CreateToolhelp32Snapshot` · `DecodePointer`
- `DeleteCriticalSection` · `DeleteFileA` · `DeleteFileW`
- `DeviceIoControl` · `DosDateTimeToFileTime` · `EncodePointer`
- `EnterCriticalSection` · `EnumSystemLocalesW` · `ExitProcess`
- `ExitThread` · `ExpandEnvironmentStringsW` · `FileTimeToDosDateTime`
- `FileTimeToLocalFileTime` · `FileTimeToSystemTime` · `FindClose`
- `FindCloseChangeNotification` · `FindFirstChangeNotificationW` · `FindFirstFileA`
- `FindFirstFileExW` · `FindFirstFileW` · `FindFirstStreamW`
- `FindNextChangeNotification` · `FindNextFileA` · `FindNextFileW`
- `FindNextStreamW` · `FlsAlloc` · `FlsFree`
- `FlsGetValue` · `FlsSetValue` · `FlushFileBuffers`
- `FormatMessageA` · `FormatMessageW` · `FreeEnvironmentStringsW`
- `FreeLibrary` · `FreeLibraryAndExitThread` · `GetACP`
- `GetApplicationRestartSettings` · `GetCPInfo` · `GetCommandLineA`
- `GetCommandLineW` · `GetCompressedFileSizeW` · `GetCurrentDirectoryA`
- `GetCurrentDirectoryW` · `GetCurrentProcess` · `GetCurrentProcessId`
- `GetCurrentThread` · `GetCurrentThreadId` · `GetDateFormatEx`
- `GetDateFormatW` · `GetDiskFreeSpaceExW` · `GetDiskFreeSpaceW`
- `GetDriveTypeW` · `GetEnvironmentStringsW` · `GetEnvironmentVariableA`
- `GetExitCodeProcess` · `GetExitCodeThread` · `GetFileAttributesExW`
- `GetFileAttributesW` · `GetFileInformationByHandle` · `GetFileSize`
- `GetFileSizeEx` · `GetFileType` · `GetFinalPathNameByHandleW`
- `GetFullPathNameW` · `GetLargePageMinimum` · `GetLastError`
- `GetLocalTime` · `GetLocaleInfoA` · `GetLocaleInfoEx`
- `GetLocaleInfoW` · `GetLogicalDriveStringsW` · `GetLongPathNameW`
- `GetModuleFileNameA` · `GetModuleFileNameW` · `GetModuleHandleA`
- `GetModuleHandleExW` · `GetModuleHandleW` · `GetNativeSystemInfo`
- `GetOEMCP` · `GetOverlappedResult` · `GetProcAddress`
- `GetProcessAffinityMask` · `GetProcessHeap` · `GetProcessTimes`
- `GetProductInfo` · `GetStartupInfoA` · `GetStartupInfoW`
- `GetStdHandle` · `GetStringTypeExA` · `GetStringTypeExW`
- `GetStringTypeW` · `GetSystemDefaultLangID` · `GetSystemDirectoryA`
- `GetSystemInfo` · `GetSystemTimeAsFileTime` · `GetTempPathA`
- `GetTempPathW` · `GetThreadTimes` · `GetTickCount`
- `GetTickCount64` · `GetTimeFormatEx` · `GetTimeFormatW`
- `GetTimeZoneInformation` · `GetUserDefaultLCID` · `GetUserDefaultLangID`
- `GetVersion` · `GetVersionExW` · `GetVolumeInformationW`
- `GetWindowsDirectoryA` · `GetWindowsDirectoryW` · `GlobalAlloc`
- `GlobalFree` · `GlobalLock` · `GlobalMemoryStatus`
- `GlobalMemoryStatusEx` · `GlobalSize` · `GlobalUnlock`
- `HeapAlloc` · `HeapFree` · `HeapReAlloc`
- `HeapSize` · `InitOnceBeginInitialize` · `InitOnceComplete`
- `InitializeCriticalSection` · `InitializeCriticalSectionAndSpinCount` · `InitializeCriticalSectionEx`
- `InitializeSListHead` · `InterlockedFlushSList` · `IsDBCSLeadByteEx`
- `IsDebuggerPresent` · `IsProcessorFeaturePresent` · `IsValidCodePage`
- `IsValidLocale` · `LCMapStringA` · `LCMapStringEx`
- `LCMapStringW` · `LeaveCriticalSection` · `LoadLibraryA`
- `LoadLibraryExA` · `LoadLibraryExW` · `LoadLibraryW`
- `LocalAlloc` · `LocalFileTimeToFileTime` · `LocalFree`
- `MapViewOfFile` · `MoveFileExW` · `MoveFileW`
- `MoveFileWithProgressW` · `MulDiv` · `MultiByteToWideChar`
- `OpenProcess` · `OutputDebugStringW` · `Process32FirstW`
- `Process32NextW` · `QueryPerformanceCounter` · `QueryPerformanceFrequency`
- `QueueUserAPC` · `RaiseException` · `ReadFile`
- `RegisterApplicationRestart` · `ReleaseMutex` · `ReleaseSRWLockExclusive`
- `ReleaseSemaphore` · `RemoveDirectoryW` · `ReplaceFileW`
- `ResetEvent` · `ResumeThread` · `RtlCaptureContext`
- `RtlLookupFunctionEntry` · `RtlPcToFileHeader` · `RtlUnwind`
- `RtlUnwindEx` · `RtlVirtualUnwind` · `SetCurrentDirectoryA`
- `SetCurrentDirectoryW` · `SetEndOfFile` · `SetEnvironmentVariableW`
- `SetEvent` · `SetFileAttributesW` · `SetFilePointer`
- `SetFilePointerEx` · `SetFileTime` · `SetHandleInformation`
- `SetLastError` · `SetThreadAffinityMask` · `SetUnhandledExceptionFilter`
- `Sleep` · `SleepConditionVariableSRW` · `SleepEx`
- `SubmitThreadpoolWork` · `SystemTimeToTzSpecificLocalTime` · `TerminateProcess`
- `TerminateThread` · `TlsAlloc` · `TlsFree`
- `TlsGetValue` · `TlsSetValue` · `TryAcquireSRWLockExclusive`
- `UnhandledExceptionFilter` · `UnmapViewOfFile` · `UnregisterApplicationRestart`
- `VirtualAlloc` · `VirtualFree` · `VirtualProtect`
- `WaitForMultipleObjects` · `WaitForSingleObject` · `WaitForSingleObjectEx`
- `WaitNamedPipeA` · `WakeAllConditionVariable` · `WideCharToMultiByte`
- `WriteFile` · `_XcptFilter` · `__C_specific_handler`
- `lstrcatW` · `lstrcmpW` · `lstrcmpiA`
- `lstrcmpiW` · `lstrcpyW` · `lstrcpynA`
- `lstrcpynW` · `lstrlenW`

**USER32.dll** (17)

- `BeginPaint` · `CreateWindowExA` · `DefWindowProcA`
- `DestroyWindow` · `DispatchMessageA` · `EndPaint`
- `FillRect` · `GetClientRect` · `GetMessageA`
- `InvalidateRect` · `MessageBoxA` · `PeekMessageA`
- `PostQuitMessage` · `RegisterClassExA` · `ShowWindow`
- `TranslateMessage` · `UpdateWindow`

**msvcrt.dll** (5)

- `?terminate@@YAXXZ` · `_CxxThrowException` · `_XcptFilter`
- `__C_specific_handler` · `_purecall`

<!-- END GENERATED: w32 export table -->

A function absent from this list is absent from the personality: a binary
importing it fails at load with the name reported, rather than at the first
call. That is deliberate — see "one bounded import set" (decision D7).

## Structured exception handling unwinds `.pdata`/`.xdata` for real

Faults and `RaiseException` dispatch through the image's own unwind tables,
frame by frame: `__try`/`__except` filters, `__finally` cleanups, C++
destructors (including through libgcc's `__gxx_personality_seh0`),
`EXCEPTION_CONTINUE_EXECUTION` with a mutable fault context, and the top
filter before the terminate box. The `sigsetjmp` shim is deleted — two
exception systems is how a fault gets handled twice.

The specified edges:

- **MSVC `catch` clauses are never matched.** `_CxxThrowException`
  sweeps real cleanups, then terminates with the named
  `W32-CXX-TYPED-CATCH-GAP` (decision D7): specified, tested, and
  greppable, not a crash shaped like a mystery. `?terminate@@YAXXZ`
  and `_purecall` die by their own names.
- **Unhandled faults dump and die honestly.** Console sessions print
  `W32-SEH-UNHANDLED` to serial and die on the signal the fault came
  in on; a process with a live window gets the modal terminate box
  first, then exits with the exception code.
- **v2 unwind info (epilog opcodes) and chained `UNWIND_INFO` are
  refused, never misread.** The toolchains in use emit v1 only; the
  equivalence gate fails loudly if that ever changes.

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
  symbol — other exports of the same DLL still resolve, and the refusal names the forwarder target.
- **Delay-load imports** are honoured: an in-guest helper resolves the
  target on first call through the thunk and patches it. A target that
  is itself absent fails at first call with the DLL named — the
  documented Windows behaviour, not a load refusal.
- **Imports by ordinal** bind through per-module ordinal→name maps
  (built from import data and documented ordinals); unknown ordinals
  refuse by number, and `#<n>` names refuse with the number named.
- **`.exe` files.** `IMAGE_FILE_DLL` must be set; loading an executable
  would run its entry point under `DllMain`'s contract.
- **Images with relocations stripped** that cannot be placed at their
  preferred base. Note that an *empty* relocation table is fine — a fully
  position-independent DLL legitimately needs no fixups.

A DLL whose `DllMain` returns FALSE fails to load and its mapping is torn
down, rather than leaving a module a program believes it loaded.

A loaded DLL may import from the built-in modules *and* from other
user DLLs. The load claims its table slot before mapping, so a nested
load cannot steal it; import cycles refuse by name, depth is capped,
`DllMain` runs dependencies-first (`DLL_PROCESS_ATTACH` order) and
teardown runs in reverse at `ExitProcess`. Exported RVAs that are data
bind as addresses the loader never calls, and `.rsrc` type-24
manifests are parsed: the Common-Controls identity selects comctl32,
`requestedExecutionLevel` honours asInvoker (requireAdministrator
refuses with the reason named), `dpiAware` is recorded, and
`supportedOS` GUIDs are logged, not actioned.

## Not implemented at all

Registry, COM, .NET, DirectX, WinSock, WOW64 (32-bit programs), and
`BitBlt`/off-screen device contexts. The `W` (UTF-16) entry points exist for `KERNEL32` where the plan
required them and are otherwise deferred; `A` entry points are the primary
surface today, which is the reverse of decision D6 and is noted there.
