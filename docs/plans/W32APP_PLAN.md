# AuraLite OS — Win32 Applications Plan (w32 breadth + the OSS ladder)

## Status: PLANNED — W32A-0 – W32A-18, none started

| Phase | State |
|---|---|
| W32A-0 Import ledgers, receipt protocol, claim checker | ✅ done |
| W32A-1 Loader: ordinals, delay-load, DLL chains, manifests | ⬜ planned |
| W32A-2 `KERNEL32` breadth I — files, paths, time, process info | ⬜ planned |
| W32A-3 Threads and per-thread TLS (the one kernel change) | ⬜ planned |
| W32A-4 The table-driven SEH unwinder | ⬜ planned |
| W32A-5 `USER32` breadth I — windows and messages, the `W` core | ⬜ planned |
| W32A-6 `USER32` breadth II — dialogs, menus, clipboard, resources | ⬜ planned |
| W32A-7 `GDI32` breadth — DCs, blitting, regions, fonts | ⬜ planned |
| W32A-8 `COMCTL32` — toolbar, status, listview, treeview, tabs, ImageLists | ⬜ planned |
| W32A-9 Registry and `ADVAPI32` — the hive, SIDs, CryptoAPI, security stubs | ⬜ planned |
| W32A-10 `SHELL32` + `COMDLG32` + `SHLWAPI` + `VERSION` | ⬜ planned |
| W32A-11 `OLE32`-lite, drag-and-drop, `OLEAUT32`, `IMM32` stubs | ⬜ planned |
| W32A-12 `WS2_32` WinSock over the native socket stack | ⬜ planned |
| W32A-13 The `msvcrt` bridge (data exports, `_beginthreadex`, EH names) | ⬜ planned |
| W32A-14 App gate I — PuTTY | ⬜ planned |
| W32A-15 App gate II — 7-Zip File Manager | ⬜ planned |
| W32A-16 App gate III — Notepad++ | ⬜ planned |
| W32A-17 App horizon — Audacity, or a documented gap list | ⬜ planned |
| W32A-18 Integration, documentation and the honest matrix | ⬜ planned |

This document answers one question:

> *What is the smallest honest path from today's 44-function personality to
> real, unmodified, open-source Windows applications — PuTTY, then 7-Zip,
> then Notepad++ — with every step measured against the binaries themselves?*

It is a sequel to [`WIN32_PLAN.md`](WIN32_PLAN.md), and it follows the
structure of the existing plans: dependency-ordered phases, a definition of
done and a test gate for every phase, and one `.patch` per phase. It inherits
`WIN32_PLAN.md` §1 and decision D4 wholesale — the legal boundary does not
get renegotiated here — and extends them in §1 below for the one new thing
this plan touches: real third-party binaries as test subjects.

**Baseline:** commit `0ec41a4` (RT2-RT3 update), on top of the completed
`WIN32_PLAN.md` (W32-0 – W32-8).

**What is new about the method.** Every breadth plan before this one picked
functions by judgement. This one picks them by measurement: §2 records import
censuses taken from the actual ladder binaries with `llvm-readobj-19`, and
every breadth phase (W32A-2, W32A-5 – W32A-13) implements exactly the ledger
for its module — no more, no less. Where a measurement contradicted the
request that commissioned this plan, the measurement won and the plan says
so (see D4 and the API-sets note in §2.3).

**Evidence model.** Third-party binaries can never be committed to this
repository (§1). So every phase has two gates, not one: a **fixture gate**
(mingw-w64-built binaries, committed, running in CI) and, for the app phases,
a **receipt gate** (a paste-back protocol in the style of
[`docs/live_web.md`](../live_web.md) and
[`docs/metal_receipts.md`](../metal_receipts.md): the user runs the pinned
binary, pastes the log back, the receipt names the sha256). Fixtures prove
the mechanism; receipts prove the application. Neither is accepted as proof
of the other.

---

## 1. The legal boundary for real binaries

`WIN32_PLAN.md` §1 stands unchanged: no Microsoft code, no Microsoft SDK
headers, no Wine or ReactOS as sources *or references*, declarations from
mingw-w64 headers and published documentation, the subsystem stays `w32`.
This section adds only what real applications require on top.

### 1.1 The binaries are test subjects, never tree contents

- The ladder binaries (PuTTY, 7-Zip, Notepad++, Audacity) are obtained by
  the user from their publishers, exactly as `WIN32_PLAN.md` §1.3 already
  requires for any `.exe`. They are **never committed** — not to `w32/`,
  not to `tests/`, not to the initrd, not as base64 in a document.
- What *is* committed: version numbers, sha256 hashes, file sizes, and
  **import ledgers** — lists of DLL and symbol names. Names are facts
  about an interface, the same class of fact the generated table in
  `docs/win32.md` already records. No code, no bytes, no resources from
  the binaries enter the tree.
- `tools/check_provenance.sh` grows one rule in W32A-0: any committed file
  that parses as PE (`MZ` + `PE\0\0`) must be buildable from in-tree
  sources (fixtures) — a foreign `.exe`/`.dll` fails the build. The
  existing `.dll`/`.sys`/`.msi` filename rule stays as the backstop.

### 1.2 The ladder is open-source software, and that changes nothing about copying

| Application | Licence | Why it matters here |
|---|---|---|
| PuTTY | MIT | Permissive, but irrelevant: nothing is copied. |
| 7-Zip | LGPL-2.1-or-later | Copyleft, but irrelevant: nothing is copied. |
| Notepad++ | GPL-3.0 | Copyleft, but irrelevant: nothing is copied. |
| Audacity | GPL-2.0-or-later | Copyleft, but irrelevant: nothing is copied. |

Running a program does not create a derivative work of it, and this plan
copies nothing from any of them — no code, no dialog layouts, no icons, no
strings. The licences are recorded so a reviewer can check the reasoning,
not because any of them grants anything this plan needs.

### 1.3 The applications' sources are not references (D4 extended)

Decision D10 extends the `WIN32_PLAN.md` D4 rule to the ladder itself: a
contributor implementing `w32` behaviour does not read PuTTY's, 7-Zip's,
Notepad++'s or Audacity's source to do it — GPL/LGPL code included. The
permitted inputs are the same four D4 allows: mingw-w64 headers, published
Microsoft documentation (readable, not copyable), the PE/COFF specification,
and **observed behaviour of binaries the contributor lawfully possesses**.
A stub-vs-real decision (§3, D9) is justified by a runtime observation
recorded in the receipt, never by "their source shows it ignores the
error". This keeps the provenance story one sentence long: *no third-party
source of any licence was consulted.*

### 1.4 Total Commander is out of this plan, stated plainly

Total Commander is proprietary shareware. Nothing in §1 forbids a user from
*running* a licensed copy against the breadth this plan builds — and the
listview/treeview/registry breadth in W32A-8/W32A-9 is exactly the
Total-Commander-shaped breadth. But it cannot be a gate: no receipt for a
binary the project cannot freely redistribute evidence about, no CI
fixture, no pinned hash in the tree. If a future plan wants a proprietary
gate, it must solve the evidence problem first. This plan does not try.

### 1.5 Not legal advice

As in `WIN32_PLAN.md` §1.4: this section is a written record of reasoning
and sources, not legal advice. Anyone shipping AuraLite commercially should
have counsel review it together with `WIN32_PLAN.md` §1.

---

## 2. Where things actually stand

Measured against the tree at the baseline commit and against the ladder
binaries in `/tmp` (probed 2026-09-12, never committed), not assumed.

### 2.1 The pinned ladder

| Gate | Application | Version | File | Size | sha256 (prefix) | Format |
|---|---|---|---|---|---|---|
| W32A-14 | PuTTY | 0.85 | `putty.exe` | 1 706 136 B | `d01fdb5aae8f1125…` | PE32+ GUI x64, subsys 6.0, 10 sections |
| W32A-15 | 7-Zip FM | 24.09 | `7zFM.exe` | — | `dc4fdcd96efe7b41…` | PE32+ GUI x64, subsys 5.2 |
| W32A-15 | 7-Zip codec | 24.09 | `7z.dll` | — | `882063948d675ee4…` | PE32+ DLL x64, exports `CreateObject` et al. |
| W32A-16 | Notepad++ | 8.8.9 | `notepad++.exe` (portable) | — | `a470014bb7f6d858…` | PE32+ GUI x64, subsys 6.0 |
| W32A-16 | NPP plugins | 8.8.9 | `NppExport`/`mimeTools`/`NppConverter`/`nppPluginList` `.dll` | — | (pinned in W32A-0) | PE32+ DLL x64 |

Probe tool: `llvm-readobj-19 --coff-imports` (Debian trixie), cross-checked
with `objdump -p`. The full per-symbol ledgers (1 531 static imports across
the five files) are committed by W32A-0; this section carries the census
tables and the structural findings that shape the phases.

Ladder order rationale, from the numbers: PuTTY first (348 imports, 8
DLLs, no COMCTL32, no CRT — the narrowest real GUI program of the three),
7-Zip second (adds COMCTL32, `msvcrt`, a real DLL codec chain and
delay-load), Notepad++ third (590 imports, 19 DLLs, the full breadth).
Each gate reuses everything before it and adds exactly one new dimension.

### 2.2 The import census

Static imports per DLL (`llvm-readobj-19 --coff-imports`):

**PuTTY 0.85 — 348 imports, 8 DLLs:**

| DLL | Imports |
|---|---:|
| KERNEL32.dll | 148 |
| USER32.dll | 119 |
| GDI32.dll | 51 |
| ADVAPI32.dll | 15 |
| COMDLG32.dll | 6 |
| IMM32.dll | 5 |
| ole32.dll | 3 |
| SHELL32.dll | 1 |

**7-Zip File Manager 24.09 — 298 imports, 11 DLLs (+ delay-load, §2.3):**

| DLL | Imports |
|---|---:|
| KERNEL32.dll | 106 |
| USER32.dll | 88 |
| msvcrt.dll | 34 |
| ADVAPI32.dll | 21 |
| ole32.dll | 11 |
| SHELL32.dll | 11 |
| COMCTL32.dll | 10 |
| OLEAUT32.dll | 7 |
| mpr.dll (delay-loaded) | 6 |
| comdlg32.dll | 3 |
| GDI32.dll | 1 |

**Notepad++ 8.8.9 — 590 imports, 19 DLLs:**

| DLL | Imports |
|---|---:|
| USER32.dll | 214 |
| KERNEL32.dll | 186 |
| GDI32.dll | 63 |
| COMCTL32.dll | 23 |
| ADVAPI32.dll | 20 |
| shlwapi.dll | 17 |
| UxTheme.dll | 15 |
| ole32.dll | 11 |
| SHELL32.dll | 10 |
| imm32.dll | 9 |
| CRYPT32.dll | 8 |
| comdlg32.dll | 2 |
| OLEAUT32.dll | 2 |
| SensApi.dll | 2 |
| dwmapi.dll | 2 |
| VERSION.dll | 3 |
| dbghelp.dll | 1 |
| WININET.dll | 1 |
| WINTRUST.dll | 1 |

**7z.dll — 86 imports, 5 DLLs:** KERNEL32 54, msvcrt 22, OLEAUT32 7,
USER32 2 (`CharUpperW`, `CharPrevExA`), ADVAPI32 1 (`SystemFunction036`).

**NPP plugins** import only built-in modules plus `SHLWAPI.dll` and
`COMDLG32.dll` — no plugin-to-plugin chains, so single-level DLL loading
plus the W32A-10/W32A-11 modules suffices for the shipped plugins.

**The KERNEL32/USER32/GDI32 union:** the three applications jointly need
**611** distinct `KERNEL32`/`USER32`/`GDI32` symbols. The current
personality exports 44 functions, of which **42** are in the union. The
gap is **569 symbols**, tagged per application in the W32A-0 ledger. The
two current exports no ladder binary imports are named in the ledger, not
here — they stay regardless, because fixtures use them.

### 2.3 Structural findings (each one shapes a phase)

Measured, one by one, with the phase each finding creates or kills:

1. **Imports by ordinal are real and load-bearing.** `7zFM.exe` imports
   `COMCTL32#(17)`; `notepad++.exe` imports `COMCTL32#(17,381,410,411,412,
   413)`, `OLEAUT32#(4,6)`, `SHELL32#(165)`; `7zFM.exe`/`7z.dll` import
   `OLEAUT32#(2,4,6,7,9,10,149[,150])`. Today's loader refuses ordinal
   imports (`w32/src/w32_bind.c:137`) and `GetProcAddress` has no ordinal
   path. → W32A-1 must resolve ordinals (from mingw-w64 import data and
   documented ordinals) or refuse each unknown ordinal by name. There is
   no third option: the ladder does not load without this.
2. **Delay-load is real: `7zFM.exe` delay-loads `MPR.dll`.** The delay
   directory is present (`rva=0xd4b34, size=0x40`, one descriptor plus
   terminator); the six `WNet*` imports resolve at first call. Today's
   loader refuses any image with a delay directory
   (`w32/src/w32_module.c:212`). → W32A-1 must honour the delay directory
   (stub-thunk patching on first call), with `MPR.dll` itself provided as
   a FAIL-CLEAN module (D9): no network drives exist, and the `WNet*`
   calls say so.
3. **Per-thread TLS is required, not optional.** PuTTY and Notepad++
   carry real thread-storage directories and import
   `TlsAlloc`/`TlsFree`/`TlsGetValue`/`TlsSetValue` and the `Fls*`
   set; 7-Zip reaches TLS through `msvcrt`'s `_beginthreadex`.
   Today's TLS is one block per process (`docs/win32.md`), and `w32run`
   is single-threaded (no thread references in
   `userspace/apps/w32run/w32run.c`). → W32A-3, the plan's one kernel
   change (`swapgs` + a user GS base + a TEB-lite).
4. **The unwinder is required by all three gates.** `.pdata` counts:
   PuTTY 2 430, 7-Zip FM 4 824, Notepad++ 10 543 `RUNTIME_FUNCTION`s. All
   three import `RaiseException`, `RtlCaptureContext`,
   `RtlLookupFunctionEntry` and `RtlVirtualUnwind` (plus
   `RtlPcToFileHeader`/`RtlUnwind`/`RtlUnwindEx` in two of three).
   Today's SEH is a `sigsetjmp` shim (`w32/src/w32_crt.c`) that abandons
   frames and runs no destructors. → W32A-4, before any app gate (D7).
5. **The CRT bridge is required by the 7-Zip gate.** `7zFM.exe` imports
   34 `msvcrt.dll` symbols including `__C_specific_handler`,
   `_CxxThrowException`, `__CxxFrameHandler`, `_XcptFilter`,
   `?terminate@@YAXXZ`, `_beginthreadex`, `_initterm`, `__getmainargs`,
   `_acmdln`, `_onexit`, `_purecall` — plus **data exports** (`_fmode`,
   `_commode`, `_acmdln`) and plain `mem*`/`str*`/`malloc`. `7z.dll`
   adds `realloc`/`strstr`/`strchr`. PuTTY and Notepad++ link the CRT
   statically (no CRT DLL in their tables — verified, not assumed). →
   W32A-13. The C++ names are the plan's top technical risk (§6):
   cleanup-unwinding is in scope, typed C++ `catch` is named residue (D7).
6. **SxS manifests are real.** All three binaries depend on
   `Microsoft.Windows.Common-Controls` (all three pin v6.0.0.0 — PuTTY's
   manifest comments "Load Common Controls 6 instead of 5"); Notepad++
   adds `asInvoker`, Notepad++ and 7-Zip carry five `supportedOS` GUIDs
   each (PuTTY: none), and PuTTY declares `dpiAware` plus `PerMonitorV2`.
   → W32A-1 parses
   the manifest from `.rsrc`, selects the comctl32 major, and records the
   DPI-awareness the GDI layer honours in W32A-7.
7. **PuTTY loads its network stack dynamically.** No `WS2_32`/`WSOCK32`
   in its static table; the strings `ws2_32.dll`, `WSAStartup`,
   `getaddrinfo` and `Unable to load any WinSock library` are present. →
   W32A-12 provides `WS2_32` as a loadable module through the
   `LoadLibrary`+`GetProcAddress` path, and W32A-12's first task is a
   logging run that records exactly which `WS2_32` surface PuTTY touches
   (the ledger cannot be dumped statically for dynamic imports).
8. **API-set forwarders are NOT required by the ladder.** None of the
   five files imports any `api-ms-win-*` DLL; all use classic DLL names.
   The commissioning request listed API-set forwarders as in scope; the
   measurement overrules it. → Deferred to §7 with the reason recorded
   (D4). W32A-1 keeps the W32-7 forwarder refusal for true cross-DLL
   forwarders and resolves only same-table aliases. If a later ladder
   binary needs API sets, the ledger will say so and a phase will exist.
9. **`GDI+` is NOT required by the ladder.** No `gdiplus.dll` anywhere in
   the five files or the shipped plugins. → §7, same treatment.
10. **The COM surface is narrow and drag-drop-shaped.** All three import
    `CoInitialize`/`CoUninitialize`/`CoCreateInstance`; Notepad++ and
    7-Zip add `OleInitialize`/`OleUninitialize`, `RegisterDragDrop`/
    `RevokeDragDrop`, `DoDragDrop`, `ReleaseStgMedium`, `CoTaskMemAlloc`/
    `CoTaskMemFree`, and one `CLSIDFromProgID`. No apartments beyond
    init/uninit, no ROT, no monikers, no embedding. → W32A-11 (OLE-lite).
    Which CLSIDs `CoCreateInstance` actually requests is runtime data,
    recorded by the W32A-11 receipt probe.
11. **The registry is required by all three gates.** PuTTY: `Reg*` `A`
    set (stored sessions). Notepad++: `Reg*` `W` set + CryptoAPI
    (`CryptAcquireContextW`/`CryptCreateHash`/`CryptHashData`/
    `CryptGetHashParam`/`CryptDestroyHash`/`CryptReleaseContext`).
    7-Zip: `Reg*` + LSA (`LsaOpenPolicy`/`LsaAddAccountRights`/
    `LsaClose`), account lookup, token privileges, `Get/SetFileSecurityW`.
    → W32A-9, with the hive format ours (D5), CryptoAPI mapped onto
    `libatls` hashes where the ledger needs it, and LSA/privileges as
    documented FAIL-CLEAN (single-user machine, no privileges to hold).
12. **The clipboard is required by all three gates.**
    `OpenClipboard`/`EmptyClipboard`/`SetClipboardData`/`CloseClipboard`/
    `GetClipboardData` appear in every ladder binary. → W32A-6, mapped
    onto the existing kernel clipboard (`/apps/gclip` already manages
    it — the mapping has a working native client to test against).
13. **Shell tray, folders and file operations are real but mappable.**
    `Shell_NotifyIconW` (Notepad++ tray icon) maps onto the compositor's
    notification engine (it exists — GUI v2.0). `SHGetFolderPathW`
    (Notepad++ `%APPDATA%`) maps onto a documented directory. `SHFileOperationW`,
    `SHBrowseForFolderW`, `SHGetFileInfoW`, `ExtractIconExW`,
    `ShellExecuteExW`, `SHChangeNotify` are sized work, not research. →
    W32A-10.
14. **The stub-policy modules are small and mostly pure.** `SHLWAPI`
    (`Path*`, `Color*`, `AssocQueryStringW`), `VERSION`
    (`GetFileVersionInfo*`, real from PE resources), `WININET`
    (`InternetCrackUrlW` — a pure parser, real), `dbghelp`
    (`ImageNtHeader` — trivially real), `dwmapi` ×2, `SensApi` ×2,
    `WINTRUST` ×1, `CRYPT32` ×8, `IMM32` ×5–9, `mpr` ×6 (delay),
    `PrintDlgW` (present as an import; printing itself stays §7, the
    dialog fails cleanly with "no printers"). → spread across W32A-9 –
    W32A-12 with per-function REAL/FAIL-CLEAN records in the ledger (D9).

### 2.4 What exists to build on

| Requirement | State | Evidence |
|---|---|---|
| PE loader + import binder + DLL loading | ✅ in-tree | `kernel/proc/pe.c`, `w32/src/w32_bind.c`, `w32/src/w32_module.c`, `w32run` |
| In-tree PE parser to extend | ✅ in-tree | `w32/src/w32_pe.c`, `w32/tools/peinfo.c` |
| Compositor to map windows onto | ✅ 100 FPS, themes, notifications | `kernel/gui/`, GUI v2.0 |
| Widgets to map controls onto | ✅ label/button/textbox/checkbox/slider/progress/listbox/panel/scrollarea/tab | `lib/libauragui/include/auragui.h` |
| Kernel clipboard | ✅ exists, native client | `/apps/gclip` (`gclip --selftest` round-trips it) |
| Sockets + TCP + DNS | ✅ syscalls 300–307 incl. bind/listen/accept | `kernel/arch/x86_64/syscall.c:126-133`, `kernel/net/` |
| Hashes for CryptoAPI | ✅ SHA-256/512, SHA3, HMAC (no SHA-1) | `lib/libatls/src/` |
| CSPRNG for `SystemFunction036` | ✅ `getrandom` syscall 319 + ChaCha20 DRBG | `kernel/rng.c` |
| Threads to map `CreateThread` onto | ✅ kernel threads + `clone` syscall 56 | `kernel/proc/clone.c`, `kernel/proc/thread.c` |
| Screenshot/pixel test infra | ✅ in-tree | `tools/analyze_screen.py`, `tools/read_screen.py`, `test_gui.sh`, `vncdotool` dep |
| Path parsing, UTF-16↔UTF-8 | ✅ in-tree | `kernel/fs/path.c`, `w32/src/w32_utf.c` |
| Provenance gate to extend | ✅ in-tree | `tools/check_provenance.sh` (refuses `.dll`/`.sys`/`.msi`, Wine/ReactOS mentions) |
| Claim-checker pattern to follow | ✅ in-tree | `tools/check_lx_claims.py` (artefact + receipt pins) |
| Receipt-protocol pattern to follow | ✅ in-tree | `docs/live_web.md`, `docs/metal_receipts.md` |
| Native filesystem for the hive + `%APPDATA%` | ✅ `/disk`, `/fat`, `/ext2` writable | `docs/filesystem.md` |
| `W` entry points as primary | ❌ reversed (D6 note) | `docs/win32.md`: "`A` entry points are the primary surface today" |
| Ordinal imports, delay-load, forwarder chains | ❌ refused | `w32_bind.c:137`, `w32_module.c:212`, forwarder refusal |
| Recursive DLL loading | ❌ one level only | `docs/win32.md`: "a DLL can only import from the built-in modules" |
| Threads in `w32run`, per-thread TLS | ❌ single-threaded, per-process TLS | `userspace/apps/w32run/w32run.c`, `docs/win32.md` |
| Table-driven unwinding, `RaiseException` | ❌ `sigsetjmp` shim | `w32/src/w32_crt.c` |
| Registry, COM, WinSock, COMCTL32, SHELL32, `BitBlt`/DCs, resources | ❌ absent | no `RegOpenKey`/`CoInitialize`/`WSAStartup`/`BitBlt`/`InitCommonControls` outside the unsupported-app fixture |

### 2.5 Honest scope: what "runs" will mean, per gate

A gate is not "the application works perfectly". Each app gate promises a
named set of flows and names what is degraded:

- **PuTTY (W32A-14):** config dialog renders and edits; sessions save to
  and load from the registry; Raw/TCP and Telnet connect over the
  QEMU user network; the terminal renders text. SSH is in scope *as a
  protocol over the socket* (PuTTY ships its own crypto) against a test
  server. Serial (`COM*`) is out — there are no COM ports to map
  (§7). GSSAPI/SSPI (`SECUR32.DLL`, `GSSAPI64.DLL`, dynamically loaded)
  fail cleanly: Kerberos auth is absent, password auth works.
- **7-Zip FM (W32A-15):** the file manager launches; `7z.dll` loads
  through the DLL chain and `CreateObject`; an archive lists in the
  listview; extract produces byte-exact files; the options property
  sheet opens; settings persist via the registry. Shell-extension
  (`7-zip.dll`, COM server registration) is out. Archive *creation*
  is in scope where the `7z.dll` surface allows it; formats whose
  codecs need missing process features degrade with the codec's own
  error, not a crash.
- **Notepad++ (W32A-16):** launches; the Scintilla view renders and
  edits; tabs switch; files open and save (through whichever dialog
  path the receipt probe finds — and the W32A-16 gate's first task is
  to trace it); the Find dialog
  works; the three shipped plugins load; the tray icon appears as a
  compositor notification; the updater detects "offline" through the
  documented stubs and never crashes. Printing (`PrintDlgW` is imported)
  reports "no printers". IME/CJK input is degraded wherever the `IMM32`
  stubs stand in.
- **Audacity (W32A-17):** the horizon phase promises an outcome, not an
  application: either a launch-and-render receipt or the measured gap
  ledger for the next plan. Audio playback is out (§7).

### 2.6 What the probes could not see (recorded, not guessed)

Static import tables do not show: `GetProcAddress` names (PuTTY's whole
network surface, any plugin's late binding), `CoCreateInstance` CLSIDs,
delay-load timing beyond "present", or which imported functions are
actually *called* in a session. Each phase whose ledger has a dynamic
hole starts with a logging run that fills it (W32A-11, W32A-12, W32A-16).
A ledger entry that was never observed at runtime is marked `static-only`
until a receipt promotes it.

---

## 3. Decisions

### D1. Ladder-gated breadth (extends W32 D7)

A function enters the personality if and only if the committed ledger
shows a ladder binary importing it (statically or through an observed
dynamic load). The set grows only when the ledger grows. "Useful" and
"obviously next" are not admission criteria — the 569-symbol gap is
already larger than any contributor's intuition, and intuition is how a
plan like this fails by dilution.

### D2. Real binaries are never committed; receipts are paste-back

§1.1 is the mechanism, this is the workflow: every app gate is proved by
a receipt in `docs/w32app_receipts.md` naming the pinned sha256, the QEMU
command line, the serial excerpts and the screenshots. The claim checker
(W32A-0, wired in W32A-18) pins each gate checkbox to its receipt section
the way `check_lx_claims.py` pins phases to artefacts. A gate without a
receipt is an unchecked box, whatever the code does.

### D3. x86-64 only; PE32 is a future series, named now

The ladder is 64-bit and the plan stays 64-bit. 32-bit PE (compat-mode
segments, 32-bit argument marshalling, a WOW64-lite translation in the
spirit of the `lx` per-process map) is explicitly deferred to a future
`W32C` series, recorded in §7. Deferring it here is what keeps this plan
a breadth plan instead of a second kernel plan. The loader keeps refusing
PE32 with the current message; the refusal text gains a pointer to §7.

### D4. Load-bearing vs measured-absent: ordinals and delay-load stay, API sets go

§2.3 items 1–2 make ordinal and delay-load support mandatory — the ladder
does not load without them. §2.3 item 8 removes API-set forwarders from
scope: requested, measured absent from all five ladder files, deferred
with the reason recorded (the W32-0 header-vendoring precedent: a surface
with no caller is a liability, not an asset). W32A-1 keeps the W32-7
refusal for true cross-DLL forwarders and resolves same-table aliases
only. If a later ladder binary imports `api-ms-win-*`, the ledger will
show it and a phase will exist — the mechanism for growing is D1.

### D5. Map onto what exists; the hive is ours (extends W32 D5)

`USER32`/`GDI32`/`COMCTL32` map onto the compositor, GUI syscalls and
`libauragui` — still no second window manager, no second widget set where
one exists. `WS2_32` maps onto syscalls 300–307. CryptoAPI hashes map
onto `libatls`. `GetFileVersionInfo` reads the PE resources the loader
already parses. The registry hive is the one new store, and its format is
ours — documented in W32A-9,fsync'd, migratable — never byte-compatible
with Windows hives. Claiming Windows-hive compatibility would be claiming
an undocumented format; §7 says so.

### D6. `W` becomes the real one (repairs the D6 reversal)

`docs/win32.md` records that the `A` surface is primary today, "the
reverse of decision D6". All new string entry points in this plan
implement `W` (UTF-16) as primary with `A` as a converting wrapper, per
the original D6 and the strict (no-`U+FFFD`) conversion in
`w32/src/w32_utf.c`. The existing 44 `A`-primary exports stay as they
are — churn without a caller is the same liability D4 rejects — with one
exception: where a ladder binary imports the `W` twin of an existing `A`
export, the pair is unified behind the `W` implementation in whichever
phase owns it.

### D7. The unwinder before the apps; typed C++ `catch` is named residue

No app gate runs before W32A-4 lands: a personality that longjmps across
C++ frames would corrupt the very applications it claims to run, and
frames-abandoned is a data-loss shape, not a cosmetic one. In scope:
`.pdata`/`.xdata` walking, `RtlVirtualUnwind` and family,
`RaiseException`, `__C_specific_handler` dispatch, `__finally`, cleanup
execution (C++ destructors run during unwinding), unhandled-exception
filtering. Named residue: typed C++ `catch` (`__CxxFrameHandler` semantics
for catch-clause matching). `_CxxThrowException` unwinds cleanups and
then terminates with a named message instead of matching catch clauses.
If a gate receipt shows a ladder binary depending on typed catch in a
core flow, the gate fails honestly and the residue promotes to a phase —
the receipt, not this paragraph, decides.

### D8. One kernel change, isolated (extends W32 D2)

Everything in this plan is user space except W32A-3: `swapgs` on the
kernel entry/exit paths, a user GS base per thread, and a TEB-lite
(`TlsSlots`, `FlsSlots`, thread id, last-error slot). The change is one
phase with the full suite as its gate, precisely because it touches every
entry path (§6). If W32A-3 proves too dangerous to land, the plan has a
documented fallback — per-thread TLS emulated in `w32run` via gsbase
patching at thread switch — recorded in the phase, not silently adopted.

### D9. Three implementation classes, recorded per function

Every ledger symbol carries one class, and the class is part of the
committed ledger:

- **REAL** — full behaviour for the ladder's flows. Core paths only;
  exotic flags within a REAL function may still refuse with a named
  error (the refusal names the flag).
- **FAIL-CLEAN** — the function exists, binds, and reports honest
  failure: "no printers", "no network drives", "IME unavailable",
  "offline assumed". Returning success while doing nothing is forbidden
  — a lie in the success shape is worse than a refusal, because the
  caller proceeds on a false premise.
- **REFUSE** — load-time refusal naming the symbol (the W32-8 lesson:
  fail at load, before one instruction runs, never zero-and-pray).

A class change (FAIL-CLEAN → REAL or the reverse) is a ledger edit with
a reason, reviewed like code.

### D10. Application sources are not references (extends W32 D4)

§1.3. Stub-vs-real decisions are justified by runtime observations in
receipts, never by reading the applications' sources. The provenance
checker cannot detect a violation after the fact (the `WIN32_PLAN.md` §6
honesty, inherited), which is why the rule is stated before any code.

---

## 4. Phases

### Phase W32A-0 — Import ledgers, receipt protocol, claim checker ✅ DONE

**Objective:** commit the measurement §2 summarises, in a form machines
can check, before any behaviour phase depends on it.

#### Tasks

- [x] `tools/w32_import_ledger.py`: dumps `dll → sorted symbols (+ordinal
      markers, +delay markers)` from a user-supplied PE, using the repo's
      own `w32_pe` parser where it reaches and `llvm-readobj-19` where it
      does not yet (`.rsrc` manifests, delay descriptors). The two must
      agree on the five pinned files; disagreement is a bug in our parser,
      fixed here, not a second source of truth.
- [x] `w32/app_ledger/`: one committed file per ladder binary
      (`putty-0.85.imports`, `7zFM-24.09.imports`, `7z-24.09.imports`,
      `notepad++-8.8.9.imports`, `npp-plugins-8.8.9.imports`), each headed
      by version, size, full sha256 and probe date. Symbol names only —
      no bytes from the binaries (§1.1).
- [x] Every ledger symbol gains its D9 class (`REAL`/`FAIL-CLEAN`/`REFUSE`) and,
      for dynamic surfaces, a `static-only` marker per §2.6. The union
      gap list (569 K/U/G symbols) is generated from these files, so §2.2
      can never drift from them.
- [x] `docs/w32app_receipts.md`: the paste-back protocol — pinned-binary
      table, QEMU command lines per gate, what to paste (serial excerpts,
      screenshots), and the exact assertion phrases each gate greps.
      Empty receipt sections for W32A-14 – W32A-17, marked `AWAITING`.
- [x] `tools/check_w32app_claims.py` skeleton: parses this plan's phase
      table and verdicts, checks ledger files exist and parse, checks
      every `✅` phase names its patch + fixtures + (for gates) receipt
      section. Wired into `make test-unit` in W32A-18; until then run by
      hand.
- [x] `tools/check_provenance.sh` grows the §1.1 rule: a committed file
      that parses as PE must be reproducible from in-tree sources.
      Negative control: a planted foreign `.exe` fails the check.

#### Test gate

- The ledger tool reproduces §2.2's census tables exactly (348 / 298 /
  590 / 86 / plugin counts) from the pinned binaries.
- `check_w32app_claims.py --selftest` catches a planted violation
  (unchecked box marked `✅`, missing ledger file, receipt section still
  `AWAITING` on a `✅` gate).
- `check_provenance.sh` fails on a planted foreign `.exe` and passes on
  the tree's own fixtures.

**Deliverable:** `tools/w32_import_ledger.py`, `w32/app_ledger/*.imports`,
`docs/w32app_receipts.md`, `tools/check_w32app_claims.py` (skeleton),
provenance rule + selftest, `patches/W32A0_ledger.patch`.

**Result (2026-09-12):** the census is committed and machine-checked.
`tools/w32_import_ledger.py` is a pure-stdlib PE32+ parser (imports,
ordinals, delay descriptors, TLS presence, `.pdata` counts, `.rsrc`-24
manifest facts) with the D9 module table + per-symbol overrides baked in;
`dump` emits the five ledgers, `agree` re-parses each binary against
`llvm-readobj-19`, `check` re-derives §2.2 from the committed files plus
the live export count of `w32/src/w32_bind.c`. `agree` confirms all eight
measured binaries import-for-import (putty 348, 7zFM 298 incl. six
delay-loaded `mpr` rows, 7z.dll 86, notepad++ 590, four plugins
99/82/117/74); `check` prints `w32_import_ledger: CHECK OK -- 4 ledgers +
plugins, union 611, gap 569`. `tools/check_w32app_claims.py --check`
passes (artefacts + greppable receipts + census rerun + the REFUSE guard:
no app gate may flip green while its ledger holds a REFUSE row) and
`--selftest` proves it fails a lying tree; `tools/check_provenance.sh`
passes the tree and its selftest proves the new PE rule (name filter +
`MZ`-magic scan) catches a renamed binary. The eight REFUSE rows are the
unresolved ordinals W32A-1 must resolve-or-refuse-by-number; they block
the app gates honestly, by construction.

Measurement corrected the draft in four places, all inside this patch:
the K/U/G union is **611** (gap **569**), not 600/558 — the draft's
ad-hoc text pipeline undercounted the covered set, while the 42-symbol
overlap with the 44 current exports is unchanged; `.pdata` counts are
PuTTY 2 430 / NPP 10 543 (directory size, confirmed against section
`VirtualSize`), not 1 752 / 9 693; all three manifests pin comctl32
**v6.0.0.0** (PuTTY's XML comments "Load Common Controls 6 instead of
5" — the draft's "unversioned PuTTY" came from a one-line `strings`
grep of multi-line XML), so the v5 branch is fixture-proved rather than
ladder-measured; and Notepad++ carries a TLS directory too, not just
PuTTY. Two planned items were re-scoped, not dropped: the parser is
stdlib-only instead of the planned `w32_pe`+`readobj` hybrid (the gate
stays hermetic — CI needs no LLVM), and the receipts doc fixes the
per-gate block format plus `AWAITING` stubs now while each gate's QEMU
lines land with its gate (writing them before the loader exists would be
fiction). Delivered as `patches/W32A0_ledger.patch`.

---

### Phase W32A-1 — Loader: ordinals, delay-load, DLL chains, manifests ✅ DONE

**Objective:** turn "refused at load" into "runs until the first missing
import" for every ladder binary. After this phase the loader binds all
five pinned files' static tables (against possibly-stubbed modules —
binding is not behaviour).

#### Tasks

- [x] Ordinal imports in the static binder and in `GetProcAddress`:
      ordinal→name maps per built-in module, built from mingw-w64 import
      data and documented ordinals. Covers the measured set at minimum:
      `COMCTL32#(17,381,410,411,412,413)`, `OLEAUT32#(2,4,6,7,9,10,149,
      150)`, `SHELL32#(165)`. Unknown ordinals refuse by number (D9).
- [x] Delay-load directory honoured: `__delayLoadHelper`-equivalent that
      resolves the target on first call through the thunk and patches it.
      Proved against the measured case (`7zFM.exe` → `MPR.dll`, six
      `WNet*`). A delay target that is itself absent fails at first call
      with the DLL named — the documented Windows behaviour, not a load
      refusal.
- [x] Recursive DLL loading with an in-progress set: a user DLL may
      import from built-ins *and* from other user DLLs (lifts the
      `docs/win32.md` single-level limit). Cycles refuse by name. Depth
      cap documented. `DllMain` order: dependencies first (the
      `DLL_PROCESS_ATTACH` contract), teardown in reverse.
- [x] Data exports: exported RVAs that are data (`msvcrt`'s `_fmode`,
      `_commode`, `_acmdln`) bind as addresses, not entry points.
      `GetProcAddress` returns them; the loader never calls them.
- [x] Manifest parsing from `.rsrc` type 24: SxS `assemblyIdentity` for
      `Microsoft.Windows.Common-Controls` selects the comctl32 major
      (v6.0.0.0 in all three ladder manifests; the v5/unversioned branch
      exists for manifests without the dependency and is fixture-proved);
      `requestedExecutionLevel` is recorded and honoured as
      "asInvoker always" (there is no elevation; a `requireAdministrator`
      manifest refuses with the reason named, it does not pretend);
      `dpiAware`/`dpiAwareness` is recorded for W32A-7. `supportedOS`
      GUIDs are parsed and ignored loudly (logged, not actioned).
- [x] Same-table alias resolution where a built-in legitimately answers
      two names; true cross-DLL forwarders keep the W32-7 refusal (D4).
      The refusal text names the forwarder target.
- [x] W32A-9 reserves: none. W32A-1 binds; behaviour phases fill.

#### Test gate

- Fixtures (all `nasm -f win64` + `lld-link` or mingw-w64, in-tree):
  ordinal-import exe, delay-load exe (delay target present *and*
  absent — both behaviours asserted), two-DLL chain + DLL cycle
  (refusal names the cycle), data-export DLL, manifest v5/v6/asInvoker/
  requireAdministrator (four outcomes asserted).
- The five pinned binaries' static tables bind end to end in a
  binder-only harness (no entry-point execution): every import resolves
  to *something* (REAL, FAIL-CLEAN stub, or a named REFUSE entry — the
  gate asserts the ledger-predicted class for each, which is what makes
  the ledger falsifiable this early).
- Full `make test` green; all W32-0 – W32-8 gates unchanged.

#### Done

The loader binds every ladder binary's static table: 15 documented
ordinals (COMCTL32, OLEAUT32, SHELL32) resolve by number in the static
binder and in `GetProcAddress`, with unknown ordinals refused by number;
`#<n>` names refuse with the number named. Delay-load thunks resolve
through an in-guest helper on first call (present and absent targets
asserted separately). User DLLs import from built-ins and from each
other: the load claims its slot before mapping (a nested load can no
longer steal it), cycles refuse by name, depth is capped, `DllMain`
runs dependencies-first and detaches in reverse. Data exports bind as
addresses the loader never calls. Type-24 manifests select comctl32,
record asInvoker/dpi, refuse requireAdministrator by name, and log
supportedOS loudly. The guest suite passes 34/34; the binder-only
harness binds the five pinned tables (782 stubs, 3 data cells) and the
committed `w32/tests/W32A1.bindreport` agrees textually with a fresh
one. Delivered as `patches/W32A1_loader.patch`.

**Deliverable:** binder/loader changes, ordinal maps, delay helper,
recursive load, manifest parser, fixtures
(`w32/tests/delaytest.*`, `w32/tests/ordtest.*`, `w32/tests/mantest*`),
`tests/integration/cases/test_w32_a1_loader.sh`,
`patches/W32A1_loader.patch`.

---

### Phase W32A-2 — `KERNEL32` breadth I: files, paths, time, process info ⬜ PLANNED

**Objective:** the measurable file/time/process-info subset of the ledger —
everything in it is REAL (no stubs in this phase; a file API that lies
corrupts user data, and D9 forbids the success-shaped lie).

#### Tasks

- [ ] Find/enumerate: `FindFirstFileW`/`FindFirstFileA`/
      `FindFirstFileExW`/`FindNextFileW`/`FindNextFileA`/`FindClose`,
      `FindFirstStreamW`/`FindNextStreamW` (streams beyond `::$DATA`
      report "not found" — NTFS streams do not exist here, and the
      error says so), change notifications (`FindFirstChangeNotificationW`/
      `FindNextChangeNotification`/`FindCloseChangeNotification` —
      backed by VFS mtime polling at a documented granularity, or
      FAIL-CLEAN if the polling proves racy; the phase records which).
- [ ] Attributes/time: `GetFileAttributesW`/`GetFileAttributesExW`,
      `SetFileAttributesW` (readonly/hidden/system mapping documented —
      hidden-ness is a name-prefix convention, stated not smuggled),
      `CopyFileW`/`CopyFileExW` (progress callback honoured, cancel
      honoured), `MoveFileW`/`MoveFileExW`/`MoveFileWithProgressW`,
      `ReplaceFileW`, `DeleteFileW`/`DeleteFileA`, `RemoveDirectoryW`,
      `CreateDirectoryW`, `CreateHardLinkW` (REAL where the FS allows,
      named error where it does not), `GetCompressedFileSizeW`,
      `GetDiskFreeSpaceW`/`GetDiskFreeSpaceExW` (REAL from the VFS),
      `GetDriveTypeW`/`GetLogicalDriveStringsW`/`GetVolumeInformationW`
      (the AuraLite volume model, documented: one fixed drive per mount),
      `GetFinalPathNameByHandleW`, `GetFullPathNameW`, `GetLongPathNameW`,
      `SetFileTime`/`FileTimeToLocalFileTime`/`FileTimeToSystemTime`/
      `FileTimeToDosDateTime`/`LocalFileTimeToFileTime`/
      `CompareFileTime`/`SystemTimeToTzSpecificLocalTime`,
      `GetSystemTimeAsFileTime`, `GetLocalTime`, `GetTimeZoneInformation`
      (UTC, documented), `QueryPerformanceCounter`/
      `QueryPerformanceFrequency` (REAL from the TSC/LAPIC calibration).
- [ ] Handles/files: `CreateFileW` (all `dwCreationDisposition` values,
      sharing flags honoured between w32 handles), `ReadFile`/`WriteFile`
      breadth (overlapped: `GetOverlappedResult` REAL for completed I/O;
      true async completion is FAIL-CLEAN — the VFS has no async
      completion ports), `SetFilePointer`/`SetFilePointerEx`/
      `SetEndOfFile`/`FlushFileBuffers`/`GetFileSize`/`GetFileSizeEx`/
      `GetFileType`/`GetFileInformationByHandle`/`SetHandleInformation`,
      `CreateFileMappingW`/`CreateFileMappingA`/`MapViewOfFile`/
      `UnmapViewOfFile` (backed by `mmap`/`MAP_SHARED`), `CancelIo`,
      `DeviceIoControl` (named-refusal per control code — storage
      ioctls are not a file API).
- [ ] Pipes/processes: `CreatePipe`, `CreateNamedPipeA`/`ConnectNamedPipe`/
      `WaitNamedPipeA` (named pipes over the VFS fifo layer —
      `mkfifo` exists; single-instance semantics documented),
      `CreateProcessA`/`CreateProcessW` (PE + ELF via the spawn path,
      stdio redirection REAL, `STARTUPINFO` honoured,
      `GetStartupInfoA`/`GetStartupInfoW` REAL),
      `GetExitCodeProcess`/`TerminateProcess`/`OpenProcess`/
      `GetProcessTimes`/`GetCurrentProcess`/`GetCurrentProcessId`,
      `GetModuleFileNameA`/`GetModuleFileNameW`/
      `GetModuleHandleExW` (REAL from the loader tables),
      `GetCommandLineW` (D6: unify behind `W`),
      `GetEnvironmentStringsW`/`FreeEnvironmentStringsW`/
      `GetEnvironmentVariableA`/`SetEnvironmentVariableW`/
      `ExpandEnvironmentStringsW` (REAL; the w32 environment block),
      `GetCurrentDirectoryA`/`GetCurrentDirectoryW`/
      `SetCurrentDirectoryA`/`SetCurrentDirectoryW` (per-process cwd —
      `kernel/fs/cwd.c` exists), `GetTempPathA`/`GetTempPathW`
      (`/tmp`, documented), `GetWindowsDirectoryA`/`GetWindowsDirectoryW`/
      `GetSystemDirectoryA` (fixed virtual paths, documented as virtual),
      `GetVersion`/`GetVersionExW`/`GetProductInfo` (a *documented lie
      with a version number*: report Windows 10 build N, recorded as the
      compatibility identity — the one place this plan sanctions a fixed
      impersonation, because version checks branch on it and there is no
      honest value; the value is one constant, greppable, in one file).
- [ ] Locales/strings/time-format: `GetACP`/`GetOEMCP`/`GetCPInfo`/
      `IsValidCodePage` (65001 UTF-8 + 1252 recorded set),
      `GetUserDefaultLCID`/`GetUserDefaultLangID`/
      `GetSystemDefaultLangID`/`GetLocaleInfoA`/`GetLocaleInfoW`/
      `GetLocaleInfoEx`/`GetStringTypeExA`/`GetStringTypeExW`/
      `GetStringTypeW`/`IsValidLocale`/`EnumSystemLocalesW`/
      `CompareStringW`/`CompareStringEx`/`LCMapStringA`/`LCMapStringW`/
      `LCMapStringEx`/`IsTextUnicode` (REAL for the `en-US` + Unicode
      casing tables shipped; locales beyond the shipped set refuse by
      name), `GetDateFormatW`/`GetDateFormatEx`/`GetTimeFormatW`/
      `GetTimeFormatEx`, `MultiByteToWideChar`/`WideCharToMultiByte`
      (strict, via `w32_utf`), `lstrcmp*`/`lstrcpy*`/`lstrlenW`/
      `wsprintfW` (pure, REAL), `CharUpperW`/`CharLowerW`/`IsChar*W`,
      `FormatMessageA`/`FormatMessageW` (REAL from the message table —
      every error this personality returns gains a message here, or the
      error is unreturnable; that rule is the phase's QA).
- [ ] Memory/heaps: `HeapReAlloc`/`HeapSize` (join the existing heap
      exports), `GlobalAlloc`/`GlobalLock`/`GlobalUnlock`/`GlobalFree`/
      `GlobalSize`, `LocalAlloc`/`LocalFree`, `VirtualProtect`
      (REAL via `mprotect`), `GetLargePageMinimum` (reports 0 extra —
      large pages unsupported, allocation falls back; the caller-visible
      behaviour, not a secret), `GetPhysicallyInstalledSystemMemory`?
      only if the ledger shows it (it does not — example of D1 working).
- [ ] Misc process-info: `IsDebuggerPresent` (FALSE, documented),
      `IsProcessorFeaturePresent` (REAL from CPUID),
      `GetNativeSystemInfo`/`GetSystemInfo`, `GetTickCount`
      (joins `GetTickCount64`), `Beep` (PC speaker — REAL, the backend
      exists; frequency/duration honoured), `SleepEx` (alertable:
      FAIL-CLEAN alertable bit, REAL sleep), `OutputDebugStringW`
      (serial log, tagged), `GetApplicationRestartSettings`/
      `RegisterApplicationRestart`/`UnregisterApplicationRestart`
      (FAIL-CLEAN: no restart manager; registration is accepted and
      honoured never — *documented*, the one sanctioned accept-and-ignore,
      because refusing it breaks shutdown paths that otherwise work).

#### Test gate

- One mingw-w64 fixture per group above (find, time, mapping, pipes,
  process, locale, heap), each asserting REAL behaviour and each refusal
  by name. The message-table rule is enforced: a test enumerates every
  error code the phase can return and asserts `FormatMessage` covers it.
- Round-trips against the native shell: files created by the fixture
  visible to `ls`/`cat`, and vice versa.
- Full `make test` green.

**Deliverable:** `w32/src/kernel32_fs.c` (or equivalent split),
message table, fixtures, `tests/integration/cases/test_w32a2_kernel32.sh`,
`patches/W32A2_kernel32fs.patch`.

---

### Phase W32A-3 — Threads and per-thread TLS (the one kernel change) ⬜ PLANNED

**Objective:** `CreateThread` threads that run concurrently with
per-thread TLS/FLS, `GetLastError` and wait semantics — and the `swapgs`
change that makes user GS possible, isolated in this phase alone.

#### Tasks

- [ ] Kernel: `swapgs` on every Ring 3↔0 transition (syscall entry/exit,
      IRQ/exception return, signal trampoline), a per-thread user GS
      base, and the audit that no path reads `cpu_local` after swapping
      the wrong way. This is the plan's riskiest diff by construction;
      it lands with entry-path assertions (debug builds verify
      `GS_BASE` expectations at each transition) that stay on in CI.
- [ ] TEB-lite: per-thread `TlsSlots[64]`, `FlsSlots[128]`,
      `LastErrorValue`, `ClientId`, `ThreadLocalStoragePointer`.
      Reached through the user GS base at the documented offsets
      (`GS:[0x58]`-family compatibility for the slots the ladder's CRT
      touches — measured from `msvcrt`'s needs in W32A-13, not from
      memory).
- [ ] Threading API (all REAL): `CreateThread`/`ExitThread`/
      `TerminateThread`/`GetCurrentThread`/`GetCurrentThreadId`/
      `ResumeThread`/`OpenThread`? (ledger decides), `TlsAlloc`/
      `TlsFree`/`TlsGetValue`/`TlsSetValue`, `FlsAlloc`/`FlsFree`/
      `FlsGetValue`/`FlsSetValue` (callbacks on thread exit REAL),
      `InitializeCriticalSection`(+`AndSpinCount`, +`Ex`)/
      `EnterCriticalSection`/`LeaveCriticalSection`/
      `DeleteCriticalSection`/`TryEnterCriticalSection`? (ledger),
      `AcquireSRWLockExclusive`/`ReleaseSRWLockExclusive`/
      `TryAcquireSRWLockExclusive` (+Shared sides as the ledger shows),
      `SleepConditionVariableSRW`/`WakeAllConditionVariable`/
      `WakeConditionVariable`? (ledger), `InitOnceBeginInitialize`/
      `InitOnceComplete`, `InitializeSListHead` (+ the ledger's
      `Interlocked*SList` set), `CreateEventA`/`CreateEventW`/
      `SetEvent`/`ResetEvent`, `CreateMutexA`/`CreateMutexW`/
      `ReleaseMutex`, `CreateSemaphoreW`/`ReleaseSemaphore`,
      `WaitForSingleObject`/`WaitForSingleObjectEx`/
      `WaitForMultipleObjects`/`MsgWaitForMultipleObjects`
      (REAL over wait queues + the message queue; abandonment semantics
      REAL), `QueueUserAPC` (REAL, delivered at alertable waits),
      `EncodePointer`/`DecodePointer` (REAL, per-process cookie),
      `CreateThreadpoolWork`/`SubmitThreadpoolWork`/
      `CloseThreadpoolWork` (minimal REAL pool — Notepad++ imports
      exactly these three; the pool has N threads, N documented),
      `SetThreadAffinityMask` (REAL where the scheduler allows,
      best-effort documented — `7z.dll` sets it; silently ignoring
      affinity on an SMP scheduler is a performance lie, so the call
      reports what it actually pinned).
- [ ] TLS callbacks per thread: the PE TLS directory's callbacks run on
      every thread start (lifts the process-once limit), template
      initialised per thread. `msvcrt`'s `_beginthreadex` path is
      exercised here through a fixture (the real bridge is W32A-13).
- [ ] Fallback recorded (D8): if the `swapgs` change fails its gate, the
      fallback is per-thread TLS emulated by `w32run` re-pointing the
      user GS base at each thread switch — slower, no kernel change.
      The fallback is designed in this phase but built only if the
      primary fails; building both speculatively is the dilution D1
      forbids.

#### Test gate

- Fixture: N threads × (TLS slots differ per thread, FLS callbacks fire
  on exit, critsec/SRW/InitOnce/event/mutex/semaphore/wait/APC/threadpool
  all exercised, `GetLastError` independent per thread). Runs under
  `-smp 4` and `-smp 1`, 50 consecutive boots, zero flakes tolerated —
  threading tests that flake are worse than no tests.
- Entry-path assertion suite: every transition covered, debug + release.
- **The whole suite is the gate**: `make test` (unit + full integration)
  green, plus the lx/i386/rv64/a64 parity lanes — `swapgs` touches
  shared entry assumptions, and the tenants prove nothing moved.
- Fallback decision recorded in the phase result: primary or fallback,
  with the measurement that decided it.

**Deliverable:** entry-path diff, TEB-lite, thread/sync/TLS
implementation, fixtures, `tests/integration/cases/test_w32a3_threads.sh`,
`patches/W32A3_threads.patch`.

---

### Phase W32A-4 — The table-driven SEH unwinder ⬜ PLANNED

**Objective:** replace the `sigsetjmp` shim with a real Win64 unwinder
over `.pdata`/`.xdata`, so faults and `RaiseException` run handlers *and*
cleanups frame by frame. No app gate before this (D7).

#### Tasks

- [ ] `.pdata`/`.xdata` parser (user space, in the loader): `RUNTIME_
      FUNCTION` lookup by PC (binary search — the measured tables run
      to 10 543 entries), `UNWIND_INFO` decode (all unwind codes
      including `UWOP_SAVE_XMM128`, chained info), hostile-input-hard:
      every RVA/count validated, the W32-2 fuzz-corpus treatment
      (truncated/bit-flipped `.pdata` must refuse, never walk off the
      image). The parser is the same code the host unit test and the
      loader use (the W32-3 D2 pattern).
- [ ] `RtlVirtualUnwind` + `RtlLookupFunctionEntry` +
      `RtlCaptureContext` + `RtlPcToFileHeader` + `RtlUnwind` +
      `RtlUnwindEx`, all REAL: handler invocation with `EXCEPTION_
      RECORD`/`CONTEXT`/dispatcher context, `ExceptionContinueSearch`/
      `ExecuteHandler`/`ContinueExecution` all honoured (the shim's
      `CONTINUE_EXECUTION` gap closes here), collided-unwind detection.
- [ ] `RaiseException` REAL (software exceptions, C++-runtime-raised
      included, dispatch through the same unwinder), `__C_specific_handler`
      REAL (filter/exec/finally semantics for compiler-generated
      `__try`), `__finally` REAL via unwind, `_XcptFilter` REAL,
      `SetUnhandledExceptionFilter`/`UnhandledExceptionFilter` REAL
      (the filter runs before the terminate box; the terminate box is a
      compositor dialog + serial dump, and its screenshot is a gate
      artefact).
- [ ] `_CxxThrowException` unwinds cleanups frame by frame and then, per
      D7, terminates with the named message (`W32-CXX-TYPED-CATCH-GAP`)
      instead of matching catch clauses. `?terminate@@YAXXZ` and
      `_purecall` terminate with their own named messages. This is the
      residue made executable: the behaviour is specified, tested, and
      greppable, not a crash shaped like a mystery.
- [ ] C++ destructors run during unwinding (the RAII fix that motivates
      the whole phase): cleanup funclets from `.xdata` execute in order;
      a throwing destructor terminates per the C++ rules (named message,
      not a hang).
- [ ] The old shim is removed, not kept as a fallback. Two exception
      systems is how a fault gets handled twice. `sehtest.c` is ported
      to the unwinder and extended, then the shim code is deleted in the
      same patch.

#### Test gate

- Host unit tests under ASan/UBSan: parser vs hostile corpus (every
  7-byte prefix + bit flips, the W32-2 sweep shape); unwind-output
  equivalence vs `llvm-readobj --unwind` on the repo's own binaries.
- Fixtures: nested `__try` (innermost wins), fault-in-deep-call (frames
  unwind in order, cleanups observed in order), `RaiseException` with a
  filter, `__finally` on the unwind path, `CONTINUE_EXECUTION` resumes
  at the faulting instruction, C++ destructor order across three frames
  (mingw-w64 C++ fixture), `_CxxThrowException` terminates with the
  named message, unhandled-exception filter runs before the dialog.
- `test_w32_integration` and the SEH receipt lines unchanged-or-extended;
  full `make test` green.

**Deliverable:** unwinder + parser, `RaiseException`/filter/terminate-box,
ported `sehtest`, `tests/integration/cases/test_w32a4_unwind.sh`,
`patches/W32A4_unwind.patch`.

---

### Phase W32A-5 — `USER32` breadth I: windows and messages, the `W` core ⬜ PLANNED

**Objective:** the window/message core all three gates stand on, `W`-first
per D6, over the compositor (no second window manager — D5).

#### Tasks

- [ ] Class/window lifecycle, `W` primary: `RegisterClassW`/
      `RegisterClassExW`/`UnregisterClassW`/`GetClassInfoW`/
      `GetClassNameW`/`GetClassNameA`, `CreateWindowExW` (all styles and
      ex-styles the ledger shows; unsupported styles refuse by name at
      creation, not silently), `DestroyWindow`, `DefWindowProcW`
      (default handling for the full ledger message set),
      `CallWindowProcW`, `GetWindowLongPtrW`/`SetWindowLongPtrW`/
      `GetWindowLongW` (subclassing REAL — the ladder subclasses its own
      controls), `GetPropW`/`SetPropW`/`RemovePropW`, `GetDlgCtrlID`.
- [ ] Messages: `SendMessageW`/`SendMessageA`/`PostMessageW`/
      `PostMessageA`/`GetMessageW`/`PeekMessageW`/`DispatchMessageW`/
      `TranslateMessage`, `SendDlgItemMessageW`/`SendDlgItemMessageA`,
      `RegisterWindowMessageW`/`RegisterWindowMessageA`,
      `GetMessageTime`/`GetQueueStatus`, `MsgWaitForMultipleObjects`
      (with W32A-3), `ReplyMessage`? (ledger decides), `InSendMessage`?
      (ledger). `WM_NOTIFY`/`WM_COMMAND` routing REAL (controls speak
      through these; W32A-8 depends on it). Cross-thread `SendMessage`
      blocks correctly (W32A-3 waits).
- [ ] Geometry/placement/visibility: `GetWindowRect`/`GetClientRect`/
      `GetWindowPlacement`/`SetWindowPlacement`/`ShowWindow` breadth/
      `IsWindowVisible`/`IsIconic`/`IsZoomed`/`BringWindowToTop`/
      `SetWindowPos` (Z-order/topmost/move/size/show flags REAL against
      the compositor's Z-order)/`MoveWindow`/`AdjustWindowRectEx`/
      `ClientToScreen`/`ScreenToClient`/`MapWindowPoints`/
      `WindowFromPoint`/`ChildWindowFromPointEx`/`GetAncestor`/
      `GetParent`/`SetParent`/`GetWindow`/`IsChild`/`EnumChildWindows`/
      `EnumThreadWindows`/`FindWindowW`/`FindWindowA`/`FindWindowExW`/
      `GetFocus`/`SetFocus`/`GetActiveWindow`/`SetActiveWindow`/
      `GetForegroundWindow`/`SetForegroundWindow`/
      `GetLastActivePopup`/`FlashWindow`/`FlashWindowEx`/
      `GetCapture`/`SetCapture`/`ReleaseCapture`/`SetLayeredWindowAttributes`
      (layered-alpha REAL where the compositor supports per-pixel alpha,
      documented approximation where it does not — the ledger's one
      NPP call is `SetLayeredWindowAttributes`, measured).
- [ ] Painting/scroll: `BeginPaint`/`EndPaint`/`GetUpdateRgn`/
      `ValidateRect`/`RedrawWindow`/`UpdateWindow`/`InvalidateRect`
      breadth/`LockWindowUpdate`, `GetScrollInfo`/`SetScrollInfo`/
      `GetScrollPos`/`SetScrollPos`/`GetScrollRange`/`SetScrollRange`/
      `ShowScrollBar`/`ScrollWindow` (REAL against the compositor's
      scroll areas), `GetDC`/`GetDCEx`/`GetWindowDC`/`ReleaseDC`
      (handles into the W32A-7 DC model — this phase mints them, W32A-7
      implements drawing).
- [ ] Rect/region helpers (pure, REAL): `EqualRect`/`InflateRect`/
      `IntersectRect`/`OffsetRect`/`PtInRect`/`SetRectEmpty`/
      `IsRectEmpty`? (ledger).
- [ ] System metrics/colours (REAL from the compositor theme):
      `GetSystemMetrics` (full ledger set — `SM_CXSCREEN` etc. from the
      framebuffer, `SM_CMONITORS`=1 documented), `GetSysColor`/
      `GetSysColorBrush`, `SystemParametersInfoA`/`SystemParametersInfoW`
      (the ledger's SPIs REAL — mouse/keyboard/UI-effect values from the
      compositor config; unlisted SPIs refuse by number), `GetDoubleClickTime`/
      `GetCaretBlinkTime`/`GetKeyboardType`? (ledger).
- [ ] Monitors (single-monitor REAL, documented): `EnumDisplayMonitors`/
      `GetMonitorInfoA`/`GetMonitorInfoW`/`MonitorFromWindow`/
      `MonitorFromRect`/`MonitorFromPoint` (one monitor, the framebuffer;
      multi-monitor is §7, and the functions say `1` rather than
      pretending).
- [ ] Keyboard/mouse input state: `GetKeyState`/`GetKeyboardState`/
      `SetKeyboardState`/`GetKeyboardLayout`/`MapVirtualKeyW`/
      `ToAscii`/`ToAsciiEx`/`GetCursorPos`/`SetCursorPos`? (ledger)/
      `mouse_event` (REAL into the input queue — synthesised input, the
      honesty note: it is genuinely injected, not drawn), `TrackMouseEvent`
      (REAL hover/leave tracking), `GetMessagePos`? (ledger).
- [ ] `EndDialog`-adjacent windowing leftovers owned here (not W32A-6):
      `GetDesktopWindow`, `GetShellWindow`? (ledger decides; likely
      FAIL-CLEAN — there is no shell window).

#### Test gate

- Fixtures: subclassed `W` window exchanging the ledger's message set
  with a second thread (cross-thread send/receive asserted),
  Z-order/placement/focus/capture scripted and screenshotted,
  scroll + paint + `RedrawWindow` asserted pixel-wise (VNC infra),
  metrics/colours asserted against the compositor theme (change the
  theme, the values change — proves REAL, not constants).
- Full `make test` green.

**Deliverable:** `w32/src/user32_win.c` (or equivalent split), fixtures,
`tests/integration/cases/test_w32a5_user32win.sh`,
`patches/W32A5_user32win.patch`.

---

### Phase W32A-6 — `USER32` breadth II: dialogs, menus, clipboard, resources ⬜ PLANNED

**Objective:** the dialog engine (the ladder's config dialogs live in
`.rsrc` templates), menus, timers, caret, accelerators, and the clipboard —
plus the PE resource parser everything dialog-shaped depends on.

#### Tasks

- [ ] Resource parser (extends `w32_pe`): `.rsrc` directory walk,
      `FindResourceW`/`FindResourceA`/`LoadResource`/`LockResource`/
      `SizeofResource`/`LoadStringW`/`LoadStringA`/
      `LoadBitmapW`/`LoadIconW`/`LoadIconA`/`LoadCursorW`/`LoadCursorA`/
      `LoadImageW`/`LoadImageA`/`LoadMenuW` (REAL; hostile-`.rsrc` fuzz
      corpus like W32-2 — resources are attacker-facing bytes),
      `EnumResourceNamesW`? (ledger), `GetIconInfo`/
      `CreateIconIndirect`/`DestroyIcon`/`DestroyCursor`/
      `CopyIcon`? (ledger). Icon/cursor decoding (ICO/BMP-in-PE) REAL —
      `DrawIconEx` (W32A-7) and `ExtractIconExW` (W32A-10) depend on it.
- [ ] Dialog engine: `DialogBoxParamW`/`DialogBoxParamA`/
      `DialogBoxIndirectParamW`/`CreateDialogParamW`/`CreateDialogParamA`/
      `CreateDialogIndirectParamW`/`EndDialog`/`DefDlgProcA`/
      `IsDialogMessageW`/`IsDialogMessageA`/`MapDialogRect`/
      `GetDialogBaseUnits`/`GetDlgItem`/`GetDlgCtrlID` (with W32A-5)/
      `GetDlgItemTextW`/`GetDlgItemTextA`/`SetDlgItemTextW`/
      `SetDlgItemTextA`/`GetDlgItemInt`/`SetDlgItemInt`/
      `CheckDlgButton`/`IsDlgButtonChecked`/`CheckRadioButton`,
      `SendDlgItemMessage` (with W32A-5). Dialog templates from `.rsrc`
      AND from memory (`Indirect` forms) — both measured (NPP uses
      indirect). Modal loop REAL (own message pump, correct owner-disable).
- [ ] Menus: `CreateMenu`/`CreatePopupMenu`/`DestroyMenu`/`LoadMenuW`/
      `GetMenu`/`SetMenu`/`GetSubMenu`/`AppendMenuA`/`AppendMenuW`/
      `InsertMenuW`/`InsertMenuA`/`InsertMenuItemW`/`ModifyMenuW`/
      `RemoveMenu`/`DeleteMenu`/`EnableMenuItem`/`CheckMenuItem`/
      `CheckMenuRadioItem`/`GetMenuItemCount`/`GetMenuItemID`/
      `GetMenuItemInfoW`/`SetMenuItemInfoW`/`GetMenuStringW`/
      `GetMenuState`/`SetMenuItemBitmaps`/`DrawMenuBar`/
      `GetSystemMenu`/`TrackPopupMenu`/`TrackPopupMenuEx` (REAL popup
      tracking with keyboard/mouse, cancellable — the compositor renders,
      the personality tracks), `GetMenuBarInfo`/`NotifyWinEvent`
      (accessibility events: accepted and logged, no consumer — the
      documented accept-and-log, same shape as W32A-2's restart case).
- [ ] Accelerators/timers/caret: `CreateAcceleratorTableW`/
      `LoadAcceleratorsW`/`TranslateAcceleratorW`/
      `DestroyAcceleratorTable`, `SetTimer`/`KillTimer` (REAL over the
      timer queue; `WM_TIMER` delivery asserted), `CreateCaret`/
      `DestroyCaret`/`SetCaretPos`/`ShowCaret`/`HideCaret`.
- [ ] Clipboard (REAL onto the kernel clipboard): `OpenClipboard`/
      `CloseClipboard`/`EmptyClipboard`/`SetClipboardData`/
      `GetClipboardData`/`IsClipboardFormatAvailable`/
      `RegisterClipboardFormatW`/`RegisterClipboardFormatA`/
      `GetClipboardOwner`/`SetClipboardViewer`/
      `ChangeClipboardChain` (viewer chain REAL — two viewers notified
      in order), `CountClipboardFormats`?/`EnumClipboardFormats`?
      (ledger). Formats: `CF_TEXT`/`CF_UNICODETEXT`/registered — REAL;
      `CF_HDROP` with W32A-11. Round-trip against native `/apps/gclip`
      asserted (the mapping has a native client — §2.4).
- [ ] Hooks: `SetWindowsHookExW`/`UnhookWindowsHookEx`/`CallNextHookEx`
      (NPP imports all three) → documented FAIL-CLEAN per hook type:
      thread-local `WH_CALLWNDPROC`/`WH_GETMESSAGE`? The phase *attempts*
      thread-local hooks REAL (no injection — same-process threads only,
      which is implementable); global hooks refuse by type. The gate
      asserts NPP's actual usage works (receipt probe in W32A-16 names
      which hook types NPP installs — measured, then implemented).
- [ ] Text drawing entry points owned here (drawing itself in W32A-7):
      `DrawTextW`/`DrawTextExW`/`DrawFocusRect`/`DrawEdge`/
      `DrawFrameControl`/`DrawIconEx` routing.

#### Test gate

- Fixtures: `.rsrc`-dialog app (template from resources renders, tab
  order works, buttons return values), memory-template dialog, modal
  over owner (owner disabled, asserted), full menu bar + popup tracking
  (scripted keys, screenshot), timer/caret/accelerator script,
  clipboard round-trip w32→native→w32, thread-local hook fixture (if
  REAL) or named-refusal fixture (if FAIL-CLEAN per type).
- Full `make test` green.

**Deliverable:** resource parser, dialog engine, menus, clipboard,
fixtures incl. `.rsrc`-bearing fixtures,
`tests/integration/cases/test_w32a6_user32dlg.sh`,
`patches/W32A6_user32dlg.patch`.

---

### Phase W32A-7 — `GDI32` breadth: DCs, blitting, regions, fonts ⬜ PLANNED

**Objective:** a device-context model with memory DCs and blitting, region
clipping, and font metrics — the drawing substrate PuTTY's terminal and
Notepad++'s editor paint through.

#### Tasks

- [ ] DC model: screen/window/memory DCs (`CreateCompatibleDC`/
      `CreateCompatibleBitmap`/`CreateDIBSection`/`DeleteDC`/
      `SaveDC`/`RestoreDC`), `SelectObject` (bitmap/pen/brush/font/
      region/palette — REAL selection with prior-object return),
      `GetCurrentObject`/`GetObjectA`/`GetObjectW`, mapping modes
      (`SetMapMode`/`GetMapMode`? (ledger)/`DPtoLP`/`LPtoDP`? (ledger)),
      origins (`SetWindowOrgEx`/`SetViewportOrgEx`? (ledger)/
      `OffsetWindowOrgEx`/`SetBrushOrgEx`), `GetDeviceCaps` (REAL caps
      incl. `LOGPIXELSX/Y` from the W32A-1 DPI record — honoured, not
      96-hardcoded), `SetROP2`/`GetROP2`, `SetBkMode`/`GetBkMode`/
      `SetBkColor`/`SetTextAlign`/`SetTextColor` (with existing),
      `GetPixel`/`SetPixel` (with existing).
- [ ] Blitting drawing: `BitBlt`/`PatBlt` (`StretchBlt`? ledger —
      the gap list shows PatBlt for NPP; StretchBlt NOT in the union,
      so D1 excludes it and the ledger records the exclusion),
      `GdiAlphaBlend` (REAL per-pixel alpha over the compositor blit),
      all raster ops the ledger shows (unsupported ROPs refuse by code),
      `GetDIBits`/`SetDIBits`/`CreateBitmap`/`CreateDiscardableBitmap`?
      (ledger).
- [ ] Pens/brushes/regions/paths: `CreatePen`/`ExtCreatePen`/
      `CreateSolidBrush` (with existing)/`CreateHatchBrush`/
      `CreatePatternBrush`/`CreateRectRgn`/`CreateRectRgnIndirect`/
      `CombineRgn`/`SelectClipRgn`/`GetClipRgn`/`ExcludeClipRect`/
      `IntersectClipRect`/`RectVisible`/`PtInRegion`? (ledger)/
      `EqualRgn`? (ledger), `Rectangle`/`Ellipse`/`RoundRect`/
      `Polygon`/`Polyline`/`FrameRect` (REAL rasterisation — the
      software renderer exists; these are new primitives in it, with
      host unit tests on pixel output).
- [ ] Fonts and text metrics (REAL against the shipped fonts):
      `CreateFontA`/`CreateFontW`/`CreateFontIndirectA`/
      `CreateFontIndirectW` (LOGFONT→font matching documented: family/
      size/weight mapped onto the shipped PSF + vector? — the phase
      records what ships; if the shipped set is bitmap-only, scaling is
      nearest and the docs say so), `GetTextMetricsA`/`GetTextMetricsW`/
      `GetTextExtentPoint32A/W`/`GetTextExtentExPointA/W`/
      `GetTextExtentPointA/W`/`GetCharWidthA/W`/`GetCharWidth32A/W`/
      `GetCharABCWidthsFloatA`/`GetOutlineTextMetricsA`/
      `EnumFontFamiliesExW` (REAL enumeration of the shipped set),
      `GetCharacterPlacementW`/`TranslateCharsetInfo`/
      `GetFontLanguageInfo`? (ledger), `ExtTextOutA`/`ExtTextOutW`/
      `DrawTextW`/`DrawTextExW` (with W32A-6), `TextOutA` (with existing).
      PuTTY's terminal metrics (`GetCharWidth*` family) are asserted
      against rendered glyphs, not constants.
- [ ] Palettes (PuTTY is a palette user): `CreatePalette`/
      `SelectPalette`/`RealizePalette`/`UnrealizeObject`/
      `UpdateColors`/`SetPaletteEntries`/`GetPaletteEntries`? (ledger)/
      `GetNearestPaletteIndex`? (ledger) — REAL 8-bit palette path.
- [ ] Icons on DCs: `DrawIconEx` (REAL from the W32A-6 icon decode).
- [ ] Printing-adjacent GDI owned here as FAIL-CLEAN: `StartDocW`/
      `EndDoc`/`StartPage`/`EndPage` (no printers — the calls exist and
      report it; the abort path `AbortDoc`? ledger).

#### Test gate

- Fixtures: mem-DC scene (shapes + text + blit + clip) asserted
  pixel-exact against a host-rendered reference (the reference is
  generated by the same raster code on the host — the libgl pixel-test
  pattern); font-metrics fixture asserting every metric against the
  shipped font files; palette fixture; DPI fixture (manifest-declared
  awareness changes `LOGPIXELSX/Y` — proves honoured, not hardcoded).
- Full `make test` green.

**Deliverable:** DC model + raster primitives + font layer, fixtures,
`tests/integration/cases/test_w32a7_gdi.sh`,
`patches/W32A7_gdi.patch`.

---

### Phase W32A-8 — `COMCTL32`: toolbar, status, listview, treeview, tabs, ImageLists ⬜ PLANNED

**Objective:** the common-controls DLL the 7-Zip and Notepad++ gates are
built from — v5 and v6 selected by the W32A-1 manifest record, controls
mapped onto `libauragui` widgets where they exist.

#### Tasks

- [ ] `InitCommonControlsEx`/`InitCommonControls` (ordinal 17 resolved —
      the measured `COMCTL32#(17)`; the map records what it is),
      version selection: v6 (themed, `UxTheme` calls issued where W32A-11
      implements them) vs v5 (unthemed) per the manifest record.
      `DllGetVersion`? (ledger).
- [ ] Windowed controls (all REAL, message-driven through the W32A-5
      `SendMessage` path, `WM_NOTIFY` to the parent REAL):
      toolbar (`CreateToolbarEx` + `TB_*` messages), status
      (`CreateStatusWindowW` + `SB_*`), listview (`WC_LISTVIEW` +
      the ledger's `LVM_*` set — 7-Zip's archive pane), treeview
      (`WC_TREEVIEW` + `TVM_*` — new `ag_add_tree` widget; there is no
      tree widget today and mapping a tree onto a listbox would be the
      D5 violation this task exists to avoid), tabcontrol
      (`WC_TABCONTROL` + `TCM_*` — onto `ag_add_tab`, extended where the
      message set exceeds it — Notepad++'s document tabs), tooltip
      (`TOOLTIPS_CLASS` + `TTM_*`), progress (`PROGRESS_CLASS` + `PBM_*`
      — onto `ag_add_progress`), header (`WC_HEADER` + `HDM_*` —
      listview's header, REAL), animation/hotkey/datetimepicker/
      monthcalendar/ipaddress/pager/nativetext? — ONLY if the ledger
      shows them (D1; the static tables show none — this list is the
      tripwire for receipt-probe additions, not a work list).
- [ ] `ImageList_Create`/`ImageList_Destroy`/`ImageList_AddMasked`/
      `ImageList_ReplaceIcon`/`ImageList_GetImageCount`/
      `ImageList_GetIcon`/`ImageList_GetIconSize`/`ImageList_GetImageInfo`/
      `ImageList_Draw`/`ImageList_SetIconSize`/`ImageList_Remove`/
      drag set (`ImageList_BeginDrag`/`ImageList_DragEnter`/
      `ImageList_DragMove`/`ImageList_EndDrag`/
      `ImageList_DragShowNolock` — REAL drag images during listview
      drags, asserted in screenshots), `_TrackMouseEvent` (with W32A-5),
      ordinals 381/410/411/412/413 resolved-or-refused by number.
- [ ] `PropertySheetW` (7-Zip's options) REAL: page dialog hosting over
      the W32A-6 engine, Apply/OK/Cancel semantics, `PSN_*` notifications.
- [ ] Theming behaviour: with v6 selected, controls issue the `UxTheme`
      calls W32A-11 implements; with v5 (no ladder manifest selects it —
      all three pin v6 — so the fixture proves the v5 path), controls draw
      unthemed. The gate asserts both renderings differ where the theme
      engine draws (proves the selection is honoured, not parsed-and-ignored).

#### Test gate

- Fixtures: one window per control family, scripted (insert/select/
  drag/scroll/notify asserted, screenshots for toolbar+status+listview+
  tree+tabs+tooltip+progress+propsheet), v5-vs-v6 rendering diff
  asserted, ordinal-17 fixture, delay-chain fixture with W32A-1.
- Full `make test` green.

**Deliverable:** `w32/src/comctl32.c` (+ `ag_add_tree` + tab/progress
extensions), fixtures, `tests/integration/cases/test_w32a8_comctl32.sh`,
`patches/W32A8_comctl32.patch`.

---

### Phase W32A-9 — Registry and `ADVAPI32`: the hive, SIDs, CryptoAPI, security stubs ⬜ PLANNED

**Objective:** a registry all three gates write real settings to, in a
hive format that is ours and documented — plus the security/CryptoAPI
remainder of `ADVAPI32`, each honestly classed.

#### Tasks

- [ ] The hive: format design (documented in `docs/win32.md`: key tree +
      value types `REG_SZ`/`REG_EXPAND_SZ`/`REG_DWORD`/`REG_QWORD`/
      `REG_BINARY`/`REG_MULTI_SZ`, file-backed, fsync policy,
      corruption behaviour — a torn write fails loud at next open, never
      half-reads), location (`/disk/w32hive` when `/disk` exists, else
      `/tmp/w32hive` with the volatility logged at boot — the `/opt`
      durability lesson from `docs/filesystem.md`, cited not repeated),
      predefined keys (`HKEY_CURRENT_USER` REAL root; `HKEY_LOCAL_MACHINE`
      REAL read-mostly with documented writable subtrees; `HKEY_CLASSES_ROOT`
      as the documented `HKLM\Software\Classes`+`HKCU` merge view).
- [ ] Registry API, all REAL, `A` + `W`: `RegOpenKeyEx`/`RegCreateKeyEx`/
      `RegCloseKey`/`RegQueryValueEx`/`RegSetValueEx`/`RegDeleteValue`/
      `RegDeleteKey`/`RegDeleteKeyEx`/`RegEnumKey`/`RegEnumKeyEx`/
      `RegQueryInfoKey`/`RegGetValue`/`RegEnumValue`? (ledger)/
      `RegFlushKey`? (ledger — fsync exists, so likely REAL).
- [ ] Identity/security: `GetUserNameA`/`GetUserNameW`
      (single-user name, documented), `AllocateAndInitializeSid`/
      `CopySid`/`EqualSid`/`GetLengthSid`/`FreeSid`/
      `CheckTokenMembership` (REAL against the single-user SID model —
      `Administrators` membership is TRUE with the single-user rationale
      recorded; this is the one place the plan sanctions "admin", and
      the rationale is one paragraph, not a pretence of ACLs),
      `InitializeSecurityDescriptor`/`SetSecurityDescriptorDacl`/
      `SetSecurityDescriptorOwner` (REAL descriptor building; enforcement
      is owner-only, documented), `IsTextUnicode` (REAL, pure).
- [ ] CryptoAPI mapped onto `libatls` (all REAL): `CryptAcquireContextW`/
      `CryptReleaseContext`/`CryptCreateHash`/`CryptHashData`/
      `CryptGetHashParam`/`CryptDestroyHash`/`CryptDeriveKey`?/
      `CryptEncrypt`?/`CryptDecrypt`? (ledger decides — the static tables
      show hash-only for NPP, so hash-only is the plan; the receipt probe
      may promote). Hash algorithms: the `libatls` set (SHA-256/512,
      SHA3); **SHA-1 is absent from `libatls`** — if the NPP receipt shows
      `CALG_SHA1` in a core flow, this phase implements SHA-1 in
      `libatls` (NIST-vectored, host-tested) rather than failing the
      call; if no receipt shows it, `CALG_SHA1` refuses by name. The
      phase result records which happened and cites the receipt.
- [ ] FAIL-CLEAN security set (documented, one reason each):
      `LsaOpenPolicy`/`LsaAddAccountRights`/`LsaClose`,
      `LookupAccountNameW`/`LookupPrivilegeValueW`,
      `OpenProcessToken`/`AdjustTokenPrivileges` (single-user, no
      privileges to hold — 7-Zip's backup-privilege path degrades to
      normal file access with 7-Zip's own error, asserted in the gate),
      `GetFileSecurityW`/`SetFileSecurityW` (owner + readonly mapping
      documented as an approximation — archive ACL preservation is the
      named casualty, recorded in the W32A-15 receipt expectations).
- [ ] `SystemFunction036` REAL via `getrandom` (the `7z.dll` single
      `ADVAPI32` import — `RtlGenRandom` semantics, documented alias).

#### Test gate

- Fixtures: registry CRUD + persistence across reboot (hive file survives
  — asserted via the FAT persistence harness shape), corrupt-hive
  fixture (torn write fails loud), SID/descriptor fixture, CryptoAPI
  hash-vs-`libatls`-vector fixture, privilege-degradation fixture
  (7-Zip's call sequence returns the documented errors, no crash).
- Full `make test` green.

**Deliverable:** hive + `w32/src/advapi32.c` (+ optional `libatls` SHA-1),
fixtures, `tests/integration/cases/test_w32a9_registry.sh`,
`patches/W32A9_registry.patch`.

---

### Phase W32A-10 — `SHELL32` + `COMDLG32` + `SHLWAPI` + `VERSION` ⬜ PLANNED

**Objective:** folders, file operations, the open/save dialogs, and the
small pure modules — the "application furniture" phases.

#### Tasks

- [ ] Known folders (REAL, documented mapping table):
      `SHGetFolderPathW`/`SHGetSpecialFolderPathW`/
      `SHGetSpecialFolderLocation` (`CSIDL_APPDATA` → the documented
      w32 data dir — Notepad++'s config home; `CSIDL_DESKTOP`/
      `CSIDL_STARTMENU`/… mapped onto real directories or refused by
      CSIDL with the reason named), `SHGetDesktopFolder`,
      `SHGetPathFromIDListW` (PIDL model: minimal-but-REAL —
      `SHGetSpecialFolderLocation`+`SHGetPathFromIDListW` round-trips,
      asserted; full PIDL algebra is §7 and the functions say which
      forms they accept), `SHCreateItemFromParsingName` (minimal REAL
      `IShellItem` for filesystem paths — Notepad++ imports it; the
      interface answers path queries, nothing more, and says so),
      `SHGetFileInfoW` (REAL type info + icons via W32A-6 decode),
      `ExtractIconExW` (REAL from PE resources + the file-type map).
- [ ] File operations and shell execution: `SHFileOperationW` (copy/
      move/delete/rename REAL over the VFS, no undo — `FOF_ALLOWUNDO`
      refused by flag, progress callbacks honoured), `SHBrowseForFolderW`
      (REAL folder picker over the W32A-6 dialog engine),
      `ShellExecuteW`/`ShellExecuteA`/`ShellExecuteExW` (verbs `open`/
      `runas`? — `open` on PE/ELF executes via spawn, on documents
      refuses "no associations" by name; `runas` refuses, there is no
      elevation — §7 consistent), `Shell_NotifyIconW` (REAL onto the
      compositor notification engine — add/modify/delete asserted with
      the notification visible in screenshots), `SHChangeNotify`
      (accepted; file dialogs refresh — the narrow REAL behaviour, not
      a broadcast system), `DragQueryFileW`/`DragQueryPoint`/
      `DragFinish` (REAL with W32A-11), `SHELL32#(165)`
      resolved-or-refused by number.
- [ ] Common dialogs (REAL over the W32A-6 engine):
      `GetOpenFileNameW`/`GetOpenFileNameA`/`GetSaveFileNameW`/
      `GetSaveFileNameA` (multi-select, filters, initial dir, overwrite
      prompt — the ledger's flag sets REAL, exotic flags refused by
      flag), `ChooseColorW`/`ChooseColorA`/`ChooseFontW`/`ChooseFontA`
      (REAL pickers — PuTTY's settings depend on both),
      `CommDlgExtendedError` (REAL), `PrintDlgW` → FAIL-CLEAN
      ("no printers", the `PDERR_NODEFAULTPRN` shape — printing itself
      is §7, the import binds and the dialog says so).
- [ ] Pure modules, all REAL: `SHLWAPI` `Path*` family
      (`PathCombineW`/`PathAppendW`/`PathRemoveFileSpecW`/
      `PathFindExtensionW`/`PathFindFileNameW`/`PathStripPathW`/
      `PathAddExtensionW`/`PathMatchSpecW`/`PathIsRelativeW`/
      `PathIsNetworkPathW`/`PathGetDriveNumberW`/
      `PathRemoveExtensionW`/`PathCompactPathExW`), `Color*`
      (`ColorHLSToRGB`/`ColorRGBToHLS`/`ColorAdjustLuma`),
      `AssocQueryStringW` (REAL "no associations" answers — the table is
      empty and documented, not missing); `VERSION`
      `GetFileVersionInfoSizeW`/`GetFileVersionInfoW`/`VerQueryValueW`
      (REAL from PE `VS_VERSION_INFO` — extends the W32A-6 resource
      parser; fixtures carry real version resources); `WININET`
      `InternetCrackUrlW` (pure parser, REAL); `dbghelp`
      `ImageNtHeader` (trivially REAL).
- [ ] FAIL-CLEAN network-identity set (updater-shaped, never core):
      `SensApi` `IsNetworkAlive`/`IsDestinationReachableW` (best-effort:
      attempt the documented probe — a TCP connect with a short timeout
      — and report the outcome; "assumed offline/online" is never
      hardcoded), `WINTRUST` `WinVerifyTrust` + `CRYPT32` eight
      (signature status "unknown", documented — no trust decisions are
      made on the answer, and the docs say the updater treats it as
      unverified), `dwmapi` ×2 (composition off, colourisation default —
      constants with the reason recorded), `mpr` `WNet*` ×6 (no network
      provider — reached via the W32A-1 delay path, asserted through it).

#### Test gate

- Fixtures: known-folder round-trips, file-op suite (copy/move/delete/
  rename incl. progress + cancel), open/save dialogs scripted
  (filter/type/select asserted), color/font pickers scripted, notify-
  icon lifecycle screenshotted, `SHGetFileInfoW`/`ExtractIconExW`
  against PE fixtures with real icons, `GetFileVersionInfo` against a
  version-stamped fixture, `InternetCrackUrlW` vector suite,
  `ImageNtHeader` self-check, every FAIL-CLEAN above asserting its
  documented error (not success, not crash).
- Full `make test` green.

**Deliverable:** `w32/src/shell32.c`, `w32/src/comdlg32.c`,
`w32/src/shlwapi.c`, `w32/src/version.c` (+ small stubs),
fixtures, `tests/integration/cases/test_w32a10_shell.sh`,
`patches/W32A10_shell.patch`.

---

### Phase W32A-11 — `OLE32`-lite, drag-and-drop, `OLEAUT32`, `IMM32` stubs ⬜ PLANNED

**Objective:** the narrow COM the census shows — init, a CLSID table, and
working drag-and-drop — plus the VARIANT helpers and the honest IME stubs.

#### Tasks

- [ ] COM-lite core (REAL, single MTA documented — no apartments, no
      marshalling, and the docs say the three words): `CoInitialize`/
      `CoUninitialize`/`OleInitialize`/`OleUninitialize` (nesting counts
      REAL), `CoTaskMemAlloc`/`CoTaskMemFree`/`CoTaskMemRealloc`?
      (ledger), `CLSIDFromProgID` (REAL over the committed ProgID table),
      `CoCreateInstance` REAL for the CLSIDs the receipt probe observes
      the ladder requesting (the phase's first task is that probe: a
      logging build records every CLSID+IID pair across scripted
      sessions of all three apps; each observed CLSID is implemented or
      refused by CLSID with the consequence named — e.g. "IFileDialog
      refused: the classic `GetOpenFileName` path serves file picking").
- [ ] Drag-and-drop (REAL within the documented scope):
      `RegisterDragDrop`/`RevokeDragDrop`/`DoDragDrop`/`ReleaseStgMedium`/
      `RegisterClipboardFormat` (with W32A-6) — file drops *into* app
      windows work end to end (`DragQueryFileW` yields real paths,
      asserted); drags *between* top-level windows work where the
      compositor routes them; OLE drag sources outside the personality
      do not exist and the docs say so. `OleSetClipboard`?/
      `OleGetClipboard`? (ledger decides).
- [ ] `UxTheme` (REAL onto the compositor theme engine):
      `OpenThemeData`/`CloseThemeData`/`DrawThemeBackground`/
      `DrawThemeTextEx`/`DrawThemeParentBackground`/`GetThemePartSize`/
      `GetThemeFont`/`GetThemeBackgroundContentRect`/
      `GetThemeTransitionDuration`/`SetWindowTheme`/
      `EnableThemeDialogTexture`/`BeginBufferedAnimation`/
      `EndBufferedAnimation`/`BufferedPaintStopAllAnimations`/
      `BufferedPaintInit`?/`BufferedPaintUnInit`? (ledger) — themed
      rendering where the theme engine draws the part, documented
      fallback where it does not (each fallback is a per-part record,
      not a blanket "unthemed"). The v5/v6 rendering-diff fixture from
      W32A-8 is extended to theme calls.
- [ ] `OLEAUT32` ordinals (REAL, small): `#2/#4/#6/#7/#9/#10/#149/#150`
      resolved to the documented `SysAllocString`/`SysFreeString`/
      `VariantInit`/`VariantClear`/`VariantCopy`/… set (the map cites
      the documentation per ordinal — no guessing, and any ordinal the
      docs do not pin stays refused by number). BSTR semantics REAL
      (length-prefixed UTF-16, embedded NULs, `SysStringLen` vs
      `wcslen` asserted apart).
- [ ] `IMM32` → documented FAIL-CLEAN stubs: `ImmGetContext`/
      `ImmReleaseContext`/`ImmGetCompositionStringW`/
      `ImmSetCompositionWindow`/`ImmSetCompositionFontA/W`/
      `ImmSetCandidateWindow`/`ImmSetCompositionStringW`/
      `ImmEscapeW`/`ImmNotifyIME` — IME is unavailable, composition
      reads empty, CJK input degrades to direct input. The stubs are
      specified per function (which return NULL, which return FALSE,
      which accept-and-ignore) and the gate asserts each — "IME stub"
      is not one behaviour, it is nine, and the ledger records all nine.

#### Test gate

- Fixtures: COM-init nesting + CLSID table queries (observed CLSIDs
  implemented, one unobserved CLSID asserting the named refusal),
  file-drop end to end (drop → `DragQueryFileW` paths → file contents
  read), themed-vs-unthemed control rendering diff, BSTR/VARIANT vector
  suite (embedded NULs, length semantics), IME stub-behaviour suite
  (nine assertions, one per function).
- The CLSID probe log is committed (runtime data, facts not code) and
  the claim checker pins the `CoCreateInstance` table to it.
- Full `make test` green.

**Deliverable:** `w32/src/ole32.c`, `w32/src/uxtheme.c`,
`w32/src/oleaut32.c`, `w32/src/imm32.c`, CLSID probe log, fixtures,
`tests/integration/cases/test_w32a11_ole.sh`,
`patches/W32A11_ole.patch`.

---

### Phase W32A-12 — `WS2_32` WinSock over the native socket stack ⬜ PLANNED

**Objective:** PuTTY's dynamically loaded network surface, real over
syscalls 300–307 — with the surface itself discovered by a logging run,
since static tables cannot show it (§2.6).

#### Tasks

- [ ] The logging run comes first: a `GetProcAddress`-logging build of
      the loader records every `WS2_32` name PuTTY resolves across a
      scripted Raw/Telnet/SSH session set. The recorded set becomes the
      phase's ledger (committed as runtime data); the tasks below are the
      expected superset, and anything unobserved stays unimplemented
      (D1 applies to dynamic surfaces too).
- [ ] Startup/errors (REAL): `WSAStartup`/`WSACleanup` (version
      negotiation REAL — 2.2 offered, lower accepted, higher refused by
      version), `WSAGetLastError`/`WSASetLastError` (per-thread, with
      W32A-3 — the `WSAEWOULDBLOCK`-after-`select` interleaving is
      asserted multithreaded).
- [ ] Sockets (REAL over syscalls 300–307 + the net stack): `socket`/
      `bind`/`listen`/`accept`/`connect`/`send`/`recv`/`sendto`/
      `recvfrom`/`shutdown`/`closesocket`/`select`/`ioctlsocket`
      (`FIONREAD`/`FIONBIO` REAL — PuTTY's async shape depends on
      non-blocking + `select`), `getsockopt`/`setsockopt` (the observed
      option set REAL — `SO_REUSEADDR`/`SO_KEEPALIVE`/`TCP_NODELAY`
      expected; unobserved options refuse by number),
      `getpeername`/`getsockname`, `WSAAsyncSelect`? — **only if the
      logging run shows it**: PuTTY's classic shape is async-via-window-
      messages, which would make this REAL-via-`PostMessage` task
      load-bearing; if the run shows select-threads instead, the task
      does not exist. The phase result cites the log either way.
- [ ] Resolution (REAL over the DNS stack): `getaddrinfo`/
      `freeaddrinfo`/`gethostbyname`/`gethostbyaddr`?/`inet_ntop`?/
      `inet_addr`?/`WSAAddressToString`? (the observed set — `getaddrinfo`
      is certain from the binary strings), `gethostname`, IDNA?
      refused-by-name unless observed (punycode is a plan of its own).
- [ ] Events/overlap (measured-or-stubbed): `WSACreateEvent`/
      `WSACloseEvent`/`WSASetEvent`/`WSAResetEvent`/
      `WSAWaitForMultipleEvents`/`WSAEventSelect`? — REAL if observed
      (mappable onto W32A-3 events + the socket poll), named-refusal
      otherwise. True overlapped completion (`WSAOVERLAPPED` + IOCP) is
      §7; the functions that only serve IOCP refuse by name.

#### Test gate

- The logging run is reproducible: same script, same recorded set, twice.
- Fixtures: blocking echo, non-blocking + `select` echo, `FIONREAD`
  framing, resolution vectors (`getaddrinfo` incl. failure modes),
  per-thread `WSAGetLastError` interleaving, `WSAAsyncSelect` *or*
  select-thread fixture per the log's verdict, graceful close + abortive
  close (`SO_LINGER`? only if observed).
- Loopback + QEMU-user-network echo servers in the integration harness
  (the `tcpserver` pattern, extended); full `make test` green.

**Deliverable:** `w32/src/ws2_32.c`, the `WS2_32` probe log,
fixtures, `tests/integration/cases/test_w32a12_winsock.sh`,
`patches/W32A12_winsock.patch`.

---

### Phase W32A-13 — The `msvcrt` bridge ⬜ PLANNED

**Objective:** the 34+22 `msvcrt` symbols the 7-Zip gate imports —
forwarded onto the native libc and the W32A-3/W32A-4 runtimes, with the
CRT heap unified with the process heap so `HeapSize` never lies.

#### Tasks

- [ ] Startup/exit (REAL): `__getmainargs`/`_acmdln` (w32 command line,
      with the `test_w32_argv`-proven parsing), `_initterm` (function-
      table range invocation — the CRT's static-initialiser twin of the
      `.CRT` handling), `_onexit`/`__dllonexit` (atexit chains, DLL-
      scoped REAL), `exit`/`_exit`/`_cexit`/`_c_exit` (exit-code paths
      asserted distinct: flush-vs-not, callbacks-vs-not),
      `__set_app_type`/`__setusermatherr` (REAL trivial),
      `_fmode`/`_commode` (REAL data exports with documented defaults).
- [ ] Heap unity (REAL, the phase's load-bearing invariant): `malloc`/
      `free`/`realloc`/`_msize`? (ledger — `HeapSize` covers it if
      absent) allocate from the process heap (`GetProcessHeap`), so
      `HeapSize(malloc(n))`, `HeapReAlloc` on CRT pointers, and
      `_msize` all agree. The gate asserts the agreement; two heaps
      that pretend to be one is the bug this invariant exists to forbid.
- [ ] Strings/memory (REAL, forwarded): `memcmp`/`memcpy`/`memmove`/
      `memset`/`strlen`/`strcmp`/`strstr`/`strchr`/`wcscmp`/`wcslen`/
      `wcsstr`/`rand`/`srand` (+ the `7z.dll`-only remainder:
      `realloc` joins heap unity; any further names the W32A-0 ledger
      shows for `7zG.exe`/`7z.exe` if the W32A-15 receipt pulls them in
      — console 7-Zip is a receipt-stretch, not a gate promise).
- [ ] Threading/exceptions (REAL onto W32A-3/W32A-4): `_beginthreadex`
      (REAL — `7z.dll`'s thread creation; handle semantics incl.
      `CREATE_SUSPENDED` + `ResumeThread` asserted, since `7z.dll`
      imports both), `_XcptFilter` (onto the unhandled-filter chain),
      `__C_specific_handler` (REAL filter dispatch into compiler-
      generated `__try` scopes), `_CxxThrowException`/
      `__CxxFrameHandler`/`?terminate@@YAXXZ`/`_purecall` (the D7
      residue, executable: unwind-cleanups-then-named-terminate;
      `??1type_info@@UEAA@XZ` REAL destructor — typeinfo teardown is
      not catch matching and stays implemented).
- [ ] `__dllonexit`/`_onexit` ordering across the DLL chain (with
      W32A-1 teardown order): LAST-IN callbacks first, asserted with a
      two-DLL fixture.

#### Test gate

- Fixtures: CRT-startup exe (args/initterm/onexit/exit-code matrix),
  heap-unity suite (`malloc`↔`HeapSize`↔`HeapReAlloc`↔`free` in every
  direction), `_beginthreadex` suspended/resume/affinity suite (with
  W32A-3), EH-name suite (filter dispatch REAL, typed-throw terminates
  named, purecall terminates named), two-DLL atexit-order fixture.
- Full `make test` green.

**Deliverable:** `w32/src/msvcrt.c`, fixtures,
`tests/integration/cases/test_w32a13_msvcrt.sh`,
`patches/W32A13_msvcrt.patch`.

---

### Phase W32A-14 — App gate I: PuTTY ⬜ PLANNED

**Objective:** the pinned `putty.exe` (0.85, `d01fdb5a…`) runs its §2.5
flows: config dialog, registry sessions, TCP/Telnet connection, rendered
terminal. First real application on the personality.

#### Tasks

- [ ] Receipt section `docs/w32app_receipts.md#putty` filled: pinned
      hash verified at receipt time (the harness hashes the user-supplied
      file and refuses to record against a different build — a receipt
      for the wrong binary is a false receipt), QEMU command line,
      serial excerpts with the gate's assertion phrases.
- [ ] Config dialog renders (screenshot asserted: category tree, host
      field, port field, connection-type radios — pixel-region checks,
      not hashes, with the tolerance policy from the GUI tests),
      edits persist to the registry (close, reopen, values intact —
      asserted through the hive file, not just the UI).
- [ ] Session save/load round-trip via the registry (PuTTY's `Reg*` `A`
      set exercised: save `test-session`, kill, reload, connect uses it).
- [ ] Raw/TCP connect to the harness echo server over QEMU user
      networking (send bytes, receive bytes, asserted); Telnet option
      negotiation against the harness Telnet stub (asserted option bytes,
      not "it connected"); SSH handshake against a test server to banner
      + key exchange (password auth asserted working; GSSAPI offered-and-
      absent asserted in the log — the §2.5 Kerberos note, proved).
- [ ] Terminal renders: scripted session output (ASCII + box-drawing +
      colours) screenshotted and region-asserted; font-chooser + colour-
      chooser dialogs open and apply (the `COMDLG32` six, exercised);
      clipboard copy from the terminal round-trips to native `gclip`.
- [ ] Negative flows asserted: serial session selected → the documented
      "no COM ports" message (not a hang); `SECUR32`/`GSSAPI64` absent →
      auth falls back (log shows the fallback, session proceeds).
- [ ] Fixture twin in CI: a mingw-w64 dialog+registry+dynamic-`WS2_32`
      fixture exercising the same personality paths (the receipt proves
      PuTTY; the fixture proves the paths stay green without PuTTY).

#### Test gate

- All receipt assertions pass against the pinned binary (human-run,
  pasted back, hash-verified); the fixture twin passes in CI;
  full `make test` green.

**Deliverable:** receipt section, fixture twin,
`tests/integration/cases/test_w32a14_putty_fixture.sh`,
`patches/W32A14_putty.patch` (personality fixes the gate forced —
expected non-empty; a gate that changes nothing is suspicious).

---

### Phase W32A-15 — App gate II: 7-Zip File Manager ⬜ PLANNED

**Objective:** the pinned `7zFM.exe` + `7z.dll` (24.09,
`dc4fdcd9…`/`88206394…`) list, extract and configure: the DLL-chain,
`msvcrt`, COMCTL32 and registry phases proved by a real consumer.

#### Tasks

- [ ] Receipt section `#7zip` filled (hash-verified both files).
- [ ] Launch: main window renders (listview + toolbar + status —
      region-asserted screenshots); `7z.dll` loads through the W32A-1
      chain (log shows chain + `DllMain` order + `CreateObject`
      binding); delay-load of `MPR.dll` fires on first network-folder
      touch and reports no provider (the W32A-1 delay path, observed
      live — not fixture-only).
- [ ] Archive listing: open a harness-built `.zip` and `.7z` (byte-known
      fixtures), listview rows asserted (names, sizes, dates);
      navigate into a folder, back out.
- [ ] Extract: extract-all to a directory, every file byte-compared
      against the harness originals (the gate's hard assertion —
      extraction that corrupts is worse than extraction that refuses).
- [ ] Options property sheet opens (the W32A-8 `PropertySheetW`), a
      setting changes, persists via the registry (asserted through the
      hive file), takes effect after reopen.
- [ ] File drop into the window adds to the archive flow (the W32A-11
      drop path, observed); `SHFileOperationW` delete/rename from the
      UI asserted on the VFS.
- [ ] ACL approximation named in the receipt (W32A-9 consequence):
      extract an archive carrying ACLs, assert AuraLite's documented
      owner-only mapping and the absence of crashes — the casualty is
      recorded where the user looks, not where the code hides.
- [ ] Fixture twin in CI: mingw DLL-chain + `msvcrt`-heap + listview +
      delay-load fixture covering the same paths.

#### Test gate

- Receipt assertions pass against the pinned binaries; byte-exact
  extraction; fixture twin green in CI; full `make test` green.

**Deliverable:** receipt section, fixture twin,
`tests/integration/cases/test_w32a15_7zip_fixture.sh`,
`patches/W32A15_7zip.patch`.

---

### Phase W32A-16 — App gate III: Notepad++ ⬜ PLANNED

**Objective:** the pinned `notepad++.exe` (8.8.9, `a470014b…`) edits:
launch, Scintilla view, tabs, open/save, Find, plugins, tray — the full
breadth, one application.

#### Tasks

- [ ] Receipt section `#notepad++` filled (hash-verified).
- [ ] The open-file path is traced FIRST (the §2.3 surprise: no
      `GetOpenFileName` import): a logging run records how the pinned
      build opens files (`SHCreateItemFromParsingName`? custom dialog?
      direct path entry?). The gate's file assertions follow the traced
      path — asserting a dialog that the application never opens would
      be testing the fixture, not the app.
- [ ] Launch: window + menu + toolbar + tab bar + status bar render
      (region-asserted); the three shipped plugins load (log shows the
      chain + `SHLWAPI` binding; plugin menu entries appear, asserted);
      tray icon appears as a compositor notification on minimise-to-tray
      (the W32A-10 mapping, observed).
- [ ] Edit: type into the Scintilla view (scripted keys), text renders
      (region-asserted incl. syntax colours for a known language file);
      tabs: open three files, switch, close one (asserted view contents
      per tab); save + save-as through the traced path (bytes asserted
      on the VFS); Find dialog opens, finds, replaces (asserted buffer).
- [ ] Offline-updater behaviour asserted: with no network, the updater
      path reports offline through the documented stubs (`SensApi`/
      `WININET`/`WINTRUST`/`CRYPT32`) and never crashes, hangs, or
      claims success (the D9 success-shaped-lie rule, observed live).
- [ ] Hooks observed: the receipt probe names which hook types NPP
      installs; the W32A-6 hook behaviour for those types is asserted
      working (REAL) or failing-clean (documented) — whichever the phase
      implemented, the gate checks the receipt matches the ledger class.
- [ ] Fixture twin in CI: mingw tabcontrol+`SHLWAPI`+plugin-DLL+drop
      fixture covering the same paths.

#### Test gate

- Receipt assertions pass against the pinned binary; fixture twin
  green in CI; full `make test` green.

**Deliverable:** receipt section, fixture twin, open-path trace log,
`tests/integration/cases/test_w32a16_npp_fixture.sh`,
`patches/W32A16_npp.patch`.

---

### Phase W32A-17 — App horizon: Audacity, or a documented gap list ⬜ PLANNED

**Objective:** attempt the next order of magnitude — and produce either a
receipt or the next plan's seed. Modelled on W32-7 ("`LoadLibrary`, or a
documented refusal"): the phase succeeds by deciding, with evidence.

#### Tasks

- [ ] Pin an Audacity version + hash (portable x64 zip), dump its ledger
      with the W32A-0 tool, and diff against the personality: the diff is
      committed as `w32/app_ledger/audacity-*.gap` whatever the outcome.
- [ ] Attempt launch under the receipt protocol. Expected missing (to be
      confirmed, not assumed): wxWidgets breadth beyond the ledger,
      `winmm`/`WASAPI` audio, possibly `GDI+` (absent from the ladder so
      far — the ledger diff will show it if Audacity needs it).
- [ ] Outcome A — runs with named degradations: receipt section filled,
      degradations listed (audio expected first among them), gate boxes
      check with the degradation list attached. The plan gains an app.
- [ ] Outcome B — does not run: the gap ledger + a launch log showing
      the first fatal gap become the committed artefacts; the phase
      result names the next plan (`W32D`? audio? wxWidgets?) and this
      plan's §7 gains the measured line. The plan gains a seed.
- [ ] No third outcome exists: "mostly runs, gaps unrecorded" fails the
      phase. The claim checker enforcesGate: receipt section filled XOR
      gap ledger committed, never neither, never "partially".

#### Test gate

- Either the receipt assertions pass, or the gap ledger + launch log are
  committed and cited in §7. Full `make test` green either way.

**Deliverable:** receipt section XOR gap ledger + launch log,
`patches/W32A17_audacity.patch` (empty of code in outcome B except the
ledger — an honest empty patch beats a padded one).

---

### Phase W32A-18 — Integration, documentation and the honest matrix ⬜ PLANNED

**Objective:** make it usable and make its limits legible (the W32-8 shape,
at 19-module scale).

#### Tasks

- [ ] `docs/win32.md`: the generated table extended to every module
      (`tools/gen_w32_api_table.py` extended past 3 modules; per-function
      D9 classes generated from the ledger, so the docs cannot drift
      from the classes); behaviour notes for every approximation
      (version-identity constant, single-user SIDs, PIDL subset, no-undo
      file ops, unthemed fallbacks, IME stubs, offline assumptions).
- [ ] `docs/w32app_receipts.md` complete: all gate sections filled or
      `AWAITING` with the reason; the "how to run a receipt" page tested
      by a second person following it blind (the docs' own gate — a
      protocol nobody but its author can follow is a diary, not a gate).
- [ ] `tools/check_w32app_claims.py` wired into `make test-unit`:
      artefact pins (patch + fixtures per `✅` phase) + receipt pins
      (section filled + hash line present per `✅` gate), with
      `--selftest` negative controls (the `check_lx_claims.py` shape:
      N phases, M artefact pins + K receipt pins, all printed).
- [ ] `docs/status.md` + `README.md` + `CHANGELOG.md` entries; the
      `w32` row graduates from 🧪 with the gate list attached (or stays
      🧪 with the reason — the verdict follows the receipts, not the
      ambition).
- [ ] `w32/examples/` gains the ladder-shaped examples (dialog app,
      listview app, delay-load app — mingw-w64, `w32-sdk-check`
      extended); the shell routing message covers the new refusals
      (ordinal/manifest/elevation/clsid — each refusal greppable).
- [ ] Residue sweep: every `static-only` marker left in the ledgers,
      every FAIL-CLEAN the gates never observed, and every §6 risk that
      materialised is either closed or entered into
      `docs/residue_ledger.md` with an owner phase of the next series.
      Nothing discovered by this plan stays discoverable-only-here.

#### Test gate

- Generated tables regenerate byte-identical in CI (the `sdk-check`
  pattern); the claim checker + selftests green; the receipt protocol
  followed blind by a second person; full `make test` green including
  every prior phase's gate.

**Deliverable:** docs, checker wiring, examples, ledger sweep,
`patches/W32A18_integration.patch`.

---

## 5. Order and rationale

| Phase | Why here |
|---|---|
| W32A-0 | Measurement before mechanism; the ledger is every breadth phase's spec |
| W32A-1 | The loader unlocks real-binary probing — every later phase debugs against it |
| W32A-2 | Files/time/process-info: no app draws a window before it reads the disk |
| W32A-3 | Threads + TLS before anything multithreaded (all three gates) can start |
| W32A-4 | The unwinder before the apps (D7) — faults must unwind before features exist to fault in |
| W32A-5 | Window/message core: the substrate dialogs, controls and GDI paint through |
| W32A-6 | Dialogs/menus/clipboard/resources: config dialogs are the first visible payoff |
| W32A-7 | GDI breadth: PuTTY's terminal paints through nothing else |
| W32A-8 | COMCTL32: needs windows (W32A-5), messages (W32A-5), themes (W32A-11 partial — the theme *calls* are stubbed until W32A-11, recorded) |
| W32A-9 | Registry: sessions/settings for all gates; CryptoAPI rides `libatls` |
| W32A-10 | Shell/dialogs: needs the dialog engine (W32A-6) and resources (W32A-6) |
| W32A-11 | OLE-lite: needs clipboard formats (W32A-6) and windows (W32A-5); themes complete W32A-8 |
| W32A-12 | WinSock: needs threads (W32A-3) and the logging loader (W32A-1); PuTTY's last dependency |
| W32A-13 | `msvcrt`: needs threads (W32A-3, `_beginthreadex`), unwinder (W32A-4, EH names), heap (W32A-2) |
| W32A-14 | PuTTY: needs W32A-1 – W32A-12 except COMCTL32/CRT (narrowest gate first) |
| W32A-15 | 7-Zip: adds COMCTL32 (W32A-8) + `msvcrt` (W32A-13) + DLL chains (W32A-1) |
| W32A-16 | Notepad++: the full breadth; nothing new, everything at once |
| W32A-17 | Audacity: the horizon decision, cheapest when the breadth exists to measure against |
| W32A-18 | Documentation last, when there is something true to document |

**If only one phase is ever built, build W32A-1.** It turns every ladder
binary from "refused at load" into a running probe that names its next
missing import — the whole plan's debugging posture in one phase — and it
needs no kernel change and no new modules.

**If the plan is abandoned after W32A-4**, the result is coherent: a
hardened loader, file/process breadth, real threading and a real unwinder
with fixtures — a defensible stopping point that stands without any
application.

**If the plan is abandoned after W32A-13**, the result is a broad
fixture-proved personality with no app receipts — useful, honest, and
clearly labelled as unreceipted. The gates add the applications, not the
mechanisms.

## 6. Risks

**Provenance contamination via the ladder.** The binaries are the most
tempting thing ever placed next to this tree: debugging Notepad++ without
peeking at its source requires discipline, and debugging *with* a peek is
undetectable after the fact. Mitigations: D10 stated before code (§1.3),
the §1.1 commit rule enforced by machine, receipts that cite runtime
observations (a receipt that reads like source analysis is rejected in
review). The rule is social; the commit gate is mechanical; both are named.

**Scope creep wears a ladder costume.** "Notepad++ needs just one more
control" is how a 19-phase plan becomes a 40-phase plan. Mitigation is D1:
the ledger admits functions, not arguments. A receipt probe that finds a
new dynamic surface amends the ledger through W32A-0's tool with a reason,
not through a phase quietly widening.

**The unwinder parses attacker-facing tables.** `.pdata`/`.xdata` arrive in
untrusted files; a malicious table must refuse, never walk off the image
or loop. Mitigation: the W32-2 treatment (validation-first parser, shared
host/loader code, fuzz corpus in the gate), plus unwind-depth and
handler-chain caps that terminate loud instead of recursing forever.

**`swapgs` touches every entry path.** A wrong-way swap corrupts
per-CPU state silently and crashes far away — the calling-convention risk
of `WIN32_PLAN.md` §6, transposed into the kernel. Mitigation: one phase,
entry assertions in CI debug builds, the *entire* suite (all widths, all
shards) as the gate, and the D8 fallback designed before it is needed.

**Typed C++ `catch` is the top technical unknown.** D7's residue is a bet
that the ladder's core flows never match a catch clause. The bet is
falsifiable per gate (the named-terminate message *is* the detector), but
if it falsifies on all three gates, the plan needs a `__CxxFrameHandler`
phase the size of W32A-4. That phase is not written here; §7 does not
forbid it, it prices it.

**The `WS2_32` surface is discovered, not specified.** If PuTTY's logging
run shows `WSAAsyncSelect` + completion ports, W32A-12 grows an IOCP task
currently sitting in §7. Mitigation: the run comes first in the phase, and
the phase result cites it — the plan bends at the advertised joint.

**Stub-vs-real misjudgement.** A FAIL-CLEAN that a core flow needed REAL
fails the gate late (W32A-14 – W32A-16), far from the phase that chose it.
Mitigation: every FAIL-CLEAN carries the receipt observation that chose
it, and the app gates run early private attempts ("does it start yet?")
that promote misjudged stubs before the formal receipt. The promotion is a
ledger edit with a reason (D9), not a quiet patch.

**Ordinal resolution sources may be thin.** mingw-w64 import data does not
promise every ordinal, and documented ordinals are scattered. Mitigation:
per-ordinal citation in the map; an ordinal nobody documents stays refused
by number, and the gate that needs it fails honestly until a source (new
mingw-w64 data, vendor docs) appears. The map never guesses.

**"It runs Windows programs" will be over-read.** The moment a PuTTY
screenshot exists, the claim will outrun the truth — the
`WIN32_PLAN.md` §6 risk, now with evidence-shaped fuel. Mitigations: §2.5
per-gate flow lists (quotable in the other direction), receipts that name
degradations first, and no claim of Windows compatibility anywhere (§7).

**Screenshot-test flakiness.** Region assertions with tolerances, font
rendering pinned to shipped fonts, theme fixed during tests — the GUI
tests' existing discipline, extended, not invented.

## 7. What this plan does not do

- No 32-bit PE, no WOW64 — deferred to a future `W32C` series (D3). The
  refusal stays and points here.
- No API-set (`api-ms-win-*`) forwarders — measured absent from the ladder
  (§2.3.8, D4). The ledger re-admits them the day a ladder binary imports one.
- No .NET/CLR images (all ladder `CLR headers` are empty — verified), no
  DirectX, no `GDI+` (measured absent), no printing beyond the
  fail-clean `PrintDlgW`.
- No full COM/OLE: no apartments beyond single-MTA, no marshalling, no
  ROT/monikers/embedding, no COM server registration (7-Zip's shell
  extension stays unrunnable — recorded in W32A-15).
- No Windows-hive compatibility: our hive format only (D5). No registry
  virtualisation, no transaction API (`RegCreateKeyTransacted` refused
  by name if the ledger ever shows it).
- No services/drivers, no 16-bit anything, no serial/COM ports (PuTTY
  serial stays refused with the message — W32A-14 asserts it).
- No audio (`winmm`/`WASAPI`): the expected first gap of W32A-17,
  named here in advance so its arrival is a confirmation, not a surprise.
- No Total Commander gate: proprietary, evidence uncommittable (§1.4).
  Users may run licensed copies against the breadth; the plan claims
  nothing about it.
- No completion ports (IOCP), no Job objects, no fibers beyond FLS
  (fibers themselves: refused by name unless the ledger shows them —
  it does not).
- No ABI stability guarantee for `w32` across releases (inherited from
  `WIN32_PLAN.md` §7), and no claim of Windows compatibility anywhere.

## 8. A note on why this ladder, in this order

The honest case for this plan is not "AuraLite will run Windows software".
It is that a hobby OS reaches a point where its foreign personality must
either meet real binaries or admit it is a fixture theatre — and this
repository, unusually, can meet them: it has the loader, the compositor,
the sockets, the testing culture, and now the measurements.

The ladder is ordered so that each application is a *diagnostic* for a
new layer, not a trophy: PuTTY diagnoses dialogs, registry and dynamic
network loading; 7-Zip diagnoses controls, DLL chains and the CRT; Notepad++
diagnoses everything at once, which is a different test from everything
separately. Audacity diagnoses the plan's own boundary, which is why its
phase accepts a gap list as success.

If the outcome is PuTTY with a receipt, 7-Zip with a receipt, Notepad++
with a receipt, and Audacity with a gap list — each saying exactly what
works and what does not — that is a real result, and this plan will have
been worth following.
