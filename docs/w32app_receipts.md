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

## W32A-11 — incremental evidence only (2026-09-24 and 2026-09-27; NOT DONE)

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

## W32A-14 putty — AWAITING

## W32A-15 7zFM — AWAITING

## W32A-16 notepad++ — AWAITING

## W32A-17 final — AWAITING
