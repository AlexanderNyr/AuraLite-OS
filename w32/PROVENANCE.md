# w32 — provenance record

Every file under `w32/` is either **written for this repository** or
**vendored** from a permissively licensed upstream. This file records which,
and is enforced by `tools/check_provenance.sh`.

The rules that govern additions are in [`LICENSING.md`](LICENSING.md); the
reasoning is in [`../docs/plans/WIN32_PLAN.md`](../docs/plans/WIN32_PLAN.md) §1.

---

## Written for AuraLite OS

Apache-2.0, like the rest of the repository. Written from the published PE/COFF
specification and the Unicode standard — no Microsoft SDK header, no Wine, no
ReactOS.

| Path | What it is |
|---|---|
| `include/w32/w32_pe.h` | PE32+ structure constants and parser API |
| `src/w32_pe.c` | PE32+ parser (bounds-checked, no allocation) |
| `include/w32/w32_utf.h` | UTF-16 ↔ UTF-8 conversion API |
| `src/w32_utf.c` | UTF-16 ↔ UTF-8 conversion |
| `tools/peinfo.c` | Host tool: dump a PE image |
| `tests/petest.asm` | A freestanding PE32+ test program (W32-3 fixture) |
| `include/w32/w32_abi.h` | The Windows-x64 ABI boundary: W32ABI and Win32 widths |
| `include/w32/w32_errno.h` | Win32 error codes and the last-error slot |
| `include/w32/w32_handle.h` | HANDLE table API |
| `include/w32/kernel32.h` | The bounded KERNEL32 import set (W32-4, D7) |
| `include/w32/w32_bind.h` | Import binding API |
| `src/w32_errno.c` | Last-error slot and errno translation |
| `src/w32_handle.c` | HANDLE table |
| `src/kernel32.c` | KERNEL32 translation layer |
| `src/w32_bind.c` | Import binding against a static export table |
| `tests/kernel32_test.asm` | A PE that imports KERNEL32 (W32-4 fixture) |
| `tests/kernel32.def` | Export list for the import library |
| `include/w32/user32.h` | USER32 + GDI32 declarations (W32-5) |
| `src/user32.c` | USER32/GDI32 mapped onto libauragui |
| `tests/user32_test.asm` | A PE that creates a window (W32-5 fixture) |
| `tests/user32.def`, `tests/gdi32.def` | Export lists for the import libraries |
| `include/w32/w32_crt.h` | CRT startup, TLS and SEH declarations (W32-6) |
| `src/w32_crt.c` | TLS callbacks, `.CRT$XC*`, `setjmp`-based `__try` |
| `include/w32/w32_argv.h` | Command-line splitting interface (W32-6) |
| `src/w32_argv.c` | Documented Win32 argv quoting rules |
| `tests/crt_test.asm` | A PE with a TLS directory and `.CRT` table |
| `include/w32/w32_module.h` | `LoadLibrary`/`GetProcAddress` interface (W32-7) |
| `src/w32_module.c` | Module table and the DLL load path |
| `tests/testdll.asm`, `tests/testdll.def` | A real DLL fixture (W32-7) |
| `examples/console-app/hello.c` | Console example, mingw-w64 built (W32-8) |
| `examples/gui-app/window.c` | GUI example over the compositor (W32-8) |
| `examples/unsupported-app/registry.c` | A deliberately refused program (W32-8) |
| `examples/dialog-app/dialog.c` | Common-dialog example, mingw-w64 built (W32A-18) |
| `examples/listview-app/listview.c` | COMCTL32 list-view example, mingw-w64 built (W32A-18) |
| `examples/delay-load-app/delayload.c` | Runtime/delay-load example, mingw-w64 built (W32A-18) |
| `app_ledger/putty-0.85.imports` | Measured import ledger, PuTTY 0.85 (W32A-0) |
| `app_ledger/7zFM-24.09.imports` | Measured import ledger, 7-Zip FM 24.09 (W32A-0) |
| `app_ledger/7z-24.09.imports` | Measured import ledger, 7z.dll 24.09 (W32A-0) |
| `app_ledger/notepad++-8.8.9.imports` | Measured import ledger, NPP 8.8.9 (W32A-0) |
| `app_ledger/npp-plugins-8.8.9.imports` | Measured import ledger, NPP plugins (W32A-0) |
| `app_ledger/audacity-3.7.5.gap` | W32A-17 gap ledger: Audacity 3.7.5 transitive import surface vs the personality (names/counts/hashes only, no bytes). Outcome B: does not run |
| `include/w32/oleaut32.h` | BSTR/VARIANT API, REAL (W32A-1) |
| `src/w32_oleaut32.c` | BSTR/VARIANT memory management, pure libc (W32A-1) |
| `include/w32/w32_manifest.h` | Manifest probing API (W32A-1) |
| `src/w32_manifest.c` | Type-24 manifest scan: execution level, comctl, dpi, OS (W32A-1) |
| `include/w32/w32_gen.h` | GENERATED ordinal/stub tables, from the TSVs (W32A-1) |
| `src/w32_stubs_gen.c` | GENERATED stub bodies (W32A-1) |
| `ordinal_map.tsv` | Ordinal→name facts, published source cited per row (W32A-1) |
| `stub_map.tsv` | GENERATED one row per unimplemented ladder import (W32A-1) |
| `tests/ordtest.asm`, `tests/ordbadname.asm`, `tests/ordbadnum.asm` | Ordinal-import fixtures (W32A-1) |
| `tests/oleaut32_ord.def`, `tests/comctl32_ord.def`, `tests/comctl32_named.def`, `tests/nosuchdll.def` | Import-library .def fixtures (W32A-1) |
| `tests/delayhelper.asm`, `tests/delaytarget.asm`, `tests/delaytarget.def` | Delay-load helper and target (W32A-1) |
| `tests/delaytest_present.asm`, `tests/delaytest_absent.asm` | Delay-load present/absent fixtures (W32A-1) |
| `tests/chain_a.asm`, `tests/chain_a.def`, `tests/chain_b.asm`, `tests/chain_b.def`, `tests/chainmain.asm` | Recursive-load chain + DllMain ordering (W32A-1) |
| `tests/cyc_c.asm`, `tests/cyc_c.def`, `tests/cyc_d.asm`, `tests/cyc_d.def`, `tests/cycmain.asm` | Import-cycle refusal fixtures (W32A-1) |
| `tests/datadll.asm`, `tests/datadll.def`, `tests/datamain.asm` | Data-export fixtures (W32A-1) |
| `tests/fwdtest.asm`, `tests/fwdtest.def`, `tests/fwdstatic.asm`, `tests/fwdmain.asm` | Forwarder-refusal fixtures (W32A-1) |
| `tests/mantest.asm`, `tests/mantest_*.manifest`, `tests/mantest_*.rc` | Manifest fixtures: v5/v6/admin/bad (W32A-1) |
| `tests/W32A1.bindreport` | Committed ledger-harness report, refreshed by the A1 unit test (W32A-1) |
| `src/kernel32_fs.c` | KERNEL32 file breadth: find/enumerate, volumes, attributes (W32A-2) |
| `src/kernel32_loc.c` | Locale, code-page and string-type breadth (W32A-2) |
| `src/kernel32_ps.c` | Process and system-info breadth: spawn, toolhelp (W32A-2) |
| `src/w32_msg.c` | The message table behind FormatMessage (W32A-2) |
| `tests/w32a2_common.h` | Shared harness for the A2 mingw-w64 guest fixtures (W32A-2) |
| `tests/w32a2_find.c` | Guest fixture: find/enumerate, attributes, volumes (W32A-2) |
| `tests/w32a2_heap.c` | Guest fixture: heaps, Global/Local, VirtualProtect (W32A-2) |
| `tests/w32a2_locale.c` | Guest fixture: code pages, locales, string types (W32A-2) |
| `tests/w32a2_map.c` | Guest fixture: CreateFile breadth and file mappings (W32A-2) |
| `tests/w32a2_pipes.c` | Guest fixture: anonymous and named pipes (W32A-2) |
| `tests/w32a2_proc.c` | Guest fixture: spawn, process info, toolhelp (W32A-2) |
| `tests/w32a2_time.c` | Guest fixture: file/system time, DOS formats (W32A-2) |
| `include/w32/w32_teb.h` | TEB-lite layout reached through GS; thread-init API (W32A-3) |
| `src/kernel32_thr.c` | Threads, TLS/FLS, synchronisation objects, APCs (W32A-3) |
| `tests/w32a3_threads.asm`, `tests/w32a3_tls.asm` | Guest fixtures: threads + sync, TLS + FLS (W32A-3) |
| `include/w32/w32_seh.h` | Win64 SEH: unwind-info structures and dispatch API (W32A-4) |
| `src/w32_seh.c` | Table-driven RtlVirtualUnwind and the `__try` dispatch (W32A-4) |
| `tools/unwinddump.c` | Host tool: dump `.pdata`/`.xdata` through our parser (W32A-4) |
| `tests/msvcrt.def` | The msvcrt import set the C++ fixtures need (W32A-4, D7) |
| `tests/w32a4_try.asm`, `tests/w32a4_unwind.asm`, `tests/w32a4_raise.asm`, `tests/w32a4_continue.asm` | SEH guest fixtures: nested `__try`, `__finally` order, RaiseException parameters, mended CONTINUE_EXECUTION (W32A-4) |
| `tests/w32a4_crash.asm`, `tests/w32a4_filter.asm`, `tests/w32a4_dialog.asm` | The unguarded-fault lane: crash, SetUnhandledExceptionFilter, the GUI-or-console dialog (W32A-4) |
| `tests/w32a4_term.asm`, `tests/w32a4_purecall.asm`, `tests/w32a4_cxthrow.asm`, `tests/w32a4_cxx.cpp` | The C++ termination surface: `?terminate`, `_purecall`, `_CxxThrowException`, and a real libgcc-personality throw/catch (W32A-4) |
| `include/w32/user32_priv.h` | The seam between the two USER32 translation units: DC handles, the window index↔handle map, client rect/bg colour and the live-window count (W32A-5) |
| `src/user32_win.c` | USER32's window and message core over the compositor: class registry, window table, queues and cross-thread `SendMessage`, Z-order/placement/focus/capture, paint/update region, scroll, metrics/colours from the theme, monitors, input state (W32A-5) |
| `tests/w32a5_win.asm` | The W32A-5 guest fixture: twelve sections through one subclassed window and a second thread (62 imports, exit 78) |
| `include/w32/w32_pe.h`, `src/w32_pe.c` | PE resource-directory walker extended with arbitrary (type,name,lang) lookup (`pe_find_resource_ex`) and bounds-checked data-entry decode used by FindResource/LoadResource/LoadString (W32A-6) |
| `include/w32/w32_rsrc.h`, `src/w32_rsrc.c` | FindResourceW/A/ExW/ExA, LoadResource/LockResource/SizeofResource/FreeResource, LoadStringW/A RT_STRING block walker, LoadIcon/Cursor/Image (returns the LockResource pointer per D5), DestroyIcon/Cursor; EnumResourceNamesW/A stub returning 1 (W32A-6) |
| `src/w32_dlg.c` | Dialog engine (DialogBoxParam/IndirectParam(A/W), EndDialog, IsDialogMessage, MapDialogRect, GetDialogBaseUnits, DlgItem(Int/Text) walkers), HMENU table (Create/Append/Insert/Check/Remove/Track/Get/Sub/Set/LoadMenu), Set/KillTimer with weak-link pump hook, caret, accelerators (Create/Copy/Destroy/Load/Translate), clipboard mapped to ag_set/get_clipboard for CF_TEXT/CF_UNICODETEXT, thread-local hooks (global hooks refused ERROR_CALL_NOT_IMPLEMENTED per D9), DrawText(W/A/Ex)/DrawFocusRect/DrawEdge/DrawFrameControl/DrawIcon(Ex)/NotifyWinEvent (W32A-6) |
| `tests/w32a6_dlg.asm` | The W32A-6 guest fixture: hand-emitted .rsrc (RT_DIALOG/1 + RT_STRING/1), resource walk, LoadStringW, DialogBoxIndirectParamW modal with DlgItem round-trips, popup menu, accelerators, thread-local hook, timer-closed modal loop, caret, CF_TEXT clipboard round-trip, DrawTextW/DrawFocusRect paint (W32A-6) |
| `include/w32/gdi32.h` | GDI32 declarations, struct layouts (BITMAPINFO/LOGPALETTE/LOGPEN/LOGBRUSH/LOGFONT/TEXTMETRIC/OUTLINETEXTMETRIC/ENUMLOGFONTEX), ROP/stock-object/text-constant pins, and the internal exports the USER32 half draws through (W32A-7) |
| `src/w32_gdi.c` | The GDI raster engine (W32A-7): typed GDI handle table, memory DCs and DIB sections (top-down/bottom-up, GetDIBits/SetDIBits), BitBlt/PatBlt/GdiAlphaBlend with software raster ops (unsupported ROPs refuse by code), Rectangle/Ellipse/RoundRect/Polygon/Polyline/FrameRect rasterisation with hatch/pattern brushes and region clipping, the font layer measured against the shipped PSF2 (metrics via GUI_OP_FONT_INFO), 8-bit palettes realised into DIB colour tables, DrawIconEx from the W32A-6 decode with a blob-keyed cache, fail-clean printing, GetDeviceCaps with the manifest dpiAware fact applied to LOGPIXELSX/Y, and window-DC rendering through one compositor blit per call |
| `tests/unit/test_w32_a7.c` | The W32A-7 host gate (169 checks): byte-exact 64x48 scene against an independent reference raster, ten documented ROPs, font metrics vs a PSF2 parser in the test, DPI-aware and unaware manifest fixtures, object model, regions, DIBs, palettes, icons, printing refusals, window-DC compositor path |
| `tests/w32a7_gdi.asm` | The W32A-7 guest fixture: window DC caps (incl. the unaware 96-DPI contract), stock objects and pen/brush lifetimes, a 64x48 compatible bitmap drawn into (PatBlt/SetPixel/LineTo/shapes/ROP2/SaveDC), text through the shipped font (metrics 8x16), regions, DIB section BGRA words and GetDIBits flip, palette realise/re-realise, window-DC BitBlt+FillRect with GetPixel readback; 54 imports, exit 78 |
| `tests/integration/cases/test_w32a7_gdi.sh` | The W32A-7 integration gate: two boots (default + tinted theme) asserting every section marker, the exit receipt, and no unresolved imports or kernel faults |
| `include/w32/comctl32.h` | COMCTL32 declarations: the 27-symbol surface, struct layouts (TBBUTTON/LV*/TV*/TC*/TOOLINFOW/HDITEMW/PSH*/NMHDR family) pinned from published documentation, message/notification constant tables, and the host palette seam (W32A-8) |
| `src/comctl32.c` | The common-control engine (W32A-8): InitCommonControls(Ex) class registration with the WS_CHILD admit marker, toolbar/status/listview/treeview/tab/progress/tooltip/header window procs over the compositor's widget set (treeview rides the real ag tree), ImageList create/add-masked/replace/draw with per-pixel alpha through the public DC API plus the drag window, SetWindowSubclass/RemoveWindowSubclass with reference data, PropertySheetW over in-memory DLGTEMPLATE word streams (PSN_APPLY both directions, IDOK/IDCANCEL), and the refusal contracts (LoadIconWithScaleDown HRESULTs, TB_CUSTOMIZE ERROR_CALL_NOT_IMPLEMENTED, _TrackMouseEvent forwarding) |
| `tests/unit/test_w32_a8.c` | The W32A-8 host gate (154 checks): the a7 fake-compositor shim extended with the widget seam, independent tab/tree/listbox hit-semantics walk as the drift detector, v5/v6 palette pins through the host knobs, and per-control message contracts incl. subclass ordering and both PSN_APPLY lParam directions |
| `tests/w32a8_comctl32.asm` | The W32A-8 guest fixture: 14 sections (INIT WSCHILD TOOLBAR STATUS LISTVIEW TREEVIEW TAB PROGRESS TOOLTIP IMAGELIST SUBCLASS PSHEET REFUSE HEADER), 48 imports, exit 78 |
| `tests/integration/cases/test_w32a8_comctl32.sh` | The W32A-8 integration gate: one boot asserting every section marker, the exit receipt, and no unresolved imports or kernel faults |
| `include/w32/advapi32.h` | ADVAPI32 declarations: the 46-symbol surface, W32_SID (revision 1, up to 8 subauthorities), the W32_SECURITY_DESCRIPTOR at the x64 natural layout (owner@8, dacl@32), disposition/registry type/CALG constants pinned from published documentation |
| `src/advapi32.c` | The registry + security engine (W32A-9): the W32HIVE1 whole-file hive (CRC32, torn-write latch), the 18 Reg* entry points with the HKLM \Software policy and the HKCR merge view, RegGetValueW coercion/expansion, the SID set + single-user membership model, descriptor builders, the hash-only CryptoAPI over libatls (SHA-256/512, SHA3-256/512; SHA-1/MD5/SHA-384 NTE_BAD_ALGID), SystemFunction036 via getrandom, and the nine FAIL-CLEAN refusals. IsTextUnicode stays in kernel32_loc.c and is bound under ADVAPI32 (the forwarder shape) |
| `tests/advapi32.def` | The import library definition for the A-9 fixture link (lld-link -def), mirroring the bind table's ADVAPI32 block |
| `tests/unit/test_w32_a9.c` | The W32A-9 host gate (364 checks): CRUD/type round-trips, MORE_DATA, folding, enums, QueryInfoKey, delete contracts, persistence reload, HKLM policy, HKCR merge, RegGetValueW, four torn-write shapes, SIDs/descriptors, crypto vectors + ragged 64KiB incremental vs the atls one-shot, RtlGenRandom, identity, failclean finals; links kernel32_loc.c so the forwarder IsTextUnicode runs the same vectors as the guest |
| `tests/w32a9_registry.asm` | The W32A-9 guest fixture: 12 sections (CORRUPT HIVE PERSIST HKLM HKCR SID SD CRYPTO RND IDENT FAILCLEAN), 46 imports, exit 78; fully position-independent, UTF-16LE one dw per character |
| `tests/integration/cases/test_w32a9_registry.sh` | The W32A-9 integration gate: two boots against one persistent AHCI scratch disk — cross-boot marker read-back, then a torn hive that must fail loud and still exit 78 (39/39) |
| `include/w32/shlwapi.h` | SHLWAPI declarations: the 18-symbol Path*/Color*/AssocQueryStringW surface (W32A-10) |
| `src/shlwapi.c` | The SHLWAPI pure engine (W32A-10): Path* string transforms (drive/UNC/slash, drive number, extension, FileSpec, Combine/Append, AddExtension with .exe default, CompactPathEx ellipses), PathFileExistsW via w32_fs_xlate, PathMatchSpecW wildcard, Color* HLS (0..240) with the documented luminance math, AssocQueryStringW empty-table S_FALSE |
| `include/w32/shell32.h` | SHELL32 declarations: the 19-symbol known-folder/PIDL/file-op/shell-execution/notification surface (W32A-10) |
| `src/shell32.c` | The shell engine (W32A-10): known folders (CSIDL mapping onto /disk or /tmp, PIDL {cb,csidl,path} round-trip), IShellItem minimal, SHGetFileInfoW/ExtractIconExW via PE icon decode, SHFileOperationW over the VFS (no undo), ShellExecute verb dispatch (open via spawn, runas/noassoc refused), Shell_NotifyIconW onto ag_notify, Drag trio model |
| `include/w32/shell32_priv.h` | WR-1 (W32RUN_PLAN.md §7) the shell namespace ABI: the documented shobjidl vtable layouts (`IShellFolder`/`IEnumIDList`), `STRRET`/`SFGAO_*`/`SHGDN_*`/`SHCONTF_*` constants, our PIDL item kinds, and the IID byte arrays — an author's expression of a documented interface ABI (mingw-w64 headers / MSDN), the same class of fact docs/win32.md records; no foreign implementation consulted |
| `src/shell32_ns.c` | WR-1 (W32RUN_PLAN.md §7) the REAL shell namespace: a minimal-but-honest `IShellFolder`/`IEnumIDList`/PIDL graph rooted Desktop→My Computer→C:→CFSFolder, enumerating the VFS through the REAL FindFirstFileW; backs `SHGetDesktopFolder` and the `SHGetFileInfoW` PIDL branch; the GUI-object verbs (CreateViewObject/GetUIObjectOf/BindToStorage/SetNameOf) fail clean E_NOTIMPL by name; author-written |
| `include/w32/comdlg32.h` | COMDLG32 declarations: the 10-symbol common-dialog surface (W32A-10) |
| `src/comdlg32.c` | The common-dialog engine (W32A-10): GetOpenFileNameW/Save (filter/initial-dir/multi-select, hook precedence, overwrite prompt via MessageBox), ChooseColor/ChooseFont over the 16-colour and VGA 8x16 ladder, PrintDlgW FAIL-CLEAN, SHBrowseForFolderW folder picker, the listbox class (LB_*) and the dialog template expander |
| `include/w32/version.h` | VERSION declarations: the 3-symbol file-version surface (W32A-10) |
| `src/version.c` | The VERSION reader (W32A-10): PE VS_VERSION_INFO tree walk (wLength/wValueLength, 32-bit pads, StringFileInfo language tables), GetFileVersionInfoSizeW/W via w32_fs_xlate and pe_find_resource, VerQueryValueW with the engine blob prefix |
| `include/w32/w32aux.h` | Small-module declarations: WININET InternetCrackUrlW, DBGHELP ImageNtHeader, DWMAPI pair, SENSAPI pair, WINTRUST WinVerifyTrust, CRYPT32 eight (W32A-10) |
| `src/w32aux.c` | The small-module engine (W32A-10): InternetCrackUrlW pure URL parser, ImageNtHeader pointer arithmetic, DWMAPI constants, SENSAPI TCP probes, WinVerifyTrust TRUST_E_NOSIGNATURE, CRYPT32 eight fail-clean |
| `tests/shlwapi.def`, `tests/shell32.def`, `tests/comdlg32.def`, `tests/version.def`, `tests/wininet.def`, `tests/dbghelp.def`, `tests/dwmapi.def`, `tests/sensapi.def`, `tests/wintrust.def`, `tests/crypt32.def` | Import-library .def fixtures for the A-10 surface (lld-link -def) |
| `tests/unit/test_w32_a10.c` | The W32A-10 host gate (158 checks): Path*/Color*/Assoc, known folders/PIDL, Shell file ops/notify/drag/execute, Version via the generated PE, Url/ImageNtHeader, DWM/SensApi/Wintrust/Crypt32, comdlg validation; amalgamates kernel32_fs.c and the five engines |
| `tests/unit/test_w32_a10_pe.h` | Generated PE fixture for the A-10 host gate: RT_VERSION/1 (VS_VERSION_INFO 1.2.3.4 + 4 strings), RT_GROUP_ICON/1 + RT_ICON/1 (16x16) (W32A-10) |
| `tests/w32a10_furniture.asm` | The W32A-10 guest fixture: 12 sections (PATH COLOR FOLDER PIDL COMDLG URL IMAGE DWM SENSAPI WINTRUST CRYPT), 17 imports, exit 78 |
| `tests/integration/cases/test_w32a10_furniture.sh` | The W32A-10 integration gate: one boot asserting every section marker, the exit receipt, and no kernel faults |
| `tests/integration/cases/test_w32a1_loader.sh` | W32A-1 gate, recalibrated in W32A-7 after the suite's first machine run: the exit-code triad now anchors on the shell's `[shell]` attribution line because the kernel reports every child exit twice ([thread] then [shell]) |
| `tests/integration/cases/test_w32_user32.sh` | W32-5 gate, reworked in W32A-7 after the suite's first machine run: the hostile-WNDPROC leg boots alone behind a QEMU monitor socket and dismisses the W32A-4 unhandled-exception alert with sendkey, instead of blocking headless on the MessageBoxA modal; asserts the PE's nonzero exit, the compositor reap, and clean shell exits for both boots |
| `include/w32/ole32.h`, `src/ole32.c` | W32A-11 incremental COM init depth and task allocator; CLSID activation remains TODO |
| `include/w32/imm32.h`, `src/imm32.c` | W32A-11 typed FAIL-CLEAN IME boundaries; no composition engine |
| `include/w32/uxtheme.h`, `src/uxtheme.c` | W32A-11 compositor-palette flat BUTTON/EDIT/TAB/PROGRESS/COMBO part/state matrix, per-part refusal, per-window theme cleanup, zero-duration compatible-bitmap animation; timed effects/DIB/alpha refused |
| `src/w32_clipfmt.c` | W32A-11 shared A/W named clipboard-format registry; does not imply clipboard data transport |
| `include/w32/ws2_32.h`, `src/ws2_32.c` | W32A-12 WS2_32 WinSock: thin adapter over the native libc socket surface + DNS/inet parser; version negotiation, socket verbs, `select` fd_set translation, `getpeername`/`getsockname` cache, resolution; WSA event/overlapped family refused by name; author-written, no third-party bytes |
| `tests/w32a12_winsock.c` | W32A-12 guest fixture: mingw-w64 TU (`-lws2_32`, name imports) exercising the WinSock surface over the native stack; author-written |
| `include/w32/msvcrt.h`, `src/msvcrt.c` | W32A-13 msvcrt bridge: forwards the 34+22 7-Zip-gate symbols onto the native libc + the W32A-2/3/4 runtimes (heap unity via `GetProcessHeap`/`HeapAlloc`, `_beginthreadex` onto `CreateThread`, the CRT startup/exit surface, the string/mem core, the MSVCRT rand LCG, `__CxxFrameHandler` continue-search + type_info dtor); author-written, no third-party bytes |
| `tests/w32a13_msvcrt.c` | W32A-13 guest fixture: mingw-w64 TU (`-lmsvcrt`, name imports) exercising heap unity across two DLLs, strings, rand, `_beginthreadex`, and the `_onexit`->`exit(78)` path; author-written |
| `src/kernel32_con.c` | W32A-14 (PuTTY app gate) KERNEL32 console + serial slice: `GetConsoleMode`/`GetConsoleOutputCP`/`WriteConsoleW`/`ReadConsoleW`/`SetStdHandle` REAL over the std handles + UTF converter, and the serial (COMM) set fail-clean (serial is a documented non-goal); author-written, no third-party bytes |
| `tests/w32a14_putty.c` | W32A-14 guest fixture: mingw-w64 TU (name imports for KERNEL32/USER32/ADVAPI32, dynamic WS2_32) exercising PuTTY's personality paths — `Reg*A` session save/load, console/cursor/bell/class-long, serial fail-clean, dynamic WinSock; author-written |
| `tests/W32A14.probe.log` | W32A-14 import-coverage probe of the pinned `putty.exe`: names/counts only (348 imports, 8 DLLs, 0 TODO), sha256/size of the source binary; no binary bytes |
| `tests/w32a15_7zip.c` | W32A-15 (7-Zip FM app gate) guest fixture: mingw-w64 TU (name imports for KERNEL32/USER32/GDI32/COMCTL32/SHELL32/ADVAPI32, dynamic COMCTL32) exercising 7-Zip's personality paths — `LoadBitmapW` RT_BITMAP→HBITMAP + GetPixel palette expansion, `msvcrt` heap trio (malloc/free/realloc), SysListView32 rows, MPR delay-load no-provider, `Reg*W` round-trip, `SHFileOperationW` delete; author-written |
| `tests/w32a15_7zip.rc` | W32A-15 fixture resource script embedding an author-written 4bpp packed-DIB `RT_BITMAP` (7-Zip toolbar-strip shape) for the `LoadBitmapW` path; author-written, no third-party bytes |
| `tests/w32a15_glyph.bmp` | W32A-15 fixture bitmap: author-authored 4bpp/16-colour packed DIB referenced by the .rc; hand-built pixel bytes, no third-party bytes |
| `tests/W32A15.probe.log` | W32A-15 import-coverage probe of the pinned `7zFM.exe` + `7z.dll`: names/counts only (298 + 86 imports, 11 + 5 DLLs, 0 TODO after LoadBitmapW REAL, 6 mpr delay), sha256/size of the source binaries and two live-boot transcripts; no binary bytes |
| `tests/integration/cases/test_w32a15_7zip_fixture.sh` | W32A-15 QEMU effect gate booting `w32a15_7zip.exe` and asserting `W32A15-7ZIP-OK`/exit 78; CI-provable half of the 7-Zip gate, no pinned-app pixel claim |
| `tests/unit/test_w32_a15_bitmap.c` | W32A-15 sanitized host vector for `w32_gdi_bitmap_from_dib`: hand-built 1/4/8/24/32bpp packed DIBs (bottom-up + top-down) and malformed inputs, `GetObjectW`/`SelectObject`/`GetPixel` round-trip; author-written |
| `tests/w32a16_npp.c` | W32A-16 (Notepad++ app gate) guest fixture: mingw-w64 TU (name imports for KERNEL32/USER32/GDI32/COMCTL32/SHELL32/SHLWAPI/OLE32/WININET/SENSAPI/WINTRUST) exercising NPP's personality paths — SysTabControl32 tab strip, SHLWAPI Path*/Color*, the `IShellItem` traced open path, VFS save/reopen, plugin DLL chain, tray icon, offline updater, and (added this phase) the nine newly-landed imports: DeferWindowPos batch, `CreateIconIndirect`/`GetIconInfo` round-trip, `wsprintfW`, `FreeLibraryWhenCallbackReturns`, `ReadDirectoryChangesW` fail-clean; author-written, no third-party bytes |
| `tests/W32A16.probe.log` | W32A-16 import-coverage probe of the pinned `notepad++.exe`: names/counts only (590 imports, 19 DLLs), class census (565 REAL / 25 FAIL-CLEAN, 0 REFUSE, 0 TODO), the traced no-`GetOpenFileName` open path, the census-gap correction (13 → 4) and the refreshed live fixture boot; sha256/size of the source binary, no binary bytes |
| `tests/integration/cases/test_w32a16_npp_fixture.sh` | W32A-16 QEMU effect gate booting `w32a16_npp.exe` and asserting `W32A16-NPP-OK`/exit 78; CI-provable half of the NPP gate, no pinned-app pixel claim; registered in `tests/integration/run_all.sh` |
| `tests/wr1_shellns.c` | WR-1 (W32RUN_PLAN.md §7) the SHELL32 namespace guest fixture: mingw-w64 TU (name imports for SHELL32/OLE32/UUID/KERNEL32/USER32) driving `SHGetDesktopFolder` and the `IShellFolder`/`IEnumIDList`/PIDL graph through the documented COM vtable — desktop QI, EnumObjects, BindToObject descent to the VFS root, GetDisplayNameOf/GetAttributesOf, ParseDisplayName round-trip + fail-clean, CreateViewObject E_NOTIMPL; author-written, no third-party bytes |
| `tests/unit/test_shell_ns.c` (repo-root `tests/`, host twin) | WR-1 host gate: amalgamates `src/shell32_ns.c` and mocks FindFirstFileW/FindNextFileW/FindClose against a scripted VFS, driving the whole object graph through the vtable under ASan/UBSan (45 checks); author-written |
| `tests/integration/cases/test_wr1_shell_namespace.sh` | WR-1 QEMU effect gate booting `wr1_shellns.exe` and asserting `WR1-SHELLNS-OK`/exit 78; CI-provable half of the namespace gate; registered in `tests/integration/run_all.sh` |
| `tests/unit/test_w32_a16_layout_icon.c` | W32A-16 sanitized host vector for the two REAL slices landed this phase: `BeginDeferWindowPos`/`DeferWindowPos`/`EndDeferWindowPos` batch deferral + flush + HDWP lifetime, and the `CreateIconIndirect`/`GetIconInfo` icon⇄bitmap round-trip (colour + AND-mask alpha, malformed fail-clean); author-written |
| `tests/unit/test_w32_a16_shlwapi.c` | W32A-16 sanitized host vector for the `shlwapi.c` Path*/Color*/`AssocQueryStringW` families; author-written |
| `tests/ole32_a11.def`, `tests/oleaut32_a11.def`, `tests/imm32_a11.def`, `tests/uxtheme_a11.def` | W32A-11 incremental PE import-library declarations (only fixture names; no third-party bytes) |
| `tests/w32a11_core.asm` | W32A-11 subset guest fixture: COM/BSTR/VARIANT, OLEAUT32 ordinals, IMM32 refusal, clipboard-format IDs, BUTTON pixel diff |
| `tests/w32a11_probe.asm` | W32A-11 independent Win64 PE with synthetic GUID and Unicode ProgID vectors: generated TODO probes log exact IDs, return `E_NOTIMPL`, clear outputs; no pinned-app activation is inferred |
| `tests/W32A11.pinned-probe.partial.log` | W32A-11 text-only SHA-256-verified upstream application bind probes; no observed CLSID/IID pairs yet |
| `src/w32_ole_drag.c` | W32A-11 in-process OLE drop-target callbacks and STGMEDIUM ownership; no external OLE source or cross-process marshalling |
| `tests/w32a11_drag.asm` | W32A-11 Win64 callback/refcount fixture: real IDataObject vtable hands CF_HDROP/UTF-16 DROPFILES in HGLOBAL to a target, which reads the Unicode-named file; mouse messages are posted, not an external source |
| `tests/w32a11_file_receiver.asm` | W32A-11 independent PE receiver asserting a compositor-generated WM_DROPFILES BMP UTF-16 pathname, opened by CreateFileW and checked against on-disk bytes |
| `tests/w32a11_native_drop.c` | W32A-11 native GUI sender, spawning an independent w32run and submitting a pathname through the kernel compositor |
| `tests/w32a11_payload.txt`, `tests/w32a11-é.txt` | Original ASCII and BMP Unicode-named on-disk payloads with known bytes for the file-drop / IDataObject guest fixtures; author-written, no third-party bytes |
| `tests/w32a11_token_sender.c`, `tests/w32a11_token_receiver.c` | Independent native tasks testing window-slot recycling, stale/one-shot drop tokens, per-owner ACL and actual payload bytes |
| `tests/w32a11_theme_matrix.asm` | Two independent v5/v6 manifest PE fixtures using real GDI pixel reads, zero-duration animation and timed-effect refusal |
| `tests/integration/cases/test_w32a11_dragdrop.sh`, `tests/integration/cases/test_w32a11_tokens.sh`, `tests/integration/cases/test_w32a11_theme.sh` | Incremental QEMU effect gates; none claims the full CLSID/IID application probe |
| `tests/unit/test_w32_a11_drag.c` | Sanitized host Win64 OLE callback/refcount and STGMEDIUM ownership vector |
| `tests/W32A11.pinned-probe.5350c54.log` | Text-only, SHA-256-matched second probe on exact 5350c54 base; all three blocked at earlier USER32 imports before entry, NOT a CLSID/IID table |
| `tests/W32A11.pinned-probe.7d38775.log` | Text-only, SHA-256-matched probe on exact 7d38775 base plus incremental W32A-11 changes; same three earlier USER32 bind failures, no observed CLSID/IID |
| `LICENSING.md`, `PROVENANCE.md` | This documentation |

### On the application ledgers

`app_ledger/*.imports` are generated by `tools/w32_import_ledger.py` from
user-supplied binaries that are measured and never committed. They record
DLL names, symbol names, counts and hashes -- facts about an interface,
no bytes -- in the same spirit as the `.def` files below. The provenance
gate scans the tree for the `MZ` magic so a renamed binary fails like a
committed one.

`app_ledger/*.gap` (W32A-17) is the same class of fact for a binary that does
NOT run: `audacity-3.7.5.gap` records Audacity 3.7.5's transitive import surface
(names/counts/hashes across its 117 bundled DLLs) diffed against the personality.
Because Audacity's `.exe` imports only its own bundle, the surface was measured
transitively (the W32A-0 tool's single-binary model plus the loader's own parser,
`build/w32_peinfo`); no bytes from the binary enter the tree.

### On the Win32 names and error codes

`kernel32.h` declares function names such as `WriteFile` and constants such as
`ERROR_INVALID_HANDLE = 6`. These are the **interface being reimplemented** --
the names and values a program links against and compares to. They are written
from published documentation, in this project's own style (`W32_ERROR_*`,
`W32ABI`, `W32_BOOL`), with implementations written from scratch. No SDK
header was opened; see `LICENSING.md` and `WIN32_PLAN.md` section 1.

`tests/kernel32.def` lists the same names so `lld-link` can build an import
library. It is a text file naming an interface, not a Microsoft artefact, and
no `kernel32.dll` from Microsoft is used or shipped.

### On the kernel-side loader

`kernel/proc/pe.c`, `kernel/proc/pe.h` and `userspace/apps/w32run/w32run.c`
live outside this directory but belong to the same effort. They are written for AuraLite, modelled on the
in-tree `kernel/proc/elf.c`, and call the parser here rather than duplicating
it. No Microsoft, Wine or ReactOS code was consulted.

### On the PE structure constants

`w32_pe.h` defines values such as `PE_DOS_MAGIC 0x5A4D`, `PE_MACHINE_AMD64
0x8664` and the `IMAGE_SCN_*` flag bits. These are **facts about a published
file format**, taken from the PE/COFF specification, which Microsoft publishes
for exactly this purpose. They are written here in this project's own naming
style (`PE_SCN_MEM_EXECUTE`, not `IMAGE_SCN_MEM_EXECUTE`) and with this
project's own structure layouts, deliberately: the file is an independent
expression of the same facts, not a transcription of a header.

No SDK header was opened while writing it.

---

## Vendored (not yet imported)

**Status: none vendored yet.**

`WIN32_PLAN.md` phase W32-0 provides for vendoring the **mingw-w64** Win32 API
headers into `w32/include/` when the personality begins implementing exported
functions (phase W32-4). Nothing has needed them so far: the parser and the
converter are written from specifications and require no API declarations.

When they are imported, each entry gets a row here:

| Path | Upstream | Version / commit | Licence | Imported |
|---|---|---|---|---|
| *(none yet)* | | | | |

Only these licences are acceptable for vendored files: **public domain,
ZPL-2.1, BSD-3-Clause, MIT**. mingw-w64's headers are distributed under
public-domain dedications and ZPL-2.1, which is why they are the chosen source;
its *runtime* is partly LGPL and must not be imported.

---

## Explicitly absent

For the avoidance of doubt, the following have contributed **nothing** to any
file under `w32/`:

- Wine (LGPL-2.1-or-later)
- ReactOS (GPL-2.0 / LGPL-2.1)
- The Microsoft Windows SDK or DDK
- Any leaked Windows source
- Any disassembly or decompilation of a Microsoft binary

No Microsoft binary is redistributed by this repository.
