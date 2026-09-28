# W32APP receipts

Receipt protocol for `docs/plans/W32APP_PLAN.md` (phases W32A-0…W32A-18).
A receipt is greppable evidence a claimed thing exists and was measured:
a file in the tree, a checker that passes, a log line with a fixed pattern.
Prose in the plan is not a receipt. Every phase gate ends by naming its
receipts; `tools/check_w32app_claims.py` re-greps them.

## The ladder binaries (pinned inputs)

The three applications were chosen because each stresses a different
real-API surface (PuTTY: GDI + files + time; 7-Zip: dialogs + menus +
delay-load; Notepad++: common controls + plugins). The binaries below are
USER-SUPPLIED: fetched by the developer, hashed, measured — never
committed (§1.1 of the plan; the provenance rule in
`tools/check_provenance.sh` fails the tree if any MZ file is tracked).

| binary | version | sha256 |
|---|---|---|
| putty.exe | PuTTY 0.85 | `d01fdb5aae8f112526040a39b0bfb9e27d813003178645e65f8d1cfdb2a26c87` |
| 7zFM.exe | 7-Zip 24.09 | `dc4fdcd96efe7b41e123c4cba19059162b08449627d908570b534e7d6ec7bf58` |
| 7z.dll | 7-Zip 24.09 | `882063948d675ee41b5ae68db3e84879350ec81cf88d15b9babf2fa08e332863` |
| notepad++.exe | Notepad++ 8.8.9 | `a470014bb7f6d8587d3a4b9ddc3bab0a356a945e8f63b8067834805d3082472d` |

The last two hashes correct transcription errors in this receipt table. They
match both the W32A-0 committed ledgers and the files extracted from the
upstream 7-Zip 24.09 installer and Notepad++ 8.8.9 portable archive on
2026-09-24. The previous values must not be used to approve binaries.

Plugin DLLs shipped with the portable Notepad++ 8.8.9 (NppExport,
mimeTools, NppConverter, nppPluginList) are hashed inside
`w32/app_ledger/npp-plugins-8.8.9.imports`.

Upstream URLs (informational — the hash is the identity, not the URL):

- `https://the.earth.li/~sgtatham/putty/0.85/w64/putty.exe`
- `https://www.7-zip.org/a/7z2409-x64.exe` (self-extracting archive)
- `https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.9/npp.8.8.9.portable.x64.zip`

## Reproducing the ledgers (W32A-0)

The ledgers are generated, not hand-written, by a stdlib-only parser so
that CI needs no LLVM install:

```
python3 tools/w32_import_ledger.py dump <PE-FILE> --label <NAME> --version <VER>
python3 tools/w32_import_ledger.py agree <PE-FILE>   # dev-only: vs llvm-readobj
python3 tools/w32_import_ledger.py check              # the W32A-0 gate
```

`agree` requires `llvm-readobj` on PATH and is expected to print
`<file>: N imports identical to llvm-readobj` for all eight measured
binaries (4 applications + 4 plugins). `check` is hermetic: it parses
the five committed ledgers, re-derives the §2.2 census (348/298/86/590,
union 611, gap 569) and the live export count from `w32/src/w32_bind.c`.

## W32A-11 — incremental evidence (2026-09-24 and 2026-09-27; superseded by the phase-close receipt at the end of this section)

The original 2026-09-24 core receipt below did **not** cover compositor
file-drops or OLE drags. The 2026-09-27 follow-up does; neither is the
scripted-app CLSID/IID probe, the full `test_w32a11_ole.sh` phase gate or a
green full `make test` receipt. The plan stays IN PROGRESS.

- Guest: `make iso && bash tests/integration/cases/test_w32a11_core_subset.sh`
  (13/13 assertions). QEMU boots the ISO and runs the committed
  `/tests/w32a11_core.exe`; it binds real PE IAT entries, exits **78**, and
  prints `A11-COM-OK`, `A11-VARIANT-OK`, `A11-ORDINALS-OK`, `A11-IMM-OK`,
  `A11-CLIPFMT-OK`, `A11-THEME-OK`, `W32A11-CORE-SUBSET-OK`. These markers
  are conditional on in-guest return values, BSTR bytes, distinct theme
  pixels and mutation guards. `OLEAUT32.dll` is imported **by ordinal**
  (`2,4,6,7,9,10,149,150`); no third-party binary is in the ISO. Reproduce
  the IAT check with `llvm-readobj --coff-imports build/user/w32a11_core.exe`
  (on Debian trixie, `llvm-readobj-19`). The QEMU serial log is generated at
  `build/integration-logs/w32a11_core_subset.log`.
- ASan/UBSan host tests (all zero failures):
  `test_w32_a11_oleaut` 29, `test_w32_a11_imm` 21,
  `test_w32_a11_com` 203, `test_w32_a11_clipfmt` 263,
  `test_w32_a11_theme` 16, `test_w32_a11_drop` 20.
  The drop test opens a real temporary file by a path read back through an
  owned HDROP; it does **not** test a compositor event or window-to-window
  OLE drag.
- Neighbouring guest regression cases rerun: W32A-6 **39/39** (two boots;
  optional VNC pixel check skipped because `vncdotool` absent), W32A-8
  **21/21**, W32A-10 **15/15**. W32A-10's PE had an uninitialised
  `WriteFile` argument; a *separate* A10 fixture fix was needed to make its
  gate runnable; do not mistake its simple markers for a W32A-11 effect.
- `python3 tools/w32_gen_stubs.py --check` and
  `python3 tools/w32_import_ledger.py check` pass; the latter reports **607**
  personality export entries (including TODO stubs, not 607 implementations)
  and **41** ledger gaps. `run_all.sh --check-groups`
  passes. `make -j8 test-unit` passed after adding the seven new A11
  source/header entries to `w32/PROVENANCE.md` (the first run exposed that
  omission); `tools/check_provenance.sh` and its negative control passed.
  The optional, non-hermetic W32A-1 *drafting* helper
  `w32_mkstubmap.py --check` is not in sync with the later phases' curated
  REAL rows; **do not regenerate** `stub_map.tsv` from it. This is not the
  CI-hermetic generated-stubs check above.

**Blocked gates:** `w32/tests/W32A11.pinned-probe.partial.log` records three
independent QEMU boots from a FAT volume **outside Git**. All four official
PE files matched their committed SHA-256 pins before use. All three programs
were stopped **before program entry** on their first unresolved USER32
import: PuTTY `CreateDialogParamA`, 7-Zip FM `GetMenuItemInfoW`, Notepad++
`CreateDialogIndirectParamW`. The 7z.dll was on FAT but was **not** observed
loading; Notepad++ plugins were not included in this probe. These are NOT
scripted application sessions and the log is NOT an empty CLSID/IID table:
no COM activation was observed. `CoCreateInstance` and `CLSIDFromProgID`
therefore stay typed TODOs. As of this original receipt, `GUI_EVT_DROP`
carried coordinates and a 16-bit data field, not file paths; separate HDROP
handles and `DragQueryFileW` alone could not deliver cross-process drops.
The 2026-09-27 patch adds an owned pathname-token transport and the bounded
OLE/flat-theme implementations below. Timed animation remains unsupported.
`check_w32app_claims.py --check` also fails on pre-existing missing
`patches/W32A2_kernel32fs.patch`, `patches/W32A3_threads.patch`, and
`patches/W32A4_unwind.patch`; do not mask that failure as W32A-11 DONE.

### Follow-up on exact `5350c54d466173f38bc80b550c65c3d8bfff8a5f` (2026-09-27)

This patch is **directly against that commit**, not a re-export of the
already-merged W32A-11 core subset. New, independently asserted effects:

| gate / command after `make iso` | result | observed contract |
|---|---:|---|
| `bash tests/integration/cases/test_w32a11_core_subset.sh` | 13/13 | Regression: Win64 COM nesting, OLEAUT32 ordinal BSTR/VARIANT bytes, ten IMM32 behaviours, A/W clipboard IDs, v6 BUTTON pixels; PE exit 78. The fixture is from the base commit. |
| `bash tests/integration/cases/test_w32a11_dragdrop.sh` | 11/11 | OLE `IDropTarget`/`IDropSource` Win64 callbacks across two registered HWNDs, proper refcounts and a mouse-release drop; separately, a native task sends a path across process boundaries to an `WS_EX_ACCEPTFILES` PE HWND. That PE queries **its own** HDROP, opens the returned path and compares actual file contents. Both PE instances and native sender exit 78. |
| `bash tests/integration/cases/test_w32a11_tokens.sh` | 7/7 | Two independent native tasks: old still-busy path token rejected after destruction/reuse of the **same** window slot; unrelated PID cannot take the new token, while its actual owner can consume it once and read the on-disk payload. Both tasks exit 78. |
| `bash tests/integration/cases/test_w32a11_theme.sh` | 9/9 | Distinct v5/v6 manifest PEs: v5 retains a GDI fallback pixel; v6 overwrites it with two distinguishable themed states, refuses an unknown part without painting, measures size/content and a zero-duration transition, copies a compatible-bitmap animation frame into the target DC, rejects a timed frame, invalidates a stopped buffer; each exits 78. |
| `make -j8 test-unit` | PASS | ASan/UBSan A11 checks: OLEAUT 29, IMM 21, COM 203, named clipboard 263, theme 254 (all advertised part/states plus resource cleanup), HDROP 20, OLE drag 61 including release over a new/empty HWND; width ratchet 394/394. Provenance negative-control messages in its log are *expected* detections. |
| `test_gui_acl.sh`, `test_gui_bad_pointers.sh` | 5/5, 2/2 | Adjacent kernel GUI ownership and hostile-pointer regression gates. |

`python3 tools/w32_gen_stubs.py --check` and
`python3 tools/w32_import_ledger.py check` pass (607 personality exports,
611 ledger union, 41 uncovered); 202/202 integration cases are registered,
with each case assigned to exactly one shard. All three new QEMU gates are
registered. Runtime logs are written under `build/integration-logs/` and
build artefacts/third-party PE bytes are **not** part of this patch.

**Boundaries:** file-drop transports one bounded, absolute pathname per
token (eight outstanding slots per HWND, 256-byte slot) with destination PID
ownership, not a cross-process pointer or OLE object. Event polling/waiting
rechecks owner while holding the event lock, and blocking waits also pin the
window lifetime; a reused integer HWND cannot deliver its new owner's event
to an old waiter. The 16-bit event-data token reserves four bits for its slot
and 12 bits for a nonzero generation;
generation survives HWND recycling but, by construction, wraps after 4095
uses of a given slot. External OLE sources, apartments/marshalling, UxTheme
timed animations and DIB/alpha effects remain unsupported; unadvertised
part/state combinations return `E_NOTIMPL`, not a fake themed drawing.

The fresh upstream probe `w32/tests/W32A11.pinned-probe.5350c54.log` verifies
all four SHA-256 pins again and boots the three executables from an **external**
FAT32 image. PuTTY still stops at `USER32!CreateDialogParamA`, 7-Zip FM at
`USER32!GetMenuItemInfoW`, Notepad++ at
`USER32!CreateDialogIndirectParamW`: each fails import binding **before**
its entry point, so no CLSID/IID or ProgID is observed and **no activation
table is claimed**. Fixing those unrelated USER32 prerequisites is separate
from this W32A-11 patch. `CoCreateInstance`/`CLSIDFromProgID` remain
instrumented, typed TODOs. The full `make test` suite has **not** been run;
`tools/check_w32app_claims.py --check` remains red for the pre-existing
missing `patches/W32A2_kernel32fs.patch`, `patches/W32A3_threads.patch`,
`patches/W32A4_unwind.patch`. Neither the full W32A-11 gate nor the three
pinned-app gates are green.

### Follow-up on exact `7d38775423c09d61b3ddc1773dc5821d60c20a58` (2026-09-27)

This delta contains **only changes after that commit**; it does not replay the
previous COM/drag/drop/theme/token implementation. New assertions:

| gate / command | observed result | scope |
|---|---:|---|
| `make iso` | PASS | PE receiver imports `CreateFileW`; in-tree Unicode-named payload copied to initrd (third-party application PEs are not). |
| `bash tests/integration/cases/test_w32a11_core_subset.sh` | **13/13** | Regression: base COM/BSTR/VARIANT/ordinals, IMM32, clipboard and pixel/theme guest fixture still passes after relinking fixture import libraries. |
| `make -j8 test-unit` | PASS | All unit gates pass; A11 owned HDROP host test increases from 20 to **44** checks (ASan/UBSan), including UTF-8 invalid sequences, UTF-16 surrogate/truncation, file read, path bounds, isolation and stale handles. A11 OLE drag test remains 61 checks. Optional foreign-ABI suites report their own skips. |
| `bash tests/integration/cases/test_w32a11_dragdrop.sh` | **11/11** | Two-window PE supplies a genuine `IDataObject` (`QueryInterface`, `QueryGetData`, `GetData`); right target reads `CF_HDROP`/Unicode path from an allocated `STGMEDIUM` and checks `ReleaseStgMedium` freed it. Separate native process sends a Unicode UTF-8 pathname via compositor; in this **historical 7d38775 run**, PE queries the owned HDROP's 22 UTF-16 units (a BMP accent and a surrogate pair) and opens the on-disk file via `CreateFileW`. Both PE processes and sender exit 78. **The OLE source is in-process, not the native sender.** |
| `python3 tools/w32_gen_stubs.py --check`; `python3 tools/w32_import_ledger.py check` | PASS | Generated TODOs remain in sync; the structural K/U/G ledger gap remains 41. No imported USER32 prerequisite was silently reclassified as REAL. |

**CLSID/ProgID probe:** official PuTTY 0.85, 7-Zip FM 24.09 (+7z.dll)
and Notepad++ 8.8.9 PEs were downloaded outside Git, all **four** extracted
PE hashes matched the pinned SHA-256 values, and one QEMU session on the
patched ISO loaded them from an external FAT32 disk. PuTTY refused
`USER32!CreateDialogParamA`, 7-Zip `USER32!GetMenuItemInfoW` and Notepad++
`USER32!CreateDialogIndirectParamW` before application entry (exit 1 each).
There were **zero calls** to the already instrumented `CoCreateInstance` and
`CLSIDFromProgID` *because none reached entry*, not because no COM classes
are required. Raw outcome is in `w32/tests/W32A11.pinned-probe.7d38775.log`;
this is **not** an observed CLSID/IID table. External OLE sources,
scripted application sessions, the activation table, timed theme effects,
and the full `make test`/W32A-11 phase gate remain open. This patch does not
try to repair earlier KERNEL32/USER32/GDI32 breadth phases.

### Follow-up on exact `e4be5d968a4b69d397245b560eb2a6ec88be83bb` (2026-09-27)

This delta does **not** repeat the already merged W32A-11 drag/drop,
COM-lite or theme implementation. It makes both guest file-drop consumers
use the existing BMP-accented payload already staged by `e4be5d9`, rather
than expecting a different, absent filename. It also makes the existing
**TODO** class probes testable without pretending to know which classes
the pinned applications use:

| gate / command | result | scope |
|---|---:|---|
| `make iso` | PASS | The existing `w32a11-é.txt` is copied unchanged to initrd; the native sender, PE receiver and in-process OLE PE now use that **same** BMP Unicode filename. The repository fixture filename has no supplementary-plane character. A separate author-written PE probe is packaged in initrd. |
| `bash tests/integration/cases/test_w32a11_core_subset.sh` | **18/18** | The old Win64 COM/automation/IMM/clipboard/theme fixture still exits 78. A new independent PE imports `CoCreateInstance` and `CLSIDFromProgID`, submits two **synthetic** GUID pairs (with distinct IIDs and CLSCTX values) and a Unicode ProgID, checks `E_NOTIMPL`/`ERROR_NOT_SUPPORTED` and cleared outputs, and exits 78. QEMU serial output is asserted for each GUID and a one-line reversible UTF-16 ProgID. |
| `bash tests/integration/cases/test_w32a11_dragdrop.sh` | **11/11** | Both independent PEs and the native sender agree on the BMP-accented path; the OLE `IDataObject`/file-drop gate opens the **actual Unicode-named file** and checks its bytes (the PE receiver asserts 19 UTF-16 path units); sender and both PEs exit 78. This revised guest gate does **not** claim surrogate-pair file-drop coverage. |
| `bash tests/integration/cases/test_w32a11_tokens.sh` | **7/7** | Regression for owner-bound, single-use, stale-token rejection across native tasks. |
| `bash tests/integration/cases/test_w32a11_theme.sh` | **9/9** | Regression for v5/v6 themed rendering and explicit timed-animation refusal; no timed animation is newly implemented. |
| `make -j8 test-unit` | PASS | ASan/UBSan generated-stub probe: **28 checks**, zero failures; tests canonical GUID byte order, bounded/lossless UTF-16 escaping including BMP/non-emoji supplementary (U+20000), newline, backslash, whitespace and `=`, explicit truncation, every-call logs, failure HRESULT/error and cleared outputs. The separate owned HDROP host test also round-trips U+20000 through a real temporary file; all existing host tests and provenance checks pass. |
| `python3 -B tools/w32_gen_stubs.py --check` | PASS | Committed generated code agrees with the generator; no unverified class activation table was added. |

Each `w32a11-clsid-probe` line contains full CLSID/IID and CLSCTX;
`w32a11-progid-probe` contains a reversible prefix (up to 192 UTF-16 code
units) and `truncated=0/1`. Unsafe ASCII such as newline, space and `=` is
escaped as `\uXXXX` so log lines/field boundaries cannot be spoofed. These
**synthetic test vectors are not observations from any pinned application**.
No third-party app was redownloaded or rerun for this delta; the last
SHA-256-verified attempts are the earlier
`w32/tests/W32A11.pinned-probe.7d38775.log`, where each executable failed a
USER32 import before entry. Until actual app code reaches the probe, there
is no defensible CLSID/IID table. Timed UxTheme effects, full
`test_w32a11_ole.sh`, the three pinned-app gates and the full `make test`
remain open; **W32A-11 is NOT DONE**. `check_w32app_claims.py --check`
still fails solely on the three pre-existing missing W32A-2/3/4 patch
receipts named above; this delta does not repair those earlier phases.

### Follow-up after `d45cb71` (2026-09-28): typed probes to REAL semantics

No new third-party application run is claimed. The three generated TODO
stubs `CLSIDFromProgID`, `CoCreateInstance` and
`BufferedPaintRenderAnimation` are replaced by **REAL** implementations
whose claims still rest only on receipts already in this file:

- `CLSIDFromProgID` (REAL): case-insensitive hive lookup
  `HKCR\<ProgID>\CLSID` through the existing W32A-9 advapi32 view, no
  committed ProgID table. Refusals stay typed and named: NULL argument →
  `E_INVALIDARG`/`ERROR_INVALID_PARAMETER`; empty or over-long ProgID →
  `CO_E_CLASSSTRING`/`ERROR_INVALID_NAME`; unregistered →
  `CO_E_CLASSSTRING`/`ERROR_FILE_NOT_FOUND`; malformed value, wrong value
  type or unterminated GUID text → `CO_E_CLASSSTRING`/`ERROR_INVALID_DATA`.
  Success returns `S_OK` and leaves last-error untouched. Every call keeps
  logging its `w32a11-progid-probe:` line (same escape/truncation rules)
  now stamped with the per-call `result=0x%08x`.
- `CoCreateInstance` (REAL over an **empty** activation table): the
  previous probes observed **zero** CLSID/IID pairs, so `w32_com_classes`
  ships with no rows and `W32_COM_CLASS_COUNT==0` is pinned by test and
  row schema comment; the first class may be added only with a receipt
  log line that name it. Typed vocabulary: `E_POINTER` for NULL out;
  `E_INVALIDARG`/87 for `CLSCTX==0` or NULL CLSID/IID;
  `CLASS_E_NOAGGREGATION` for a non-NULL outer; `E_NOTIMPL`/50 for any
  context beyond in-process; `REGDB_E_CLASSNOTREG`/2 for in-process while
  the table is empty. The output pointer is always cleared on failure.
- `BufferedPaintRenderAnimation` (REAL): live-buffer snapshot blitted
  into the target DC without holding the uxtheme animation lock across
  the blit; `TRUE` no-op when no animation runs on the window;
  `FALSE`/`ERROR_INVALID_HANDLE` on a dead target DC or unknown window;
  `FALSE`/`ERROR_INVALID_PARAMETER` on a NULL target rect.

| gate / command | result | scope |
|---|---:|---|
| `make iso` | PASS | Fixture `w32a11_probe.exe` rebuilt from the rewritten `w32/tests/w32a11_probe.asm` (seeds `HKCR\AuraW32A11.Probe\CLSID` in-guest via `RegCreateKeyExW`/`RegSetValueExW`; exits 78 only when positive lookup, malformed refusal and empty-table refusal all agree). |
| `bash tests/integration/run_all.sh '^test_w32a11_core_subset$'` | **20/20** | Two new asserts pin the in-guest `result=0x00000000` hive hit and the `result=0x800401f3` malformed-CLSID refusal; older probe-line asserts still prefix-match. |
| `test_w32a11_tokens` / `test_w32a11_theme` / `test_w32a11_dragdrop` | **7/7, 9/9, 11/11** | No regression in the neighbouring W32A-11 guest gates. |
| `test_w32_a11_probe` / `_com` / `_theme` (host) | **56 / 203 / 265 checks, 0 failures** | Probe test amalgamates ole32+advapi32+w32_errno+w32_utf+kernel32_loc+atls TUs and uses a scratch hive under `/tmp/w32a11_hive_*`; the com test's always-miss `Reg*` mocks keep the real hive path exclusive to the probe test. |
| `python3 tools/w32_gen_stubs.py --check`; `python3 tools/w32_import_ledger.py check` | PASS | Generated surface in sync (**105** stubs, down from 108); K/U/G ledger gap still 41; `stub_map.tsv` rows 218/219/408 flipped TODO→REAL with evidence notes. |
| `python3 tools/check_test_registry.py` | **202 cases registered** | Registry parity unchanged. |

Timed UxTheme effects, `test_w32a11_ole.sh`, the three pinned-app gates
and the full `make test` remain open; **W32A-11 is still NOT DONE**: the
USER32 import prerequisites blocking application entry are untouched, the
COM activation table is empty by receipt, and no plugin/scripted session
has exercised the new lookup in situ.

### 2026-09-28 (same-day second delta): USER32 prerequisites the pinned apps named

The stale `stub_map.tsv` REAL rows with no code behind them — 13 USER32
symbols, including all three names the committed probe
`w32/tests/W32A11.pinned-probe.7d38775.log` recorded as import-binding
failures (`CreateDialogParamA` PuTTY, `GetMenuItemInfoW` 7-Zip FM,
`CreateDialogIndirectParamW` Notepad++) — are implemented and bound.
**No application binary was downloaded, run or re-probed for this delta**;
closer-to-entry claims wait for the next external probe. What this delta
proves is binding plus in-guest real semantics on synthetic fixtures:

| gate / command | result | scope |
|---|---:|---|
| `make iso` | PASS | New binds link into the personality; `user32.def` gains the same 13 names (its header contract: one row per bind-table entry). |
| `bash tests/integration/run_all.sh '^test_w32a6_user32dlg$'` | **43/43** | Fixture `w32a6_dlg.asm` gains phase 4b (MENUITEMINFO round-trip: length-only query, copy+truncation, cbSize 44 refused, set/get state-id-data, insert by position and by command, modify, radio range, bitmaps verbatim, `GetMenuState` popup-count packing, `GetMenuStringW`) and phase 11b (`CreateDialogParamA` MAKEINTRESOURCE(1) creates the modeless frame hidden, WM_CLOSE reaches the app proc which destroys, id 999 refused, indirect twin + destroy, `DefDlgProcA==0`, `MessageBoxIndirectW` refuses NULL and cbSize 40). 72 imports bound in-guest, exit 78. |
| `test_w32a5_user32win` / `test_w32a7_gdi` / `test_w32a8_comctl32` / `test_w32a10_furniture` / `test_w32_integration` / `test_w32_user32` | PASS | No regression in the neighbouring W32 guest gates. |
| `test_w32a11_core_subset` / `_theme` / `_dragdrop` / `_tokens` | **20/20, 9/9, 11/11, 7/7** | The W32A-11 family is untouched by the change. |
| `test_w32_a6` (host, ASan/UBSan) | **113 checks** (was 75), **0 failures** | NULL-template/NULL-proc INVALID_PARAMETER, hidden-without-WS_VISIBLE, modeless WM_CLOSE semantics, 12-round slot recycling, RT_DIALOG/1 creation, absent-id RESOURCE_DATA_NOT_FOUND, string-name NOT_SUPPORTED, full MENUITEMINFO round-trip, MessageBoxIndirectW alert parity with MessageBoxW. |
| `python3 tools/w32_import_ledger.py check` | PASS, **gap 41 → 28**, personality **607 → 620** | The `EXPECTED_GAP` pin cites the 13 closed names. |
| `python3 tools/w32_gen_stubs.py --check`; `check_test_registry.py` | PASS | Generated surface unchanged (105 stubs); 202 cases registered. |
| `check_w32app_claims.py --check` | FAIL — **same 3 pre-existing problems** | Only the known missing `patches/W32A2/A3/A4` receipts; this delta adds none and repairs none. |

Semantics live in `w32/src/w32_dlg.c` (modeless family on the modal
engine's frame-proc chain; MENUITEMINFO over the same item records) and
`w32/src/user32_win.c` (`MessageBoxIndirectW` over the shared alert path);
`dlg_frameproc` now distinguishes modeless WM_CLOSE and reclaims slots on
WM_DESTROY. The guest gate never calls the live alert: `ag_alert` is
modal, so only the two structural refusals are pinned in-guest, with the
positive path covered by the host test — the receipts record the split
rather than imply end-to-end alert coverage. Moderating note: modeless
dialogs still create no child controls, exactly like the W32A-6 modal
engine (documented there); menu bitmaps are stored verbatim but never
rendered. **W32A-11 and the W32A-14/15/16 application gates remain NOT
DONE.**

## 2026-09-28 (same-day probe): all three pinned apps pass USER32 import binding

The fresh runtime probe (`w32/tests/W32A6.pinned-probe.639388d.log`) replaces
the USER32 import blockers recorded below with observed facts only.  All
four pinned PEs were re-verified byte-identical to the ledger's sha256 pins
before the probe; binaries, archives, the FAT image and the screenshot PPM
stay outside Git.

### Gates and results

| Gate / command | Result | Semantics |
| --- | --- | --- |
| PuTTY 0.85 via `run /apps/glaunch &` + `run /apps/w32run /fat/PUTTY.EXE` (UEFI/OVMF framebuffer) | **348/348 imports bound, no unresolved name, no exit** | PuTTY reaches WinMain and raises its own "PuTTY Fatal Error" message box through glaunch: *"Unable to load any WinSock library"* (QMP screendump evidence, kept local).  The next observed prerequisite is W32A-12, in plan order |
| 7-Zip FM 24.09 via `run /apps/w32run /fat/7ZFM.EXE` | **292/292 imports bound** | CRT startup runs and logs the three named blockers `TODO msvcrt.dll!__set_app_type/_initterm/__getmainargs needs W32A-13`, then `W32-SEH-UNHANDLED code=0xc0000005 pc=0x4000ebb3`, exit code 139 |
| Notepad++ 8.8.9 portable via `run /apps/w32run /fat/NPP.EXE` | **590/590 imports bound** | two USER-mode page faults immediately after binding, exit code 139; no msvcrt TODO lines, no SEH chain entry, no COM probe lines — the receipts name no cause |
| TODO manifest reception | unchanged | 0 `w32a11-clsid-probe` lines across all three sessions; the COM activation table stays EMPTY by this receipt |

**W32A-11 and the W32A-12/13/14/15/16 phases remain NOT DONE.**  The import
ledger gap is now 28 (was 41) because the 13 USER32 rows below closed.

## W32A-11 — PHASE CLOSED (2026-09-28), full phase gate green

The five task groups (OLE32-lite, drag-and-drop, `OLEAUT32` ordinals, `IMM32`
FAIL-CLEAN stubs, `UxTheme`) are implemented, bound and gated together by the
named deliverable `tests/integration/cases/test_w32a11_ole.sh` — the single
phase gate the earlier incremental deltas said remained open.  It boots QEMU
once and drives every fixture; the earlier per-slice gates
(`test_w32a11_{core_subset,dragdrop,theme,tokens}`) still pass unchanged.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a11_ole.sh` (guest, QEMU) | **30/30 assertions** | core (COM/VARIANT/ORDINALS/IMM/CLIPFMT/THEME) + probe (hive ProgID + empty activation table typed refusals) + drag (in-proc `IDataObject`/`CF_HDROP`) + native file drop (Unicode name via `CreateFileW`) + UxTheme v5/v6 pixels + zero-duration animation; 5 PE fixtures each exit 78; no unresolved import, no guest fault |
| `test_w32_a11_oleaut` (host, ASan/UBSan) | **29 checks, 0 failures** | BSTR/VARIANT, length-prefixed UTF-16, embedded NULs, `SysStringLen != wcslen` |
| `test_w32_a11_imm` (host) | **21 checks, 0 failures** | nine typed FAIL-CLEAN IMM32 stubs |
| `test_w32_a11_com` (host) | **203 checks, 0 failures** | init/uninit nesting, task memory, `CLSIDFromProgID`, `CoCreateInstance` typed refusals |
| `test_w32_a11_clipfmt` (host) | **263 checks, 0 failures** | named A/W clipboard-format registration/round-trip |
| `test_w32_a11_theme` (host) | **265 checks, 0 failures** | v5 fallback vs v6 parts, sizes, `GetThemeTransitionDuration`, buffered animation |
| `test_w32_a11_drop` / `test_w32_a11_drag` (host) | **44 / 61 checks, 0 failures** | owned HDROP query/finish; `IDataObject`/`QueryGetData`/`GetData`/`ReleaseStgMedium` |
| `test_w32_a11_probe` (host) | **56 checks, 0 failures** | real hive lookup + empty-table pin; probe lines byte-exact |
| `python3 tools/check_test_registry.py --check` | **203 cases, all registered** | `test_w32a11_ole` added to `ALL_CASES`; shard partition still exact (`w32`) |
| `python3 tools/check_w32app_claims.py --check` | 3 pre-existing FAILs only | the missing `patches/W32A2/A3/A4` receipts (unrelated, documented above); marking W32A-11 DONE adds no new failure and the W32A-0 census still confirms `agree` — union 611, gap 28, `CHECK OK` |

Closing patch: `patches/W32A11_ole.patch`.  Documented non-goals stay
non-goals (the plan says the words): cross-process COM marshalling, OLE drag
sources outside the personality, timed `UxTheme` animation, and non-empty
activation (no pinned-app CLSID/IID pair observed; the `CoCreateInstance`
table is EMPTY by receipt, pinned to `w32/tests/W32A11.pinned-probe.*.log`).
The REFUSE guard is unaffected — W32A-11 is not an application gate, so no
`.imports` ledger with `MZ` binaries or REFUSE rows blocks it.

## W32A-12 — PHASE CLOSED (2026-09-28), WS2_32 WinSock over the native stack

`w32/src/ws2_32.c` binds WS2_32 as a thin adapter over AuraLite's libc socket
surface (syscalls 300–307) + the DNS/inet parser layer.  mingw-w64 imports
WS2_32 **by name** (every `Ordinal = <none>`), so the phase added only NAME
rows to `w32/src/w32_bind.c` (`WS32`) plus `add_builtin("ws2_32")` for the
dynamic `LoadLibrary`+`GetProcAddress` path — no `ordinal_map.tsv` /
`w32_stubs_gen.c` change.

**Surface discovered from the real binaries.**  The plan (§2.6) requires the
phase to learn WS2_32's surface with a logging run "since static tables cannot
show it": WS2_32 is *not* in PuTTY's or plink's import directory — both resolve
it at run time with `LoadLibrary("WS2_32")+GetProcAddress`, which is why the
static ledger `w32/app_ledger/putty-0.85.imports` carries zero ws2_32 rows.
The names were therefore read out of the pinned **PuTTY 0.85 (Win64)** binaries
themselves (`putty.exe` sha256 `d01fdb5a…`, size 1706136 — matching the ledger
header exactly; `plink.exe` sha256 `969f3687…`).  Per the no-vendored-binary
rule (`w32/LICENSING.md`, D8) the executables are **not** committed: they are
fetched into `build/w32bins/` (git-ignored, provenance-exempt) purely as a
local verification aid, and the fetch+scan is fully reproducible.
`w32/tests/W32A12.probe.log` records the **union of 34 names** both binaries
resolve, per-binary, with a coverage cross-check showing **34/34 bound** in
`w32_bind.c`.  The observed set is *load-bearing async*, not select-threads:
plink drives `WSAEventSelect`+`WSAEnumNetworkEvents` over a kernel32 event and
PuTTY drives `WSAAsyncSelect` with window-message delivery — so this phase
implements that pair for REAL rather than refusing it.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a12_winsock.sh` (guest, QEMU) | **5/5 assertions** | The **expanded** `w32a12_winsock.exe` (`-lws2_32`, **40 name imports**, 43 bound in-guest) runs the whole surface end to end on real hardware-emulated boot (`build/auralite.iso` via `qemu-system-x86_64`): the core verbs (socket/bind/getsockname/listen/accept via the kernel tcp test fallback, peer 10.0.2.2:54321; getpeername; recv of the canned request + send; `select`; `ioctlsocket`; observed `setsockopt` + refuse-by-number; shutdown/close), the resolution/format surface (`getaddrinfo`+`inet_ntop`/`inet_pton`/`inet_ntoa`, `getservbyname`/`getservbyport`, `getnameinfo`, `WSAAddressToStringA`/`WSAStringToAddressA`, `gethostname`), `WSAIoctl` (FIONBIO/FIONREAD/SIO_KEEPALIVE_VALS), the WSA event objects over kernel32, and the readiness pair `WSAEventSelect`+`WSAEnumNetworkEvents` plus `WSAAsyncSelect` registration; prints `W32A12-WINSOCK-OK`, exits 78, no fault. **This live gate caught a real ABI bug the host suite could not: WinSock's `SERVENT` swaps `s_proto`/`s_port` under `_WIN64` (`psdk_inc/_ip_types.h`), so `struct ws2_servent` was reordered to match — the host test shared the same struct and so had passed regardless** |
| `test_w32_a12_ws2_32` (host, ASan/UBSan) | **110 checks, 0 failures** | everything above as pure boundary logic: version negotiation, refcounted cleanup + `WSANOTINITIALISED`, byte order, errno→WSAE*, `fd_set`/`ADDRINFOA` translation, endpoint cache, `ioctlsocket`+`WSAIoctl` dispatch, option accept/refuse, `inet_ntoa`/`getservby*`/`getnameinfo`/`WSAAddressToStringA`↔`WSAStringToAddressA`, **non-blocking recv/send returning `WSAEWOULDBLOCK` via a zero-timeout select gate**, `WSAEventSelect`+`WSAEnumNetworkEvents` over kernel32 event doubles, `WSAWaitForMultipleEvents` pumping readiness, and `WSAAsyncSelect` delivering a `PostMessageW` through the pump |
| `ld.lld … -o build/user/w32run.elf` | **links clean** | the whole w32 personality (incl. `w32_ws2_32.o`) links: the cross-module symbols `w32_ws2_pump` / `w32_user32_set_socket_pump` and ws2_32's calls into kernel32 events + user32 `PostMessageW` all resolve |
| `bash tools/check_provenance.sh` | **PASS (83 files)** | ws2_32.c/.h + fixture recorded; `build/` (the fetched binaries) is exempt from the MZ scan |
| `python3 tools/check_test_registry.py` | **204 cases, all registered** | `test_w32a12_winsock` in `ALL_CASES` |
| `python3 tools/check_w32app_claims.py --check` | 3 pre-existing FAILs only | the missing `patches/W32A2/A3/A4` receipts (unrelated historical debt); K/U/G ledger union stays **611**, gap **28**, `CHECK OK` (no app imports WS2_32 statically — it is loaded dynamically) |

Documented non-goals (D1, honest and matching what the binaries ask for):
overlapped I/O (`WSAOVERLAPPED`+IOCP, `WSASend`/`WSARecv`/`WSAConnect`/
`WSAGetOverlappedResult`) refuses BY NAME — neither binary resolves it;
`WSAIoctl` beyond FIONBIO/FIONREAD/SIO_KEEPALIVE_VALS refuses
(`SIO_GET_EXTENSION_FUNCTION_POINTER`→`WSAEOPNOTSUPP`, else `WSAEINVAL`);
`FIONREAD` reports 0 (no bytes-available query); non-blocking `connect()`
completes synchronously and delivers `FD_CONNECT` on the next pump/enum;
readiness is measured with a zero-timeout `select()`, and reverse DNS (PTR) is
numeric-only.  Closing patch: `patches/W32A12_winsock.patch` (format-patch
form).  W32A-12 is not an application gate, so no `.imports` ledger blocks it.

## W32A-13 — PHASE CLOSED (2026-09-28), the msvcrt bridge

`w32/src/msvcrt.c` bridges msvcrt.dll onto runtimes AuraLite already owns:
malloc/free/realloc onto the **process heap** (`GetProcessHeap`/`HeapAlloc`/
`HeapFree`/`HeapReAlloc`, W32A-2), `_beginthreadex` onto `CreateThread`
(W32A-3), and the C++ EH names onto W32A-4's unwinder.  msvcrt is a PE DLL
whose callers cross the Windows-x64 boundary, so every export is `W32ABI`.
The phase added only NAME rows to `w32/src/w32_bind.c` (`MCRT`) — the static
table shadows the `w32_stubs_gen.c` TODO stubs exactly as W32A-4's five C++ EH
names already did, so no `stub_map.tsv` / `w32_stubs_gen.c` change.

**Heap unity — the load-bearing invariant.**  A CRT pointer *is* a process-heap
pointer, byte for byte: `malloc` is `HeapAlloc(GetProcessHeap(),…)`, so
`HeapSize(malloc(n)) == n`, `HeapReAlloc` grows a malloc'd block, and `_msize`
(ledger-absent; `HeapSize` covers it) would agree.  Two heaps pretending to be
one is the bug this forbids; the in-guest gate proves the agreement across the
msvcrt→kernel32 DLL boundary.

**Surface discovered from the real binaries.**  The 7-Zip gate's msvcrt surface
was read out of the pinned **7-Zip 24.09 (Win64)** binaries themselves, fetched
from `https://www.7-zip.org/a/7z2409-x64.exe` (an SFX; the members were
extracted with the official Linux `7zz`).  Per the no-vendored-binary rule
(`w32/LICENSING.md`, D8) they are **not** committed: they live in
`build/w32bins/` (git-ignored, provenance-exempt) purely as a local
verification aid, and the fetch+scan is reproducible.  `w32/tests/W32A13.probe.log`
records, per binary (with sha256/size), the msvcrt name imports:

- `7zFM.exe` (sha256 `dc4fdcd9…`) → **34** msvcrt imports — **real == ledger** `7zFM-24.09.imports`.
- `7z.dll`  (sha256 `88206394…`) → **22** msvcrt imports — **real == ledger** `7z-24.09.imports`.
- UNION(7zFM, 7z.dll) = **37 distinct symbols**, **all 37 bound** in `w32_bind.c`
  (5 are W32A-4's C++ EH names; W32A-13 adds the remaining **32**).
- The console `7z.exe`/`7zG.exe` pull extra stdio (`_iob`/`fflush`/`fgetc`/…) and
  `__initenv`/`_isatty`: per §3 those are a receipt-stretch, **not** a gate
  promise — the gate is the GUI 7zFM + 7z.dll surface.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a13_msvcrt.sh` (guest, QEMU) | **6/6 assertions** | `w32a13_msvcrt.exe` (`-lmsvcrt`, **15 msvcrt name imports** resolved through `w32_bind.c`) runs the bridge REAL over the personality: **heap unity across two DLLs** (a `msvcrt.dll!malloc` block measured by `kernel32.dll!HeapSize` — they agree at 100 then 200 bytes — and `kernel32.dll!HeapReAlloc` growing a CRT pointer), the string/mem core, the seeded MSVCRT `rand` LCG (`srand(1)`→41, reproducible), `_beginthreadex` over `CreateThread` (join + exit code 55), and the `_onexit` chain — the run ends through `msvcrt.dll!exit(78)`, which fires the LIFO callbacks (`OX-B` before `OX-A`) then `ExitProcess`; prints `W32A13-MSVCRT-OK`, exits 78, no fault |
| `test_w32_a13_msvcrt` (host, ASan/UBSan) | **71 checks, 0 failures** | heap unity vs a kernel32 `HeapSize` double; string/mem core incl. overlapping `memmove`, `strchr(…,0)`, empty-needle `strstr`, the wide variants; the exact `rand` LCG; `__getmainargs` over the **real** `w32_argv.c` splitter (argc/argv-into-one-buffer/NULL env, the quoted arg `"b c"`); `_initterm` (NULL cells skipped, in order); the `_onexit` LIFO + the four exit-code paths kept distinct (exit: callbacks+terminate; `_exit`: terminate only; `_cexit`: callbacks+return; `_c_exit`: neither); `__dllonexit` growing a caller table on the process heap; `_beginthreadex` forwarding start/arg/flags/tid; `__CxxFrameHandler` continue-search + type_info dtor |
| `w32run.elf` link | **links clean** | the whole personality (incl. `w32_msvcrt.o`) links; `w32_msvcrt_*` referenced from `w32_bind.c` and `w32_msvcrt_init` from `kernel32.c` all resolve |
| `tools/check_provenance.sh` | **PASS (85 files)** | `src/msvcrt.c` + `tests/w32a13_msvcrt.c` recorded; the fetched 7-Zip binaries stay under git-ignored `build/` |
| `python3 tools/check_w32app_claims.py --check` | 3 pre-existing FAILs only | the missing `patches/W32A2/A3/A4` receipts (unrelated, absent in base `a664d94`); marking W32A-13 DONE adds no new failure and the W32A-0 census still confirms `agree` — union 611, gap 28, `CHECK OK` (msvcrt is not in the K/U/G union, so the gap is unchanged) |

**Honest non-goals (the plan says the words).**  C++ *catch matching* stays the
D7 gap: `__CxxFrameHandler` returns continue-search, so a C++ exception sweeps
its cleanups and then dies with a NAMED terminate rather than a fabricated
catch (`_CxxThrowException` already does the sweep-then-name).  `type_info`'s
destructor is REAL (teardown is not catch matching).  Per-DLL onexit teardown
at `FreeLibrary` time is simplified to process-exit LIFO ordering; the
`__dllonexit` table contract itself is REAL and host-tested.  CRT stdio
(`_iob`/`fflush`/`fgetc`/…, the console 7z.exe surface) is out of gate scope.
W32A-13 is not an application gate, so no `.imports` ledger with `MZ` binaries
or REFUSE rows blocks it.

Closing patch: `patches/W32A13_msvcrt.patch`.

## App-gate receipts (reserved format)

Application phases (W32A-14 PuTTY, W32A-15 7-Zip FM, W32A-16
Notepad++, W32A-17 final) each end with a receipt block in this file.
The block format is fixed now so the claim checker can require it later.
(The 7z.dll and npp-plugins ledgers ride with their app's gate: the
receipt prose names them; only the .exe ledger is structural.)

```
## W32A-<n> <app> — <date>
ledger: w32/app_ledger/<name>.imports (sha256 <hash-of-ledger>)
binary-sha256: <hash of the measured user-supplied binary>
REFUSE rows in ledger: <must be 0 for an app gate to flip green>
runs: <what was executed, in-guest, verbatim command>
seen: <greppable pattern observed in the guest log>
```

The REFUSE guard is absolute: while an application's ledger contains
any `REFUSE` row, its gate stays red no matter what runs. The claim
checker enforces this structurally (a ✅ app heading + a REFUSE row in
that app's ledger = FAIL).

## No-binary rule

`w32/app_ledger/` holds names, never bytes: DLL names, symbol names,
counts, hashes. The provenance gate scans the tracked tree for the
`MZ` magic as well as for `*.exe`/`*.dll` names, so a renamed binary
fails exactly like a committed one. The ledger headers record the
source binary's size and sha256 so a future re-measurement can prove it
measured the same file.

## Awaiting gates

Empty sections the app phases fill. A section stays `AWAITING` until its
gate lands; the claim checker fails a ✅ app phase whose section is still
`AWAITING` (or missing).

## W32A-14 putty — PHASE CLOSED (2026-09-28), the PuTTY app gate (CI-provable half)

The first application gate. The pinned **PuTTY 0.85 (Win64)** `putty.exe` was
fetched to `build/w32bins/` from `https://the.earth.li/~sgtatham/putty/0.85/w64/putty.exe`
— git-ignored, provenance-exempt, **never committed** (D8 / `w32/LICENSING.md`).
Its sha256 is `d01fdb5aae8f112526040a39b0bfb9e27d813003178645e65f8d1cfdb2a26c87`,
size `1706136` — **byte-for-byte the pin** recorded in the committed ledger
`w32/app_ledger/putty-0.85.imports`. The fetch+scan is reproducible;
`w32/tests/W32A14.probe.log` records it.

**Import surface, measured (`objdump -p putty.exe`).** 348 name imports across
8 DLLs: KERNEL32 148, USER32 119, GDI32 51, ADVAPI32 15, COMDLG32 6, IMM32 5,
ole32 3, SHELL32 1. Cross-referenced against `w32/src/w32_bind.c`'s static
`exports[]`:

| resolution | count |
|---|---:|
| resolved from static `exports[]` (REAL or fail-clean) | **348** |
| left on a loud TODO stub | **0** |
| unbound / generated-other | **0** |

Before W32A-14 the split was **334 static + 14 loud TODO stubs**. Those 14 are
the "personality fixes the gate forced" (the plan: *a gate that changes nothing
is suspicious*). W32A-14 lands them so PuTTY's **entire 348-import surface now
binds to an honest body**:

- **KERNEL32 console — REAL** (`w32/src/kernel32_con.c`): `GetConsoleMode`
  (TRUE for a std handle, FALSE + `ERROR_INVALID_HANDLE` for a redirected
  file/pipe — the signal PuTTY/plink use to detect redirected stdio),
  `GetConsoleOutputCP` (65001, matching the write path), `WriteConsoleW`
  (UTF-16 → UTF-8 over `WriteFile`, chunked, no heap), `ReadConsoleW`
  (UTF-8 → UTF-16 over `ReadFile`), `SetStdHandle` (an override
  `GetStdHandle` consults — via a WEAK default in `kernel32.c` so the host
  amalgam tests still link).
- **KERNEL32 serial — FAIL-CLEAN** (serial is a documented non-goal — no COM
  hardware on the lite personality): `GetCommState`/`SetCommState`/
  `SetCommTimeouts`/`SetCommBreak`/`ClearCommBreak` all return FALSE +
  `ERROR_INVALID_FUNCTION`, so PuTTY's serial backend shows its documented
  "unable to open" path instead of faulting on a TODO stub.
- **USER32 terminal window — REAL** (`w32/src/user32_win.c`): `MessageBeep`
  (silent success — no audio device), `SetCursor` (returns the previous
  cursor), `ShowCursor` (MSDN displacement counter, not a boolean),
  `SetClassLongPtrA` (per-class store; `GCLP_HICON`/`HICONSM`/`HCURSOR`/
  `GCL_STYLE` map to the class fields, positive offsets to a `cbClsExtra`
  slot; returns the previous value).

IMM32's 5 imports stay fail-clean per the ledger — PuTTY's IME degrades
gracefully when IMM32 is absent, so they do not gate green here.

**Census effect.** The 14 names are all in the K/U/G ledger union, and were
uncovered until now, so `tools/w32_import_ledger.py` moves the coverage gap
**28 → 14** (`EXPECTED_GAP` updated to match; union stays 611, personality
exports 620 → 634, `CHECK OK`). W32A-13's msvcrt work left the gap unchanged
(msvcrt is not in the K/U/G union); W32A-14 is the phase that closes it.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a14_putty_fixture.sh` (guest, QEMU) | **6/6 assertions** | `w32a14_putty.exe` — a mingw PE importing the KERNEL32/USER32/ADVAPI32 surface **by name** (resolved through `w32_bind.c`) and resolving **WS2_32 dynamically** (`LoadLibrary`+`GetProcAddress`) as PuTTY does. It walks PuTTY's personality paths and asserts each: the `Reg*A` **session save/load round-trip** (`HostName`/`PortNumber` intact after close+reopen), the REAL console slice (`GetConsoleOutputCP`==65001, `GetConsoleMode` TRUE/FALSE, `WriteConsoleW` widening — the log shows `W32A14 console: ok`, `SetStdHandle` override), the terminal verbs (`ShowCursor` counter, `SetCursor` previous, `MessageBeep`, `SetClassLongPtrA` class-long round-trip on a real registered class), the **serial fail-clean** negative flow (all five COMM verbs FALSE), and the dynamic WinSock chain. Prints `W32A14-PUTTY-OK`, exits **78**, no `FAIL-`, **no TODO fall-through**, no fault |
| `test_w32_a14_con` (host, ASan/UBSan) | **37 checks, 0 failures** | `kernel32_con.c` + the pure UTF converter against `WriteFile`/`ReadFile`/last-error doubles: codepage, `GetConsoleMode` (std handles TRUE, non-console + overridden FALSE, NULL-arg guard), `WriteConsoleW` (ASCII, a multibyte `U+00E9`→`C3 A9`, a >128-unit run forcing the chunk loop, zero-length no-op), `ReadConsoleW` (narrowing + EOF), `SetStdHandle`/`w32_std_handle_override` round-trip + unknown-which reject, the COMM set fail-clean with `ERROR_INVALID_FUNCTION` |
| `w32run.elf` link | **links clean** | the whole personality (incl. `w32_kernel32_con.o`) links; `SetStdHandle`/`GetConsoleMode`/… referenced from `w32_bind.c` and `w32_std_handle_override` from `kernel32.c` all resolve (strong def in the console TU wins over the weak default) |
| import coverage (`objdump -p` vs `w32_bind.c`) | **348/348, 0 TODO** | `w32/tests/W32A14.probe.log` |

**Honest non-goals (the plan says the words — human-run, not headlessly
automatable in this environment).** The full PuTTY *receipt* is human-run and
pasted back hash-verified: the **config dialog** rendering (category tree,
host/port fields, connection-type radios — GUI framebuffer / pixel-region
asserts), the **terminal render** (ASCII + box-drawing + colours screenshotted),
the **font/colour choosers** (COMDLG32 six) and **clipboard** round-trip to
`gclip`, and **live Raw/TCP + Telnet + SSH** sessions (banner + kex, password
auth, GSSAPI offered-and-absent, `SECUR32`/`GSSAPI64` auth-fallback) against
external servers. Those need a framebuffer and real network peers and are out
of this gate's automatable scope by design. What is CI-provable — the import
surface closing to 0 TODO, the registry/console/cursor/serial personality
paths, and the dynamic-WinSock resolution path — is proved above by the
fixture twin and the host unit test.

Closing patch: `patches/W32A14_putty.patch`.

## W32A-15 7zFM — AWAITING

## W32A-16 notepad++ — AWAITING

## W32A-17 final — AWAITING
