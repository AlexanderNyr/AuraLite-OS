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

## W32A-15 7zFM — PHASE CLOSED (2026-09-28), the 7-Zip File Manager app gate (CI-provable half)

The second application gate. The pinned **7-Zip 24.09** `7zFM.exe` + `7z.dll`
were extracted to `build/w32bins/` from the self-extracting installer
`https://www.7-zip.org/a/7z2409-x64.exe` — git-ignored, provenance-exempt,
**never committed** (D8 / `w32/LICENSING.md`). Their sha256 /size are
`dc4fdcd96efe7b41e123c4cba19059162b08449627d908570b534e7d6ec7bf58` / `990720`
and `882063948d675ee41b5ae68db3e84879350ec81cf88d15b9babf2fa08e332863` /
`1907712` — **byte-for-byte the pins** in the committed ledgers
`w32/app_ledger/7zFM-24.09.imports` and `w32/app_ledger/7z-24.09.imports`.
`w32/tests/W32A15.probe.log` records the fetch+scan and the two live boots.

**Import surface, measured (`objdump -p`).** `7zFM.exe` imports 298 names across
11 DLLs (KERNEL32 106, USER32 88, msvcrt 34, ADVAPI32 21, ole32 11, SHELL32 11,
COMCTL32 10, OLEAUT32 7, MPR 6 delay-load, COMDLG32 3, GDI32 1); `7z.dll`
imports 86 across 5 (KERNEL32 54, msvcrt 22, OLEAUT32 7, ADVAPI32 1, USER32 2).
Cross-referenced against `w32/src/w32_bind.c`'s static `exports[]`:

| resolution (7zFM.exe) | count |
|---|---:|
| resolved from static `exports[]` (REAL or fail-clean) | **292** |
| delay-load (mpr.dll, fail-clean on first network touch) | **6** |
| left on a loud TODO stub | **0** |
| unbound / generated-other | **0** |

**The named casualty the gate forced.** A real QEMU boot of the pinned
`7zFM.exe` (via `run /fat/7ZFM.EXE`) showed exactly ONE import falling through
to a loud TODO stub on first paint — the toolbar-strip load:

```
w32run: /fat/7ZFM.EXE mapped at 0x400000000000, 292 import(s) bound
w32: TODO user32.dll!LoadBitmapW needs W32A-5 (not yet implemented)
```

W32A-15 lands **`LoadBitmapW` as REAL** (`w32/src/w32_rsrc.c` +
`w32/src/w32_gdi.c`): it resolves the `RT_BITMAP` resource and builds a device
`HBITMAP` via `w32_gdi_bitmap_from_dib`, which expands the source packed DIB
through its palette into the engine's native 32bpp top-down ARGB. 7-Zip ships
its toolbars at **4bpp/16-colour** (48×36 and 24×24) and its sort marker at
**1bpp** — depths the raster sampler does not read directly — so the expansion
covers 1/4/8/24/32bpp and both row orientations. `LoadImageW`'s `IMAGE_BITMAP`
path now routes through the same helper (it previously returned the raw resource
pointer). After the fix the same boot no longer prints the TODO line and 7zFM's
whole static surface binds to an honest body (the security set stays fail-clean;
`SHGetFileInfoW`'s PIDL path answers `ERROR_INVALID_PARAMETER`; no fault).

**Census effect.** `LoadBitmapW`/`LoadBitmapA` land as real static bindings that
shadow the generated TODO stub (the `w32_bind.c` static table wins ties, exactly
as `LoadImageW`/`LoadCursorW`/`LoadIconW` are done). `LoadBitmapW` is in the
K/U/G ledger union and was previously uncovered, so closing it moves personality
exports **634 → 636** and the coverage gap **14 → 13** (K/U/G union stays 611);
`tools/w32_import_ledger.py`'s `EXPECTED_GAP` is updated to match and re-reports
`CHECK OK`.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a15_7zip_fixture.sh` (guest, QEMU) | **6/6 assertions** | `w32a15_7zip.exe` — a mingw PE importing the KERNEL32/USER32/GDI32/COMCTL32/SHELL32/ADVAPI32 surface **by name** (resolved through `w32_bind.c`) with an **embedded `RT_BITMAP`** (a 4bpp packed DIB, 7-Zip's toolbar-strip shape) so `LoadBitmapW` walks the real PE resource path. It asserts each 7-Zip personality path: the **LoadBitmapW REAL slice** (RT_BITMAP → device HBITMAP, `GetObjectW` dims, `SelectObject`+`GetPixel` proving the 4bpp→32bpp palette expansion, missing-id → NULL fail-clean), the run-time **DLL-chain + msvcrt heap** (`LoadLibrary`+`GetProcAddress` `InitCommonControlsEx` and `malloc`/`free`/`realloc` — the exact heap trio 7-Zip imports; `calloc` is NOT in its surface so the gate does not ask for it), a report **SysListView32** with three rows read back, the **delay-loaded MPR** network path failing CLEAN, the `Reg*W` **Options round-trip**, and **`SHFileOperationW` FO_DELETE** over the VFS. Prints `W32A15-7ZIP-OK`, exits **78**, no `FAIL-`, **no TODO fall-through**, no fault |
| `test_w32_a15_bitmap` (host, ASan/UBSan) | **36 checks, 0 failures** | `w32_gdi_bitmap_from_dib` (amalgamated with the a7 gate's GDI engine + fake compositor) against hand-built packed DIBs: 4bpp/16-colour bottom-up (each index → right ARGB, rows flip, `SelectObject`+`GetPixel` round-trip), 1bpp MSB-first, 8bpp palettised, 24bpp BGR and 32bpp BGRA straight-through, a negative-height top-down source, and malformed DIBs (short buffer, non-`BI_RGB`, absurd size, unsupported bpp) failing clean with `ERROR_INVALID_PARAMETER` |
| import coverage (`objdump -p` vs `w32_bind.c`) | **292 static + 6 delay, 0 TODO** | `w32/tests/W32A15.probe.log` |

**Honest non-goals (human-run, not headlessly automatable in this
environment).** The full 7-Zip *receipt* is human-run and pasted back
hash-verified: the **main window** render (listview + toolbar with the loaded
bitmap strips + status bar — GUI framebuffer / pixel-region asserts), **archive
listing** of a byte-known `.zip`/`.7z` with the rows asserted, **extract-all**
with every file byte-compared against the harness originals, the **Options
property sheet** (`PropertySheetW`) persisting through the hive, **drag-drop**
add and `SHFileOperationW` rename from the UI, and the **ACL approximation**
(extract an ACL-carrying archive, assert AuraLite's documented owner-only
mapping — the ADVAPI32 security set is fail-clean by design). Those need a
framebuffer, a pixel oracle and human-driven UI and are out of this gate's
automatable scope by design. What is CI-provable — the import surface closing to
0 TODO with `LoadBitmapW` REAL, the DIB→HBITMAP palette expansion, the listview/
heap/delay-load/registry/VFS personality paths — is proved above by the fixture
twin and the host unit test.

Closing patch: `patches/W32A15_7zip.patch`.

## W32A-16 notepad++ — PHASE CLOSED (2026-09-29), the Notepad++ app gate (CI-provable half)

The third application gate. The pinned **Notepad++ 8.8.9** `notepad++.exe` was
downloaded (portable x64) to `build/w32bins/` — git-ignored, provenance-exempt,
**never committed** (D2 / `w32/PROVENANCE.md`). Its sha256 / size are
`a470014bb7f6d8587d3a4b9ddc3bab0a356a945e8f63b8067834805d3082472d` / `8365216`
— **byte-for-byte the pin** in the committed ledger
`w32/app_ledger/notepad++-8.8.9.imports` (its plugins are pinned separately in
`w32/app_ledger/npp-plugins-8.8.9.imports`). `w32/tests/W32A16.probe.log`
records the census, the traced open path and the live fixture boot.

**Import surface, measured (`objdump -p`).** `notepad++.exe` imports **590**
names across **19 DLLs** (USER32 214, KERNEL32 186, GDI32 63, COMCTL32 23,
ADVAPI32 20, SHLWAPI 17, UXTHEME 15, OLE32 11, SHELL32 10, IMM32 9, CRYPT32 8,
VERSION 3, SENSAPI 2, OLEAUT32 2, DWMAPI 2, COMDLG32 2, WININET 1, WINTRUST 1,
DBGHELP 1), including nine ordinal imports (COMCTL32 `#17 #381 #410 #411 #412
#413`, OLEAUT32 `#4 #6`, SHELL32 `#165`). Cross-referenced against
`w32/src/w32_bind.c`:

| class (notepad++.exe) | count |
|---|---:|
| REAL | **565** |
| FAIL-CLEAN (crypt32 signed-object set, dwmapi, PrintDlgW, SENSAPI probes, WinVerifyTrust, + W32A-16's `GetComboBoxInfo` and `ReadDirectoryChangesW`) | **25** |
| REFUSE / unresolved ordinal | **0** |
| left on a loud TODO stub | **0** |

**Correction — this gate DID force new personality code.** An earlier draft of
this receipt claimed "NO new personality code / an honest empty patch." That was
wrong: **nine symbols `notepad++.exe` imports were still resolving to the
generated loud-TODO stubs** in `w32_stubs_gen.c` — the static `exports[]` table
did not shadow them, so a real boot that exercised them would have faulted (the
same shape as W32A-15's `LoadBitmapW`; the earlier fixture simply never called
them, so it went green while a real boot would not have). `w32_import_ledger.py
check` flagged the true gap (**13**, of which **9 are notepad++.exe imports**).
W32A-16 closes all nine with real code, taking the census gap **13 → 4**:

| symbol(s) | DLL | class | where |
|---|---|---|---|
| `BeginDeferWindowPos` / `DeferWindowPos` / `EndDeferWindowPos` | USER32 | REAL | `user32_win.c` — flicker-free frame-layout batch (HDWP → `SetWindowPos` at flush) |
| `CreateIconIndirect` / `GetIconInfo` | USER32 | REAL | `w32_gdi.c` — icon⇄bitmap bridge (icon-object + 32bpp device-bitmap; lossless round-trip) |
| `wsprintfW` | USER32 | REAL | `kernel32_loc.c` — `ms_abi` variadic guest twin of the sysv host shell |
| `FreeLibraryWhenCallbackReturns` | KERNEL32 | REAL | `w32_module.c` — drops the module ref via loader-aware `w32_FreeLibrary` |
| `GetComboBoxInfo` | USER32 | FAIL-CLEAN | `user32_win.c` — no COMBOBOX control class |
| `ReadDirectoryChangesW` | KERNEL32 | FAIL-CLEAN | `kernel32.c` — no VFS change-journal; `ERROR_NOT_SUPPORTED`, NPP falls back to manual reload |

The **four** symbols left in the census gap are **not** `notepad++.exe` imports
(`CharPrevExA`, `SetPriorityClass` — 7-Zip/PuTTY surface; `GetPrivateProfileIntW`,
`GetPrivateProfileSectionNamesW` — the NppConverter plugin's unimplemented INI
family), so `notepad++.exe`'s whole 590-import surface now binds to a REAL body
or a named fail-clean stub with **zero loud-TODO fall-through**. Everything else
NPP names was already landed by W32A-1..W32A-15 (the loader + ordinals, KERNEL32
files/threads, the SEH unwinder, USER32 windows/dialogs/menus, GDI32, COMCTL32
**including the SysTabControl32 tab control**, ADVAPI32/registry, SHELL32 +
SHLWAPI + VERSION, OLE32/OLEAUT32/IMM32, the msvcrt bridge). Census after this
phase: personality exports **645**, K/U/G union **611**, gap **4** —
`tools/w32_import_ledger.py check` `CHECK OK`.

**The traced open path (the §2.3 surprise).** `notepad++.exe` has **no
`GetOpenFileNameW`/`GetSaveFileNameW` import** — its only COMDLG32 imports are
`ChooseColorW` (REAL) and `PrintDlgW` (fail-clean). It reaches the file
namespace through the shell object model: `SHCreateItemFromParsingName` →
`IShellItem` → `GetDisplayName`, plus direct `CreateFileW` on a typed/dropped
path. The gate asserts THAT path (an `IShellItem` round-trip and a
`CreateFileW` save/reopen), not a common dialog the application never opens.

| gate / command | result | scope |
|---|---:|---|
| `tests/integration/cases/test_w32a16_npp_fixture.sh` (guest, QEMU) | **6/6 assertions** | `w32a16_npp.exe` — a mingw PE importing the KERNEL32/USER32/GDI32/COMCTL32/SHELL32/SHLWAPI/OLE32/WININET/SENSAPI/WINTRUST surface **by name** (resolved through `w32_bind.c`). It asserts each Notepad++ personality path: the **SysTabControl32** open-files tab strip (insert three pages, `TCM_GETITEMCOUNT`==3, `TCM_SETCURSEL`/`TCM_GETCURSEL` switch, `TCM_GETITEMW` read-back, `TCM_DELETEITEM` close → 2), the **SHLWAPI `Path*`** filename family and the **`Color*`** dark-mode arithmetic, the **traced open path** (`SHCreateItemFromParsingName` → `IShellItem` → `GetDisplayName` round-trip; NULL path fail-clean), a **save + reopen** and **save-as** over the VFS (bytes asserted), the run-time **plugin DLL chain + msvcrt heap** (`LoadLibrary`+`GetProcAddress` `InitCommonControlsEx`, `malloc`/`free`), the **minimise-to-tray** `Shell_NotifyIconW` NIM_ADD/DELETE, and the **offline updater** (`InternetCrackUrlW` parse → scheme HTTPS + host, `WinVerifyTrust` → `TRUST_E_NOSIGNATURE`, the SENSAPI probes answering a clean boolean without a fault). **Extended this phase** with a "REAL slices" section that drives the nine newly-landed imports in-guest: the `DeferWindowPos` batch on three `WS_POPUP` frames (defer → flush → `GetClientRect` before/after), the `CreateIconIndirect`/`GetIconInfo` round-trip (4×4 32bpp + 1bpp mask, `GetObjectW` dims), `wsprintfW` (`"%s=%d/%x"` → `"line=42/ff"`, n==10), `FreeLibraryWhenCallbackReturns` (double-load), and `ReadDirectoryChangesW` fail-clean (`ERROR_NOT_SUPPORTED`). Prints `W32A16-NPP-OK`, exits **78**, no `FAIL-`, **no TODO fall-through**, no fault |
| `test_w32_a16_layout_icon` (host, ASan/UBSan) | **51 checks, 0 failures** | the two REAL slices this phase landed, amalgamating the USER32 window core + the GDI engine + a pos/size-tracking fake compositor. **DeferWindowPos:** a three-window batch is deferred (nothing moves until `EndDeferWindowPos`), the flush applies every entry (`GetWindowRect`/`GetClientRect` checked), the HDWP is consumed (second `End` fails clean), and a bad child frees the HDWP and returns NULL; `GetComboBoxInfo` fails clean. **Icons:** a lossless icon → `GetIconInfo` → `CreateIconIndirect` → icon round-trip for opaque/transparent pixels, the colour bitmap reads back through `GetPixel`, the AND mask carries the alpha, the no-mask path treats the colour bitmap as opaque, and malformed inputs fail clean with `ERROR_INVALID_PARAMETER` |
| `test_w32_a16_shlwapi` (host, ASan/UBSan) | **33 checks, 0 failures** | the pure `shlwapi.c` modules (amalgamated with `w32_utf.c`): `PathFindFileNameW`/`PathFindExtensionW` split points, `PathStripPathW`/`PathRemoveFileSpecW`/`PathRemoveExtensionW`/`PathAddExtensionW` in-place mutation, `PathCombineW` (incl. an absolute second component overriding the first), `PathMatchSpecW` `*`/`?` case-insensitive globbing, `PathIsRelativeW` drive/UNC vs relative, `PathCompactPathExW` ellipsis-from-the-front + verbatim-when-it-fits, the `ColorRGBToHLS`→`ColorHLSToRGB` channel-close round-trip, grey staying achromatic, `ColorAdjustLuma` darkening/lightening monotonically, and `AssocQueryStringW`'s empty-table `S_FALSE` |
| import coverage (`w32_import_ledger.py check`) | **590 imports, 0 REFUSE, 0 TODO; census gap 13 → 4** | `w32/tests/W32A16.probe.log` |

**Honest non-goals (human-run, not headlessly automatable in this
environment).** The full Notepad++ *receipt* is human-run and pasted back
hash-verified: the **Scintilla** text render with syntax colours, the
**tab-switch** pixels, the **Find/Replace** dialog UI, the three shipped
**plugins'** menu entries loading through `SHLWAPI`, **drag-drop add** from the
compositor (the `GUI_EVT_DROP` → `WM_DROPFILES` path W32A-11 already proved end
to end), and **minimise-to-tray** shown as a live compositor notification.
Those need a framebuffer, a pixel oracle and human-driven UI and are out of this
gate's automatable scope by design. What is CI-provable — the 590-import
surface binding with 0 REFUSE / 0 TODO, the tab control, the path/colour
arithmetic, the `IShellItem` open path, the VFS save/reopen, the plugin DLL
chain, the tray call and the offline-updater fail-clean behaviour — is proved
above by the fixture twin and the host unit test.

Closing patch: `patches/W32A16_npp.patch`.

## W32A-17 — App horizon: Audacity — PHASE CLOSED (2026-09-29), OUTCOME B (does not run)

The horizon phase. Modelled on W32-7 ("`LoadLibrary`, or a documented refusal"),
it succeeds by **deciding with evidence**, not by running. It decided: **Audacity
3.7.5 does not run on the personality**, and the committed artefact is the gap
ledger `w32/app_ledger/audacity-3.7.5.gap`, not a receipt of a launch.

**Pinned subject (obtained from the publisher, never committed — §1.1).** Audacity
3.7.5, 64-bit portable zip (`audacity-win-3.7.5-64bit.zip`, sha256
`0bc382a8…`, 26 834 384 B) → `Audacity.exe` sha256
`e82c5ef5…`, 13 302 832 B, PE32+ GUI x64. Chosen over Audacity 4.0.0 because the
plan asks for a "portable x64 zip" (4.0.0 ships only an MSI) and anticipated
**wxWidgets** breadth — 3.7.5 is the last wxWidgets-era line (it bundles
wxWidgets 3.1.3).

**The horizon surprise: the single-binary ledger model breaks.** PuTTY/7-Zip/
Notepad++ import the Win32 system DLLs directly, so the W32A-0 tool measured them
from the `.exe`. Audacity's `.exe` does not: measured with the loader's own
parser (`build/w32_peinfo`), **Audacity.exe imports 6349 symbols across 34
modules, and every module is a BUNDLED DLL** (`lib-*.dll` + `wxmsw313u_*`), every
symbol a C++-mangled wx/Audacity name — **zero Windows system imports at the exe
level**. The real gap is therefore *transitive*, aggregated across the 117
bundled DLLs: **33 system modules, 1241 symbol imports**, of which **14 modules /
822 symbols are already MET** by the personality and **19 modules / 419 symbols
are the GAP** — the whole **Universal CRT api-set** (12 `api-ms-win-crt-*`
modules, 302 symbols; the personality bridges only legacy `msvcrt.dll`), **`winmm`
audio** (74), `wsock32` (31), `rpcrt4`/`msimg32`/`oleacc`/`winspool.drv`/`bcrypt`.

| gate / command | result | scope |
|---|---:|---|
| transitive import walk (117 bundled DLLs + exe) | **33 system modules, 1241 syms; 822 MET / 419 GAP** | `w32/app_ledger/audacity-3.7.5.gap` |
| launch attempt (real, in-guest QEMU) | **`too many relocations`, exit 1** | `run /tests/audacity.exe` → w32run |
| loader parser cross-check | **6349 imports, all bundled DLLs, 90932 relocations** | `build/w32_peinfo Audacity.exe` |

**The launch attempt (real).** Audacity.exe was staged into the initrd and booted
under QEMU. Verbatim:

```
[shell] /tests/audacity.exe imports Win32 DLLs; running via /apps/w32run
[elf]  loaded 3 segment(s), entry 0x40074a00
[proc] entering Ring 3 at 0x40074a00
w32run: manifest: comctl v5, exec=asInvoker
w32run: too many relocations
[thread] '/apps/w32run' (tid 8) exited (code=1)
```

**First fatal gap (measured live): the loader's fixed relocation buffer.**
Audacity.exe carries **90 932** base relocations (a 236 KB `.reloc`); w32run maps
them through a fixed `static pe_reloc_t relocs[16384]`
(`userspace/apps/w32run/w32run.c:167`), so it dies at `too many relocations`
**before import resolution begins**. It never reaches the bundled-DLL graph or
the 419-symbol system gap — those are the second and third walls, established
statically in the gap ledger.

**Decision — three independent walls, each fatal on its own:** (1) *scale* —
90 932 relocations vs a 16 384-entry loader buffer (live); (2) *shape* — a
117-DLL bundled application graph (wxWidgets 3.1.3 + ~60 `lib-*` + FFmpeg/FLAC),
an order of magnitude past Notepad++'s one-exe gate; (3) *runtime* — the UCRT
api-set (302 syms) + `winmm` audio (74), whole subsystems the personality does
not have. Audacity is **not the next rung of this ladder; it defines its
boundary.**

**Next-plan seed (this plan's §7 gains the measured line).** A future plan
("W32U" — the modern-runtime horizon) would need, in dependency order: (a) a
dynamic-scale loader (heap-allocated relocation/import tables + a multi-DLL
bundled-application module graph); (b) a Universal CRT bridge (`api-ms-win-crt-*`
re-exported over the `msvcrt` engine already built in W32A-13); (c) a
`winmm`/WASAPI audio subsystem (a kernel-level audio device, not just a Win32
shim). Cheapest first, biggest payoff: (a) then (b); (c) is a separate audio
plan.

**No personality `.c` code changed** — an honest gap list beats a padded
implementation of the wrong order of magnitude (the Outcome-B shape the plan
allowed). Closing patch: `patches/W32A17_audacity.patch` (the gap ledger + this
receipt + the plan/CHANGELOG/PROVENANCE lines; no code).

## W32A-18 — Integration, documentation and the honest matrix — PHASE CLOSED (2026-09-29)

This gate has no in-guest launch step: its subject is the *documentation and the
checkers*, and its receipts are the commands `make test-unit` runs. All are
CI-provable and green.

| gate / command | result | scope |
|---|---:|---|
| `python3 tools/gen_w32_api_table.py --check` | **PASS: 929 functions, 21 modules** | `docs/win32.md` table is byte-identical with `w32/src/w32_bind.c` + `w32/app_ledger/*.imports` |
| `python3 tools/gen_w32_api_table.py --write` (twice) | **second run is a no-op** | regenerates byte-identical (the `sdk-check` pattern) |
| `python3 tools/check_w32app_claims.py --check` | **PASS: done-phase claims are backed by the tree** | pins W32A-0 – W32A-18 artefacts + receipts; reruns the W32A-0 census; REFUSE guard + Outcome-B guard |
| `python3 tools/check_w32app_claims.py --selftest` | **PASS: violation detected as required** | plants a lying tree; 53 problems flagged |
| `python3 tools/check_residue_claims.py` | **PASS** | residue harvest matches the moved baseline; ledger arithmetic + class pins hold |
| `make w32-sdk-check` | builds `console`/`gui`/`unsupported` + `dialog`/`listview`/`delay-load` examples against the staged SDK | skips cleanly when `x86_64-w64-mingw32-gcc` is absent |

**D9 matrix, generated.** `docs/win32.md` now carries every module the export
table binds (929 functions / 21 modules), each function stamped with the D9
class the ledgers record (894 REAL, 35 FAIL-CLEAN; REAL is the documented
default for a function no ledger mentions). Regenerating cannot drift from the
code, because both inputs (`w32_bind.c` and the ledgers) are committed and the
`--check` gate fails the build on any divergence.

**Verdict.** The `w32` status row **stays 🧪** (not graduated). The reason is
honest, not ambition: the three running app gates (W32A-14/15/16) are green on
their **CI-provable half** — the committed import ledger plus the in-guest
*fixture* — while the **human-run half** below (a real, publisher-obtained
binary launched in QEMU with a pasted serial excerpt) stays `AWAITING`; and the
fourth app gate (W32A-17, Audacity) is a deliberate **Outcome B**. The verdict
follows the receipts.

Closing patch: `patches/W32A18_integration.patch` (this receipt + the docs +
the checker wiring + the examples + the residue reconciliation; combined with
W32A-17 in one patch).

## How to run a receipt (the protocol, followed blind)

A receipt is only a gate if a *second person* — not its author — can reproduce
it from these steps alone. Every app gate above (W32A-14 … W32A-17) is filled by
following this page; the CI-provable half needs no binary, the human-run half
does.

**The CI-provable half (no binary needed).** From a clean checkout:

1. `make test-unit` — runs every checker, including `check_w32app_claims.py`
   (`--check` + `--selftest`), `gen_w32_api_table.py --check`, and
   `check_provenance.sh`. Green here means the ledgers, the table and the plan
   agree with the tree.
2. `bash tests/integration/run_all.sh --group w32` — runs the W32A per-phase
   fixture cases (the loader, kernel32, the app fixtures). This is the half the
   `PHASE CLOSED … (CI-provable half)` receipts assert.

**The human-run half (needs the publisher's binary — never committed, §1.1).**

1. **Obtain** the pinned binary yourself from the publisher named in
   *The ladder binaries (pinned inputs)* at the top of this file (e.g. the exact
   PuTTY / 7-Zip / Notepad++ / Audacity version and URL).
2. **Verify** it: `sha256sum <file>` must equal the sha256 pinned in that table
   and in the app's ledger header. A mismatch means you have a different binary —
   stop; the receipt is about *that* one.
3. **Stage** it into the guest without committing it: copy it to `initrd/tests/`
   (git-ignored) and `make iso` (this does not add the binary to the tree — the
   provenance checker forbids committing it).
4. **Boot** it: `make run` (QEMU, TCG when `/dev/kvm` is absent), and at the
   shell run `run /tests/<binary>.exe`.
5. **Paste** the serial excerpt verbatim into the app's receipt section under a
   dated `— <date>, in-guest launch` heading, replacing its `AWAITING` line.
6. **Grep** the assertion phrase the gate names (e.g. the app's own banner, or
   for a refusal the exact `w32run:` line such as `too many relocations`). The
   phrase present in the pasted excerpt is the receipt; its absence is a failure,
   not a footnote.

The rule that makes this a gate and not a diary: **a section stays `AWAITING`
until a real pasted excerpt replaces it**, and `check_w32app_claims.py` fails any
`✅` app gate whose receipt section is still `AWAITING`.

## WR-2 7zFM — the 7-Zip File Manager, live on the framebuffer — 2026-09-30

W32RUN_PLAN.md phase WR-2. The pinned **7-Zip 24.09** `7zFM.exe`
(`dc4fdcd9…bf58`) + `7z.dll` (`88206394…2863`) run **live on the OVMF
framebuffer**: the real main window opens, all imports bind, the address bar is
built, and the process stays alive. The §0 `E_FAIL` (`0x80004005`) is gone.

### What landed (the 7-Zip personality)

Five fixes closed the startup blocker chain (found by instrumentation that has
since been fully removed — `grep -rn 'wr2-\|NSTRACE' w32/` is empty):

1. `SHGetSpecialFolderLocation` serves the virtual roots (`CSIDL_DRIVES` 0x11,
   `CSIDL_NETWORK` 0x12) as namespace PIDLs via new `ns_special_pidl`; a real
   empty `W32_NS_PIDL_NETWORK` node was added to the WR-1 graph.
2. `CreateWindowExW` accepts `WS_CHILD` for every registered class.
3. `WM_NCCREATE`/`WM_CREATE` deliver a real `W32_CREATESTRUCTW*` (was the bare
   param → corrupt vtable `0xC0000005`).
4. Style validation exempts a common control's class-defined low word.
5. `LVM_INSERTITEMW` no longer dereferences `LPSTR_TEXTCALLBACK`.

Two address-bar common controls were made **real** (ending the
`CLASS_DOES_NOT_EXIST` the launch checkpoint tolerated): `ReBarWindow32`
(band host — tracks bands, reports a one-row `RB_GETBARHEIGHT`) and
`ComboBoxEx32` (path combo — mirrors item text + selection, answers
`CBEM_GETCOMBOCONTROL`/`GETEDITCONTROL` as itself, forwards the `CB_*` it uses).

### Gates (measured 2026-09-30)

| Gate | Result | Notes |
| --- | --- | --- |
| `tests/integration/cases/test_wr2_7zip_live.sh` (guest, OVMF GUI lane, TCG) | **13/13** | 7zFM.exe binds 292 imports; no `0x80004005` / `Error #80004005` / `UNHANDLED EXCEPTION` / early exit; frame not black; a 7-Zip **main-window** title bar present (not a message box); monitor re-captures (system live). Reference frame sha256 `d5a96b225a8e0541…` (auralite-screenshots/wr2_7zip_live.png) |
| `tests/integration/cases/test_wr2_7zip_launch.sh` (guest) | launch smoke | subset of the above; loud-SKIPs without OVMF/binaries |
| `tools/wr2_make_fixture.py --check` | **CHECK OK** | byte-known ZIP built in-tree (`tests/fixtures/wr2/`), 2 members round-trip byte-exact; no foreign bytes committed |
| `make iso` (full multi-arch) | **builds** | kernel + rust (x86_64/riscv64/aarch64) + w32; `w32run.elf` links (needs the `w32_shell32_ns.o` Makefile wiring) |
| host `./build/test_shell_ns` (ASan/UBSan) | **45/0** | the Network node did not disturb the WR-1 graph |
| `tools/check_provenance.sh` | **PASS (96)** | no new `w32/` source files; comctl32.c/.h already recorded |
| `tools/check_test_registry.py` | **212, all registered** | both WR-2 cases in `ALL_CASES` |
| `tools/check_w32app_claims.py --check` | **PASS** | WR-2 is a W32RUN phase; adds no W32APP claim |

### Deferred, named (D-WR4), NOT faked

The plan's remaining **pixel-driven** WR-2 steps are blocked on ONE thing that
is out of the 7-Zip personality's scope — the **compositor does not clip
`WS_CHILD` windows into their parent**. 7-Zip builds its toolbar, both panes and
the `SysListView32` as `WS_CHILD` windows; the compositor draws each as its own
top-level surface, so the file panel is **not** rendered inside the 7-Zip
window and cannot be driven by clicking where the plan expects it. The live
frame therefore shows the 7-Zip main window (title bar + taskbar entry) with a
dark interior, and the following stay unachieved until a compositor phase adds
child clipping:

- the panel region showing `SysListView32` rows for `/fat`;
- a navigate content-delta (double-click a subdir);
- byte-exact extract driven **through the panel** (the CI fixture is byte-known
  and staged on `/fat`, and `7z.dll` loads; what is missing is the visible,
  clickable panel to drive the extract — 7zFM.exe is GUI-only, no CLI extract);
- the options `PropertySheetW` round-trip with a pixel delta.

This is recorded as a named compositor dependency, exactly as WR-1 named its
GUI-object non-goals, rather than reported as a false green. The 7-Zip
*personality* for WR-2 is complete and live; the interaction gate waits on the
compositor.

> **Update 2026-09-30 (CW-1):** the compositor dependency above has been
> **delivered** — see the CW-1 receipt below. 7-Zip's `WS_CHILD` panes now
> composite clipped inside the client area (pixel-proven: the client interior is
> a light embedded panel, ≈99% near-white, where it was black before). The
> remaining WR-2 pixel steps (the `/fat` **file-row text**, navigate, extract via
> the panel, options) are no longer blocked on the compositor; what they now
> need is listview virtual-item (`LVN_GETDISPINFO`) text rendering plus folder
> navigation — a personality/comctl32 concern, named here and not yet done.

## CW-1 — Compositor child-window embedding & clipping — 2026-09-30

`docs/plans/CW_COMPOSITOR_PLAN.md`. The kernel compositor (`kernel/gui/gui.c`)
gained real parent/child windows, resolving the WR-2 blocker: a `WS_CHILD` pane
now composites **inside** its parent, clipped to the parent's client rectangle,
stacked and moved with it.

### What landed

- `gui_win_t.parent` + `win_abs_x/win_abs_y` (relative→absolute resolution up
  the chain); `content_x/y/w/h` build on them so blit/invalidate/event-local
  coords are child-correct at every existing call site (top-level unchanged).
- Hierarchical compositing in **both** render paths (full + dirty): top-level
  windows in z-order, then each subtree drawn with the gfx clip armed to the
  intersection of every ancestor's content rect (∩ the dirty union).
- `hit_window` descends to the deepest child under the cursor; clicking a child
  raises its top-level ancestor.
- Destroy recurses to descendants; hide/minimize propagate for free.
- `GUI_OP_SET_PARENT` → `gui_set_parent()`; `ag_window_set_parent`; `user32`
  links a `WS_CHILD` and keeps parent-relative coords.

### Gates (measured 2026-09-30)

| Gate | Result | Notes |
| --- | --- | --- |
| `test_wr2_7zip_live.sh` (guest, OVMF/TCG) | **14/14** | new assertion: 7-Zip client interior is a light embedded panel — `find-color 245,245,245 --region "88 92 300 160" --min-frac 0.5` = 0.986; the pre-CW-1 frame scores 0.000 (black) |
| `build/test_wm` (host) | **35/35** | +6 CW-1 clip-geometry cases (abs origin, parent-move propagation, inside/overflow/offscreen clip, nested grandchild clip) |
| `test_gui_lane_smoke.sh` (guest) | **9/9** | top-level windows composite unchanged (no regression) |
| host `test_shell_ns` | **45/0** | — |
| `check_provenance.sh` / registry / claims | PASS / 212 / PASS | no new `w32/` source; no new test case |
| full multi-arch `make iso` | builds | kernel + rust + w32, `w32run.elf` links |

### Deferred, named (D-WR4)

CW-1 makes the panel *visible and clipped*; it does not by itself populate the
listview. The `/fat` **file-row text**, navigate, extract-through-panel and the
options property-sheet still need the listview to render virtual items via
`LVN_GETDISPINFO` and 7zFM to navigate to `/fat`. Byte-exact extract stays
human-run (guest lacks `sha256sum`; snapshot=on blocks host readback). These are
personality/comctl32 work, now unblocked by the compositor, and named rather
than asserted.
