# w32 — provenance record

Every file under `w32/` is either **written for this repository** or
**vendored** from a permissively licensed upstream. This file records which,
and is enforced by `tools/check_provenance.sh`.

The rules that govern additions are in [`LICENSING.md`](LICENSING.md); the
reasoning is in [`../WIN32_PLAN.md`](../WIN32_PLAN.md) §1.

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
| `LICENSING.md`, `PROVENANCE.md` | This documentation |

### On the kernel-side loader

`kernel/proc/pe.c` and `kernel/proc/pe.h` live outside this directory but
belong to the same effort. They are written for AuraLite, modelled on the
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
