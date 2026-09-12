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
| 7z.dll | 7-Zip 24.09 | `882063949d3d899f88e1bb8ef987f316003168fc914976c0d3a7976445d34e4` |
| notepad++.exe | Notepad++ 8.8.9 | `a470014b48373db5d244d9f95a5df37fdbd0ecc7e760bf7069fc39eb7e823b27` |

Plugin DLLs shipped with the portable Notepad++ 8.8.9 (NppExport,
mimeTools, NppConverter, nppPluginList) are hashed inside
`w32/app_ledger/npp-plugins-8.8.9.imports`.

Upstream URLs (informational — the hash is the identity, not the URL):

- `https://the.earth.li/~sgtatham/putty/latest/w64/putty.exe`
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

## App-gate receipts (reserved format)

Application phases (W32A-4 PuTTY, W32A-7 7-Zip FM, W32A-11 Notepad++,
W32A-13 7z.dll, W32A-15 plugins, W32A-17 final) each end with a receipt
block in this file. The block format is fixed now so the claim checker
can require it later:

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

## W32A-4 putty — AWAITING

## W32A-7 7zFM — AWAITING

## W32A-11 notepad++ — AWAITING

## W32A-13 7z.dll — AWAITING

## W32A-15 npp-plugins — AWAITING

## W32A-17 final — AWAITING
