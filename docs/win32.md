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

*697 functions across 6 modules. This table is generated from
`w32/src/w32_bind.c` by `tools/gen_w32_api_table.py`; edit the export table, not this list.*

**ADVAPI32.dll** (46)

- `AdjustTokenPrivileges` · `AllocateAndInitializeSid` · `CheckTokenMembership`
- `CopySid` · `CryptAcquireContextW` · `CryptCreateHash`
- `CryptDestroyHash` · `CryptGetHashParam` · `CryptHashData`
- `CryptReleaseContext` · `EqualSid` · `FreeSid`
- `GetFileSecurityW` · `GetLengthSid` · `GetUserNameA`
- `GetUserNameW` · `InitializeSecurityDescriptor` · `IsTextUnicode`
- `LookupAccountNameW` · `LookupPrivilegeValueW` · `LsaAddAccountRights`
- `LsaClose` · `LsaOpenPolicy` · `OpenProcessToken`
- `RegCloseKey` · `RegCreateKeyExA` · `RegCreateKeyExW`
- `RegDeleteKeyA` · `RegDeleteKeyExW` · `RegDeleteKeyW`
- `RegDeleteValueW` · `RegEnumKeyA` · `RegEnumKeyExW`
- `RegFlushKey` · `RegGetValueW` · `RegOpenKeyExA`
- `RegOpenKeyExW` · `RegQueryInfoKeyW` · `RegQueryValueExA`
- `RegQueryValueExW` · `RegSetValueExA` · `RegSetValueExW`
- `SetFileSecurityW` · `SetSecurityDescriptorDacl` · `SetSecurityDescriptorOwner`
- `SystemFunction036`

**COMCTL32.dll** (27)

- `CreateStatusWindowW` · `CreateToolbarEx` · `DefSubclassProc`
- `GetWindowSubclass` · `ImageList_AddMasked` · `ImageList_BeginDrag`
- `ImageList_Create` · `ImageList_Destroy` · `ImageList_DragEnter`
- `ImageList_DragMove` · `ImageList_DragShowNolock` · `ImageList_Draw`
- `ImageList_EndDrag` · `ImageList_GetIcon` · `ImageList_GetIconSize`
- `ImageList_GetImageCount` · `ImageList_GetImageInfo` · `ImageList_Remove`
- `ImageList_ReplaceIcon` · `ImageList_SetIconSize` · `InitCommonControls`
- `InitCommonControlsEx` · `LoadIconWithScaleDown` · `PropertySheetW`
- `RemoveWindowSubclass` · `SetWindowSubclass` · `_TrackMouseEvent`

**GDI32.dll** (87)

- `BitBlt` · `CombineRgn` · `CreateBitmap`
- `CreateCompatibleBitmap` · `CreateCompatibleDC` · `CreateDIBSection`
- `CreateFontA` · `CreateFontIndirectA` · `CreateFontIndirectW`
- `CreateFontW` · `CreateHatchBrush` · `CreatePalette`
- `CreatePatternBrush` · `CreatePen` · `CreateRectRgn`
- `CreateRectRgnIndirect` · `CreateSolidBrush` · `DPtoLP`
- `DeleteDC` · `DeleteObject` · `Ellipse`
- `EndDoc` · `EndPage` · `EnumFontFamiliesExW`
- `ExcludeClipRect` · `ExtCreatePen` · `ExtTextOutA`
- `ExtTextOutW` · `GdiAlphaBlend` · `GetBkMode`
- `GetCharABCWidthsFloatA` · `GetCharWidth32A` · `GetCharWidth32W`
- `GetCharWidthA` · `GetCharWidthW` · `GetCharacterPlacementW`
- `GetClipRgn` · `GetCurrentObject` · `GetDIBits`
- `GetDeviceCaps` · `GetObjectA` · `GetObjectW`
- `GetOutlineTextMetricsA` · `GetPixel` · `GetROP2`
- `GetStockObject` · `GetTextExtentExPointA` · `GetTextExtentExPointW`
- `GetTextExtentPoint32A` · `GetTextExtentPoint32W` · `GetTextExtentPointA`
- `GetTextExtentPointW` · `GetTextMetricsA` · `GetTextMetricsW`
- `IntersectClipRect` · `LineTo` · `MoveToEx`
- `OffsetWindowOrgEx` · `PatBlt` · `Polygon`
- `Polyline` · `RealizePalette` · `RectVisible`
- `Rectangle` · `RestoreDC` · `RoundRect`
- `SaveDC` · `SelectClipRgn` · `SelectObject`
- `SelectPalette` · `SetBkColor` · `SetBkMode`
- `SetBrushOrgEx` · `SetDIBits` · `SetMapMode`
- `SetPaletteEntries` · `SetPixel` · `SetROP2`
- `SetTextAlign` · `SetTextColor` · `SetWindowOrgEx`
- `StartDocW` · `StartPage` · `TextOutA`
- `TranslateCharsetInfo` · `UnrealizeObject` · `UpdateColors`

**KERNEL32.dll** (265)

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
- `EnterCriticalSection` · `EnumResourceNamesA` · `EnumResourceNamesW`
- `EnumSystemLocalesW` · `ExitProcess` · `ExitThread`
- `ExpandEnvironmentStringsW` · `FileTimeToDosDateTime` · `FileTimeToLocalFileTime`
- `FileTimeToSystemTime` · `FindClose` · `FindCloseChangeNotification`
- `FindFirstChangeNotificationW` · `FindFirstFileA` · `FindFirstFileExW`
- `FindFirstFileW` · `FindFirstStreamW` · `FindNextChangeNotification`
- `FindNextFileA` · `FindNextFileW` · `FindNextStreamW`
- `FindResourceA` · `FindResourceExA` · `FindResourceExW`
- `FindResourceW` · `FlsAlloc` · `FlsFree`
- `FlsGetValue` · `FlsSetValue` · `FlushFileBuffers`
- `FormatMessageA` · `FormatMessageW` · `FreeEnvironmentStringsW`
- `FreeLibrary` · `FreeLibraryAndExitThread` · `FreeResource`
- `GetACP` · `GetApplicationRestartSettings` · `GetCPInfo`
- `GetCommandLineA` · `GetCommandLineW` · `GetCompressedFileSizeW`
- `GetCurrentDirectoryA` · `GetCurrentDirectoryW` · `GetCurrentProcess`
- `GetCurrentProcessId` · `GetCurrentThread` · `GetCurrentThreadId`
- `GetDateFormatEx` · `GetDateFormatW` · `GetDiskFreeSpaceExW`
- `GetDiskFreeSpaceW` · `GetDriveTypeW` · `GetEnvironmentStringsW`
- `GetEnvironmentVariableA` · `GetExitCodeProcess` · `GetExitCodeThread`
- `GetFileAttributesExW` · `GetFileAttributesW` · `GetFileInformationByHandle`
- `GetFileSize` · `GetFileSizeEx` · `GetFileType`
- `GetFinalPathNameByHandleW` · `GetFullPathNameW` · `GetLargePageMinimum`
- `GetLastError` · `GetLocalTime` · `GetLocaleInfoA`
- `GetLocaleInfoEx` · `GetLocaleInfoW` · `GetLogicalDriveStringsW`
- `GetLongPathNameW` · `GetModuleFileNameA` · `GetModuleFileNameW`
- `GetModuleHandleA` · `GetModuleHandleExW` · `GetModuleHandleW`
- `GetNativeSystemInfo` · `GetOEMCP` · `GetOverlappedResult`
- `GetProcAddress` · `GetProcessAffinityMask` · `GetProcessHeap`
- `GetProcessTimes` · `GetProductInfo` · `GetStartupInfoA`
- `GetStartupInfoW` · `GetStdHandle` · `GetStringTypeExA`
- `GetStringTypeExW` · `GetStringTypeW` · `GetSystemDefaultLangID`
- `GetSystemDirectoryA` · `GetSystemInfo` · `GetSystemTimeAsFileTime`
- `GetTempPathA` · `GetTempPathW` · `GetThreadId`
- `GetThreadTimes` · `GetTickCount` · `GetTickCount64`
- `GetTimeFormatEx` · `GetTimeFormatW` · `GetTimeZoneInformation`
- `GetUserDefaultLCID` · `GetUserDefaultLangID` · `GetVersion`
- `GetVersionExW` · `GetVolumeInformationW` · `GetWindowsDirectoryA`
- `GetWindowsDirectoryW` · `GlobalAlloc` · `GlobalFree`
- `GlobalLock` · `GlobalMemoryStatus` · `GlobalMemoryStatusEx`
- `GlobalSize` · `GlobalUnlock` · `HeapAlloc`
- `HeapFree` · `HeapReAlloc` · `HeapSize`
- `InitOnceBeginInitialize` · `InitOnceComplete` · `InitializeCriticalSection`
- `InitializeCriticalSectionAndSpinCount` · `InitializeCriticalSectionEx` · `InitializeSListHead`
- `InterlockedFlushSList` · `IsDBCSLeadByteEx` · `IsDebuggerPresent`
- `IsProcessorFeaturePresent` · `IsValidCodePage` · `IsValidLocale`
- `LCMapStringA` · `LCMapStringEx` · `LCMapStringW`
- `LeaveCriticalSection` · `LoadLibraryA` · `LoadLibraryExA`
- `LoadLibraryExW` · `LoadLibraryW` · `LoadResource`
- `LoadStringA` · `LoadStringW` · `LocalAlloc`
- `LocalFileTimeToFileTime` · `LocalFree` · `LockResource`
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
- `SizeofResource` · `Sleep` · `SleepConditionVariableSRW`
- `SleepEx` · `SubmitThreadpoolWork` · `SystemTimeToTzSpecificLocalTime`
- `TerminateProcess` · `TerminateThread` · `TlsAlloc`
- `TlsFree` · `TlsGetValue` · `TlsSetValue`
- `TryAcquireSRWLockExclusive` · `TryEnterCriticalSection` · `UnhandledExceptionFilter`
- `UnmapViewOfFile` · `UnregisterApplicationRestart` · `VirtualAlloc`
- `VirtualFree` · `VirtualProtect` · `WaitForMultipleObjects`
- `WaitForSingleObject` · `WaitForSingleObjectEx` · `WaitNamedPipeA`
- `WakeAllConditionVariable` · `WideCharToMultiByte` · `WriteFile`
- `_XcptFilter` · `__C_specific_handler` · `lstrcatW`
- `lstrcmpW` · `lstrcmpiA` · `lstrcmpiW`
- `lstrcpyW` · `lstrcpynA` · `lstrcpynW`
- `lstrlenW`

**USER32.dll** (267)

- `AdjustWindowRectEx` · `AppendMenuA` · `AppendMenuW`
- `BeginPaint` · `BringWindowToTop` · `CallNextHookEx`
- `CallWindowProcW` · `ChangeClipboardChain` · `CharLowerW`
- `CharUpperW` · `CheckDlgButton` · `CheckMenuItem`
- `CheckRadioButton` · `ChildWindowFromPointEx` · `ClientToScreen`
- `CloseClipboard` · `CopyAcceleratorTableW` · `CountClipboardFormats`
- `CreateAcceleratorTableW` · `CreateCaret` · `CreateMenu`
- `CreatePopupMenu` · `CreateWindowExA` · `CreateWindowExW`
- `DefWindowProcA` · `DefWindowProcW` · `DeleteMenu`
- `DestroyAcceleratorTable` · `DestroyCaret` · `DestroyCursor`
- `DestroyIcon` · `DestroyMenu` · `DestroyWindow`
- `DialogBoxA` · `DialogBoxIndirectParamA` · `DialogBoxIndirectParamW`
- `DialogBoxParamA` · `DialogBoxParamW` · `DialogBoxW`
- `DispatchMessageA` · `DispatchMessageW` · `DrawEdge`
- `DrawFocusRect` · `DrawFrameControl` · `DrawIcon`
- `DrawIconEx` · `DrawMenuBar` · `DrawTextA`
- `DrawTextExW` · `DrawTextW` · `EmptyClipboard`
- `EnableMenuItem` · `EnableWindow` · `EndDialog`
- `EndPaint` · `EnumChildWindows` · `EnumClipboardFormats`
- `EnumDisplayMonitors` · `EnumResourceNamesA` · `EnumResourceNamesW`
- `EnumThreadWindows` · `EqualRect` · `FillRect`
- `FindResourceA` · `FindResourceExA` · `FindResourceExW`
- `FindResourceW` · `FindWindowA` · `FindWindowExW`
- `FindWindowW` · `FlashWindow` · `FlashWindowEx`
- `FrameRect` · `FreeResource` · `GetActiveWindow`
- `GetAncestor` · `GetCapture` · `GetCaretBlinkTime`
- `GetCaretPos` · `GetClassInfoW` · `GetClassNameA`
- `GetClassNameW` · `GetClientRect` · `GetClipboardData`
- `GetClipboardOwner` · `GetClipboardViewer` · `GetCursorPos`
- `GetDC` · `GetDCEx` · `GetDesktopWindow`
- `GetDialogBaseUnits` · `GetDlgCtrlID` · `GetDlgItem`
- `GetDlgItemInt` · `GetDlgItemTextA` · `GetDlgItemTextW`
- `GetDoubleClickTime` · `GetFocus` · `GetForegroundWindow`
- `GetKeyState` · `GetKeyboardLayout` · `GetKeyboardState`
- `GetKeyboardType` · `GetLastActivePopup` · `GetMenu`
- `GetMenuBarInfo` · `GetMenuItemCount` · `GetMenuItemID`
- `GetMessageA` · `GetMessagePos` · `GetMessageTime`
- `GetMessageW` · `GetMonitorInfoA` · `GetMonitorInfoW`
- `GetOpenClipboardWindow` · `GetParent` · `GetPropW`
- `GetQueueStatus` · `GetScrollInfo` · `GetScrollPos`
- `GetScrollRange` · `GetShellWindow` · `GetSubMenu`
- `GetSysColor` · `GetSysColorBrush` · `GetSystemMenu`
- `GetSystemMetrics` · `GetUpdateRgn` · `GetWindow`
- `GetWindowDC` · `GetWindowLongPtrA` · `GetWindowLongPtrW`
- `GetWindowLongW` · `GetWindowPlacement` · `GetWindowRect`
- `GetWindowTextA` · `GetWindowTextLengthA` · `GetWindowTextLengthW`
- `GetWindowTextW` · `GetWindowThreadProcessId` · `HideCaret`
- `InSendMessage` · `InflateRect` · `InsertMenuA`
- `InsertMenuW` · `IntersectRect` · `InvalidateRect`
- `IsCharAlphaNumericW` · `IsCharAlphaW` · `IsCharLowerW`
- `IsCharUpperW` · `IsChild` · `IsClipboardFormatAvailable`
- `IsDialogMessageA` · `IsDialogMessageW` · `IsDlgButtonChecked`
- `IsIconic` · `IsRectEmpty` · `IsWindow`
- `IsWindowEnabled` · `IsWindowVisible` · `IsZoomed`
- `KillTimer` · `LoadAcceleratorsA` · `LoadAcceleratorsW`
- `LoadCursorA` · `LoadCursorW` · `LoadIconA`
- `LoadIconW` · `LoadImageA` · `LoadImageW`
- `LoadMenuA` · `LoadMenuW` · `LoadResource`
- `LoadStringA` · `LoadStringW` · `LockResource`
- `LockWindowUpdate` · `MapDialogRect` · `MapVirtualKeyW`
- `MapWindowPoints` · `MessageBoxA` · `MessageBoxW`
- `MonitorFromPoint` · `MonitorFromRect` · `MonitorFromWindow`
- `MoveWindow` · `MsgWaitForMultipleObjects` · `NotifyWinEvent`
- `OffsetRect` · `OpenClipboard` · `PeekMessageA`
- `PeekMessageW` · `PostMessageA` · `PostMessageW`
- `PostQuitMessage` · `PtInRect` · `RedrawWindow`
- `RegisterClassA` · `RegisterClassExA` · `RegisterClassExW`
- `RegisterClassW` · `RegisterClipboardFormatA` · `RegisterClipboardFormatW`
- `RegisterWindowMessageA` · `RegisterWindowMessageW` · `ReleaseCapture`
- `ReleaseDC` · `RemoveMenu` · `RemovePropW`
- `ReplyMessage` · `ScreenToClient` · `ScrollWindow`
- `SendDlgItemMessageA` · `SendDlgItemMessageW` · `SendMessageA`
- `SendMessageW` · `SetActiveWindow` · `SetCapture`
- `SetCaretPos` · `SetClipboardData` · `SetClipboardViewer`
- `SetCursorPos` · `SetDlgItemInt` · `SetDlgItemTextA`
- `SetDlgItemTextW` · `SetFocus` · `SetForegroundWindow`
- `SetKeyboardState` · `SetLayeredWindowAttributes` · `SetMenu`
- `SetParent` · `SetPropW` · `SetRectEmpty`
- `SetScrollInfo` · `SetScrollPos` · `SetScrollRange`
- `SetTimer` · `SetWindowLongPtrA` · `SetWindowLongPtrW`
- `SetWindowLongW` · `SetWindowPlacement` · `SetWindowPos`
- `SetWindowTextA` · `SetWindowTextW` · `SetWindowsHookExA`
- `SetWindowsHookExW` · `ShowCaret` · `ShowScrollBar`
- `ShowWindow` · `SizeofResource` · `SystemParametersInfoA`
- `SystemParametersInfoW` · `ToAscii` · `ToAsciiEx`
- `TrackMouseEvent` · `TrackPopupMenu` · `TrackPopupMenuEx`
- `TranslateAcceleratorA` · `TranslateAcceleratorW` · `TranslateMessage`
- `UnhookWindowsHookEx` · `UnregisterClassW` · `UpdateWindow`
- `ValidateRect` · `WindowFromPoint` · `mouse_event`

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

## The registry is one file, `W32HIVE1` (W32A-9)

`ADVAPI32`'s `Reg*` surface is a real engine over one hive file, in a
format that is ours and this section is its specification.

**Format** (`W32HIVE1`, little-endian): a 28-byte header — offset 0
magic `"W32HIVE1"` (8 bytes), 8 flags `u32` (0 today), 12 sequence
`u64` (incremented per save — a stale-tail reader can tell), 20
payload length `u32`, 24 payload CRC32 — followed by the payload:
`u32` root count (always 2: HKCU then HKLM), then node records
`{ u16 name_len, name UTF-16LE, u64 mtime (FILETIME), u32 value_count,
[ u16 value_name_len, value_name, u32 type, u32 data_len, data ]×,
u32 sub_count, sub_count× child records }`. Writes are whole-file
(open + write + `fsync`), never in place, so a torn write can only
shorten or corrupt the file — never present itself as valid data: the
load path validates magic, length, and CRC over exactly `payload_len`,
and any failure latches *hive corrupt* so every subsequent `Reg*` call
answers `ERROR_FILE_CORRUPT` (1392) until the process exits. A
zero-byte file means *fresh hive*, not corruption — the distinction
matters after a first-boot `O_CREAT`. There is no `O_TRUNC` on save:
the header's `payload_len` bounds the parse, so a stale tail after a
shrinking write is ignored by construction.

**Location:** `/disk/w32hive` when `/disk` is mounted (the diskfs
scratch disk — settings survive a reboot), else `/tmp/w32hive` with
the volatility logged once at first use. A host test seam
(`w32_advapi_hive_override`) pins the engine without touching either.

**Policies.** `HKEY_CURRENT_USER` is a real, freely writable root.
`HKEY_LOCAL_MACHINE` is read-mostly: only subtrees under
`HKLM\Software` are writable; creates, value writes, and deletes
anywhere else answer `ERROR_ACCESS_DENIED`. `HKEY_CLASSES_ROOT` is a
merge view of `HKCU\Software\Classes` over `HKLM\Software\Classes`
— HKCU wins on conflicts, merged enumeration counts both sides, and
writes through HKCR land in the HKCU half. Key-name matching folds
ASCII case only (the documented locale-free contract). Keys with
subkeys refuse `RegDeleteKey`/`RegDeleteKeyEx` with
`ERROR_ACCESS_DENIED` — there is no recursive delete in `ADVAPI32`
(the real API's contract, kept). `RegCloseKey` on a predefined key is
a successful no-op. The seed: `HKLM\Software\AuraLite\CurrentVersion`
carries `ProductName` "AuraLite OS (w32 personality)", `HiveFormat`
"W32HIVE1", `CurrentVersion` "0.0.1".

`RegGetValueW` (the ledger's spelling; there is no A variant) coerces
within `RRF_RT_REG_SZ|BINARY|DWORD`: `REG_EXPAND_SZ` read as `RT_SZ`
without `RRF_NOEXPAND` is `%ENV%`-expanded through `getenv` and
reported as `REG_SZ` with byte size including the NUL;
`NOEXPAND`+`EXPAND_SZ`+`RT_SZ` is `ERROR_INVALID_PARAMETER`; a type
mismatch answers `ERROR_UNSUPPORTED_TYPE`. `RegEnumValue` is not in
any ledger and is not exported. `RegFlushKey` is REAL — `fsync`
exists, so it is observable, not ceremonial.

**Limits** (documented, not discovered): key name 255, value name
16383, value data 1 MiB, 512 subkeys, 1024 values per key, depth 32,
path 1024 UTF-16 units, 64 open key handles, 16 providers, 64 hashes,
4 MiB hash input.

## SIDs, tokens, and the single-user model (W32A-9)

`AllocateAndInitializeSid`/`CopySid`/`EqualSid`/`GetLengthSid`/`FreeSid`
are a real, self-consistent SID implementation (revision 1, up to 8
subauthorities). `GetUserNameA`/`W` answer `"user"` — the documented
single-user name; there is no account database to ask, and the short
buffer answers `FALSE` + needed length + `ERROR_INSUFFICIENT_BUFFER`
like the real thing.

`CheckTokenMembership` is TRUE for exactly the caller's own SID
(`S-1-5-21-0-0-1000`) and `BUILTIN\Administrators`
(`S-1-5-32-544`), FALSE for anything else. Administrators being TRUE
is the one place the plan sanctions "admin", and the reason is one
paragraph: every install and settings write in the ladder's binaries
asks "am I elevated?" once and then proceeds; a FALSE here would turn
every first-run of PuTTY, 7-Zip, and Notepad++ into a UAC-style
failure loop with nothing behind it. There is no ACL engine, no
token, and no privilege model to protect — membership is an answer,
not an enforcement claim.

`InitializeSecurityDescriptor`/`SetSecurityDescriptorDacl`/
`SetSecurityDescriptorOwner` build real descriptors in the x64
natural layout (revision@0, control@2, owner@8, group@16, sacl@24,
dacl@32, sizeof 40) with the `SE_*` control bits set honestly.
Enforcement is owner-only and documented as such;
`Get/SetFileSecurityW` answer `ERROR_NOT_SUPPORTED` (below).

`IsTextUnicode` is the base kernel32 engine (kernel32_loc.c), bound
under `ADVAPI32` — the real DLL-forwarder shape. `*result` is the
in-mask of tests to run (the winnls.h values; NULL flags = all
tests); the documented STATISTICS heuristic is the null-high-byte
count, not a locale table.

## CryptoAPI is hash-only (W32A-9)

`CryptAcquireContextW`/`CryptReleaseContext`/
`CryptCreateHash`/`CryptHashData`/`CryptGetHashParam`/
`CryptDestroyHash` are REAL over libatls: `CALG_SHA_256`, `CALG_SHA_512`,
`CALG_SHA3_256` (0x8025), `CALG_SHA3_512` (0x8026), input buffered
across `CryptHashData` calls (ragged feed == one-shot is asserted
against the public "abc" vectors in both gates), digest read at
`HP_HASHVAL`, `HP_HASHSIZE`/`HP_ALGID` answer, provider release frees
its hashes. `CALG_SHA1`, `CALG_MD5`, and `CALG_SHA_384` answer FALSE
with `NTE_BAD_ALGID`: no ladder receipt shows SHA-1/MD5 in a core
flow, and libatls ships neither — the phase result the plan asked
for. There is no `CryptDeriveKey`/`CryptEncrypt`/`CryptDecrypt` (not
in any ledger). `SystemFunction036` (RtlGenRandom) is REAL via the
`getrandom` syscall.

## Not implemented at all

COM, .NET, DirectX, WinSock, and WOW64 (32-bit programs).
`BitBlt`/off-screen device contexts shipped in W32A-7 (the GDI raster
engine); the registry shipped in W32A-9 (the `W32HIVE1` section
above); printing (`StartDocW`/`StartPage`/`EndPage`/`EndDoc`) is
fail-clean by design — no printers exist, so the calls report
`SP_ERROR`/`ERROR_CALL_NOT_IMPLEMENTED` instead of half-working. The `W` (UTF-16) entry points exist for `KERNEL32` where the plan
required them and are otherwise deferred; `A` entry points are the primary
surface today, which is the reverse of decision D6 and is noted there.

The `ADVAPI32` FAIL-CLEAN set, each with one documented reason:
`LsaOpenPolicy`/`LsaAddAccountRights` → `STATUS_ACCESS_DENIED` (no
LSA); `LsaClose` → `STATUS_INVALID_HANDLE`; `LookupAccountNameW` →
`FALSE` + `ERROR_NONE_MAPPED` (no account database);
`LookupPrivilegeValueW` → `FALSE` + `ERROR_NO_SUCH_PRIVILEGE` (no
privilege model); `OpenProcessToken` → `FALSE` +
`ERROR_NO_TOKEN`; `AdjustTokenPrivileges` → `TRUE` +
`ERROR_NOT_ALL_ASSIGNED` (the single-user degradation 7-Zip's backup
path takes, asserted in the gate); `Get/SetFileSecurityW` → `FALSE` +
`ERROR_NOT_SUPPORTED` (archive ACL preservation is the named
casualty, recorded in the W32A-15 receipt expectations).
