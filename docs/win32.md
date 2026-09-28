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

## The shell is a real filesystem view plus real dialogs (W32A-10)

**Known folders** (`SHGetFolderPathW`/`SHGetSpecialFolderPathW`):
`CSIDL_DESKTOP` → `/`, `CSIDL_PROGRAMS`/`STARTMENU`/`STARTUP` → `/apps`,
`CSIDL_PERSONAL` → the data root, `CSIDL_APPDATA` → `<root>/w32/appdata`,
`CSIDL_FONTS` refused (the font is baked in). The data root mirrors the
hive: `/disk` when the scratch disk is mounted (settings survive a reboot),
else `/tmp` with the volatility logged once. `CSIDL_FLAG_CREATE` creates
the directory.

**PIDL** (`SHGetSpecialFolderLocation` + `SHGetPathFromIDListW`): one
item `{ u16 cb, u16 csidl, u16 path_units, UTF-16 path, u16 0 }`. The two
functions round-trip; nothing else accepts a PIDL (full PIDL algebra is
§7). `SHGetDesktopFolder` is `E_NOTIMPL`.

**IShellItem** (`SHCreateItemFromParsingName`): a minimal `IShellItem`
for filesystem paths; `GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING)`
returns the path, every other `SIGDN` is `E_INVALIDARG`.

**File info** (`SHGetFileInfoW`/`ExtractIconExW`): type names from the
extension map (`.exe` loadable PE → Application, `.txt` → Text Document,
directory → File Folder, else File), display names are the file-name
component, attributes are the stat bits, icons decode from PE resources
(`w32_gdi_icon_decode`; the generic document parchment is minted through
the A-8 ARGB seam when the file has no icons).

**File operations** (`SHFileOperationW`): copy/move/delete/rename over
the VFS through the W32A-2 file primitives, double-null lists, wildcard
`*`/`?` via `FindFirstFileW`, `FOF_ALLOWUNDO` refused with
`ERROR_CALL_NOT_IMPLEMENTED` (no recycle bin), progress is the
notification engine (`ag_notify`).

**Shell execution** (`ShellExecuteW`/`A`/`ExW`): verb `open` on a PE/ELF
executes via `CreateProcessW` and returns `>32`; every other verb
(`runas` → `SE_ERR_ACCESSDENIED`, `print`/`edit`/unknown → `SE_ERR_NOASSOC`)
is refused by name. Documents and directories refuse as `NOASSOC` (no
association table, desktop namespace is §7).

**Notifications** (`Shell_NotifyIconW`): 8 slots keyed `(hwnd, id)`;
`ADD` shows `ag_notify`, `MODIFY` updates, `DELETE` removes;
duplicate `ADD` → `ERROR_ALREADY_EXISTS`, unknown `MODIFY`/`DELETE` →
`ERROR_INVALID_PARAMETER`.

**Drag** (`DragQueryFileW`/`DragQueryPoint`/`DragFinish`): the older NULL
handle still reads one borrowed list of at most 16 paths. W32A-11 also
provides independently owned HDROP handles (16 slots × at most 16 copied
paths each) with generation-checked lifetime, points and `DragFinish`.
`DragQueryFileW` with `0xFFFFFFFF` returns a handle's file count; actual
file bytes can be read via the path returned. **No compositor event carries
a file path yet:** no GUI-driven `WM_DROPFILES` or window-to-window OLE drag
can be claimed from this in-process model.

**Common dialogs** (`GetOpenFileNameW`/`GetSaveFileNameW`,
`ChooseColorW`/`ChooseFontW`): real modal dialogs over the W32A-6 engine
(the listbox class `AuraCDlgLst` + edit/button classes, `DialogBoxIndirectParamW`
with the hook precedence `WM_INITDIALOG` first). `PrintDlgW` is
fail-clean: `PDERR_NODEFAULTPRN` (no printers). `CommDlgExtendedError`
carries `CDERR_*`/`FNERR_*`.

**Version** (`GetFileVersionInfoSizeW`/`GetFileVersionInfoW`/`VerQueryValueW`):
reads `RT_VERSION/1` (`VS_VERSION_INFO`) via `pe_find_resource`; the
engine blob is `[u16 units][resource][u16 pad]` (the prefix bounds the
walk, the Win32 API carries no length). `VerQueryValueW` walks the
documented tree (`wLength`/`wValueLength`/`wType`, 32-bit pads).

**Small modules**: `InternetCrackUrlW` (pure URL parser, scheme/host/port
defaults 21/80/443), `ImageNtHeader` (MZ + `e_lfanew` + `PE\0\0`),
`DwmGetColorizationColor` → `S_OK` + opaque black (composition off),
`DwmSetWindowAttribute` → `E_NOTIMPL`, `IsNetworkAlive`/`IsDestinationReachableW`
(TCP connect, never hardcoded), `WinVerifyTrust` → `TRUST_E_NOSIGNATURE`,
`CryptQueryObject`/`Cert*`/`CryptMsg*` → `CRYPT_E_NOT_FOUND`/`ERROR_INVALID_HANDLE`.

## W32A-11 incremental COM, IME and themed controls (not the full phase)

**COM-lite:** `CoInitialize`/`CoUninitialize` and
`OleInitialize`/`OleUninitialize` maintain balanced initialization counts
per Win32 thread; `OleInitialize` owns a `CoInitialize` depth. This is a
single **MTA-like** in-process model, **no apartments**, **no marshalling**,
**no class activation**. Bound `CoTaskMemAlloc`/`CoTaskMemFree` own
malloc-backed memory; `CoTaskMemRealloc` is implemented in the same module
and host-tested (zero size frees), but no pinned guest import calls it.
All three official pinned apps again failed USER32 import binding before
entry on base `7d38775` with the incremental W32A-11 patch (see
`w32/tests/W32A11.pinned-probe.7d38775.log`). No CLSID/IID or ProgID was
observed, so no activation table can be claimed. Since 2026-09-28 both
exports are **REAL**, not stubs: `CLSIDFromProgID` resolves
`HKCR\<ProgID>\CLSID` case-insensitively through the W32A-9 hive view
(typed refusals `E_INVALIDARG`/`CO_E_CLASSSTRING` with named last-errors;
`S_OK` leaves last-error untouched), and `CoCreateInstance` walks a
committed **empty** class table — `REGDB_E_CLASSNOTREG` for in-process
contexts, `E_NOTIMPL` beyond in-process, `CLASS_E_NOAGGREGATION` for
aggregation, always clearing the output pointer. Every call still logs its
probe line (`w32a11-clsid-probe:` with full CLSID/IID/CLSCTX;
`w32a11-progid-probe:` with at most 192 escaped UTF-16 code units and a
truncation bit), now stamped with the per-call `result=0x%08x`. Every
`CoCreateInstance` request logs the complete CLSID/IID pair and CLSCTX;
the ProgID escape stays reversible `\\uXXXX` (BMP unit and surrogate pair covered) with an explicit truncation bit. Failure HRESULTs and the named last-errors above are the refusal contract; output pointers/GUIDs are always cleared. These fixture IDs are **not** requests observed from PuTTY, 7-Zip or Notepad++ and cannot justify an activation-table row. The USER32 imports those three sessions had stopped at are now bound (see the next section).  The same-day re-probe under identical rules (`w32/tests/W32A6.pinned-probe.639388d.log`) observed all three images binding every import: PuTTY 0.85 (348 bound) reached WinMain and raised its own *unable to load any WinSock library* message box through glaunch — the W32A-12 prerequisite sits exactly where the roadmap put it; 7-Zip FM 24.09 (292 bound) entered CRT startup and logged the W32A-13 msvcrt TODO triplet before a SEH 0xc0000005; Notepad++ 8.8.9 (590 bound) faulted before any instrumented call, cause not yet identified.  No `w32a11-clsid-probe` line fired in any session, so the activation table stays EMPTY.

**Automation:** the imported `OLEAUT32` ordinals
`#2/#4/#6/#7/#9/#10/#149/#150` bind to the project's BSTR/VARIANT
routines. A BSTR stores a byte length at pointer −4, allows embedded NUL,
and has two trailing zero bytes. `SysStringByteLen` returns exact bytes,
including odd-byte strings from `SysAllocStringByteLen`; `SysStringLen`
returns floor(bytes/2), not `wcslen`. `VariantClear` and `VariantCopy`
handle scalars, BYREF as borrowed, BSTR as owned/deep-copied and
IUnknown/IDispatch with AddRef/Release; unsupported arrays/records/decimal
refuse `E_NOTIMPL` without destroying their existing owner.

**IMM32:** no IME context or composition engine exists. `ImmGetContext`
returns NULL; `ImmGetCompositionStringW` returns 0 (empty, with no output
buffer mutation); `ImmEscapeW` returns NULL; `ImmReleaseContext`,
`ImmSetCompositionWindow`, `ImmSetCompositionFontA/W`,
`ImmSetCandidateWindow`, `ImmSetCompositionStringW` and `ImmNotifyIME`
return FALSE. Each sets `ERROR_NOT_SUPPORTED` (50). CJK composition is
unavailable; direct keyboard input still works.

**Named clipboard formats:** `RegisterClipboardFormatA/W` share a
ASCII-case-insensitive UTF-8 name registry (at most 256 names; IDs start at
`0xC000`). This allocates stable IDs but does not put file paths or any
data on the clipboard.

**UxTheme:** v6 `OpenThemeData` supports a bounded flat matrix: BUTTON
pushbutton states 1–5, EDIT edittext 1–7, TAB tabitem 1–4, PROGRESS
bar/chunk (horizontal or vertical) state 0, and COMBOBOX dropdown 1–4.
Accepted parts draw onto a real GDI DC from the compositor palette;
unsupported part/state pairs return `E_NOTIMPL` without painting. Text,
content/size/font/parent queries are implemented within that matrix;
transition duration is **zero**. Buffered animation copies a compatible
bitmap at zero duration; nonzero-duration, DIB and alpha animation are
refused. A v5 window falls back rather than gaining theme support.

**Drag and file drop:** within one PE process, registered OLE targets on
different top-level HWNDs receive a caller-owned `IDataObject`. The guest
fixture checks `QueryGetData`/`GetData(CF_HDROP)`, a UTF-16 `DROPFILES`
`STGMEDIUM`, `ReleaseStgMedium` ownership and a file read. A separate native
process can send a *bounded UTF-8 pathname*, not a cross-process COM object,
through the compositor's one-time, per-owner drop token to a receiving PE.
Its owned HDROP rejects malformed UTF-8 and `DragQueryFileW` returns the
precise UTF-16 length of the BMP-accented path; the guest opens that real
Unicode-named file with `CreateFileW`. A separate host test round-trips a
non-emoji supplementary code point (U+20000) through an owned HDROP; it is
not an end-to-end guest file-drop claim. No outside-personality OLE source is
claimed. Scripted application CLSID/IID sessions and the full W32A-11 gate
remain open.

## USER32 modeless dialogs and MENUITEMINFO complete the ledgers' rows

Thirteen `stub_map.tsv` rows claimed REAL but had neither code nor a bind:
exactly the names the three pinned applications had stopped on during
import binding (`CreateDialogParamA`/PuTTY, `GetMenuItemInfoW`/7-Zip FM,
`CreateDialogIndirectParamW`/Notepad++ in the last committed probe,
`w32/tests/W32A11.pinned-probe.7d38775.log`), plus their families. All
thirteen are now implemented and bound, on the existing W32A-6 machinery:

**Modeless dialogs:** `CreateDialogIndirectParamW`, `CreateDialogParamW`
and `CreateDialogParamA` reuse the modal engine's template parsing and
frame-proc chain, minus owner-disable and the private message loop.
Visibility follows the template's `WS_VISIBLE` bit (the guest fixture's
template omits it and `IsWindowVisible` reads 0 until the app shows the
window); `WM_CLOSE` reaches the application's dialog proc — which destroys
the window itself — instead of `EndDialog`ing into a loop the app does not
have. Dialog slots recycle on `WM_DESTROY` (twelve create/destroy rounds
succeed over the 8-slot table). String resource names in the A flavor keep
the documented refusal `ERROR_NOT_SUPPORTED`, exactly like
`DialogBoxParamA`; absent ids refuse `ERROR_RESOURCE_DATA_NOT_FOUND`.
`DefDlgProcA` (and the unbound `DefDlgProcW` it forwards to, D7-style)
exists and reports every message unhandled.

**MENUITEMINFO round-trip:** `GetMenuItemInfoW`/`SetMenuItemInfoW`/
`InsertMenuItemW`/`ModifyMenuW`/`CheckMenuRadioItem`/`SetMenuItemBitmaps`/
`GetMenuState`/`GetMenuStringW` operate on the same menu-item records
`AppendMenuW` fills. `cbSize` accepts the two real Win64 sizes (72 and
80); a length-only string query (`cch==0`, NULL buffer) returns the bare
char count, copies NUL-terminate and truncation reports the copied count.
State/id/submenu/data/checkmark-bitmap fields round-trip verbatim;
`GetMenuState` packs a popup's item count in the high byte like Win32;
`CheckMenuRadioItem` clears the rest of the addressed range.

**MessageBoxIndirectW** validates `cbSize` against the real struct size,
then drives the same one-button alert path `MessageBoxW` uses; icon,
callback and language-id fields are accepted and ignored by name. The
modal alert itself is impossible in the headless guest gate, so the guest
fixture pins only the two structural refusals (NULL and truncated
`cbSize`); the positive path is asserted in the host test.

The import-library `.def` rows mirror the bind table one-to-one, the A-6
guest fixture gained two sections (`A6-MENUINFO-OK`,
`A6-CREATEDIALOG-OK`, 72 imports bound in-guest), the W32A-6 host test
grows from 75 to 113 checks, and the personality ledger now covers 620
exports with the K/U/G union gap down from 41 to **28**
(`tools/w32_import_ledger.py`'s pin records which names closed).
The same-day external probe then re-ran all three pinned binaries under
identical rules (`w32/tests/W32A6.pinned-probe.639388d.log`): every image
now binds cleanly, PuTTY reached WinMain and reported the next named
prerequisite (W32A-12 WinSock) through its own message box.

## Not implemented at all

General COM activation, .NET, DirectX, WinSock, and WOW64 (32-bit programs).
`BitBlt`/off-screen device contexts shipped in W32A-7 (the GDI raster
engine); the registry shipped in W32A-9 (the `W32HIVE1` section
above); the shell furniture shipped in W32A-10 (the section above);
printing (`StartDocW`/`StartPage`/`EndPage`/`EndDoc`) is
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
