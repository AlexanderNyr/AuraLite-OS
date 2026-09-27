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

## W32A-11 — incremental evidence only (2026-09-24; NOT DONE)

This is **not** `test_w32a11_ole.sh`, the scripted-app CLSID/IID probe, an
end-to-end compositor file-drop, or a green full `make test` receipt. The
phase heading in the plan is IN PROGRESS; none of these checks alone flips it.

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
therefore stay typed TODOs. `UI_EVT_DROP` carries coordinates plus a 16-bit
data field, not file paths, so separate HDROP handles and `DragQueryFileW`
cannot satisfy compositor file-drop delivery. The remaining OLE drag/drop
APIs and most UxTheme parts/animation remain TODO.
`check_w32app_claims.py --check` also fails on pre-existing missing
`patches/W32A2_kernel32fs.patch`, `patches/W32A3_threads.patch`, and
`patches/W32A4_unwind.patch`; do not mask that failure as W32A-11 DONE.

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
