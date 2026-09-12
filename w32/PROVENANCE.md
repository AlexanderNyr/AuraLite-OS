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
| `app_ledger/putty-0.85.imports` | Measured import ledger, PuTTY 0.85 (W32A-0) |
| `app_ledger/7zFM-24.09.imports` | Measured import ledger, 7-Zip FM 24.09 (W32A-0) |
| `app_ledger/7z-24.09.imports` | Measured import ledger, 7z.dll 24.09 (W32A-0) |
| `app_ledger/notepad++-8.8.9.imports` | Measured import ledger, NPP 8.8.9 (W32A-0) |
| `app_ledger/npp-plugins-8.8.9.imports` | Measured import ledger, NPP plugins (W32A-0) |
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
| `LICENSING.md`, `PROVENANCE.md` | This documentation |

### On the application ledgers

`app_ledger/*.imports` are generated by `tools/w32_import_ledger.py` from
user-supplied binaries that are measured and never committed. They record
DLL names, symbol names, counts and hashes -- facts about an interface,
no bytes -- in the same spirit as the `.def` files below. The provenance
gate scans the tree for the `MZ` magic so a renamed binary fails like a
committed one.

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
