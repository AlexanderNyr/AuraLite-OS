# AuraLite OS — Win32 Application Support Plan

## Status: IN PROGRESS 🔨 — W32-0 – W32-6 done; W32-7 – W32-8 planned

| Phase | State |
|---|---|
| W32-0 Provenance and the legal record | ✅ done |
| W32-1 UTF-16 and the string layer | ✅ done |
| W32-2 PE32+ parsing, in user space | ✅ done |
| W32-3 The kernel PE loader | ✅ done |
| W32-4 `KERNEL32` bounded import set | ✅ done |
| W32-5 `USER32` + `GDI32` | ✅ done |
| W32-6 CRT startup, TLS, minimal SEH | ✅ done |
| W32-7 `LoadLibrary`, or a documented refusal | 📋 planned |
| W32-8 Integration and documentation | 📋 planned |

**A real PE32+ `.exe` now runs on AuraLite.** W32-3 landed the kernel loader,
and a `nasm -f win64` + `lld-link` binary loads, executes Ring 3 code, and
exits with its own status:

```
[pe]   loaded 4 section(s) at 0x140000000, entry 0x140001000
W32-PE-LOADER-OK
[thread] '/tests/petest.exe' (tid 8) exited (code=77)
```

The same program linked at an impossible base is relocated and produces
identical output, and a byte-identical image whose only difference is an EFI
subsystem is refused. W32-4 then added the imports. A `.exe` that calls `KERNEL32` through a real
PE import table now has those imports bound and works across the ABI
boundary:

```
w32run: /tests/k32test.exe mapped at 0x400000000000, 10 import(s) bound
W32-KERNEL32-OK
BADHANDLE-REFUSED
HEAP-OK
TICK-OK
```

W32-5 put a **window** on screen from a PE binary, with the compositor doing
the work (D5) and the personality calling back into the image:

```
w32run: /tests/u32test.exe mapped at 0x400000000000, 21 import(s) bound
WNDPROC-WM_CREATE
WINDOW-CREATED
WNDPROC-WM_PAINT
WNDPROC-ARGS-OK
WNDPROC-WM_DESTROY
W32-USER32-OK
```

This document answers one question:

> *Can an unmodified Windows `.exe` run on AuraLite OS, and if so, what is the
> smallest honest path to the first one — without infringing anyone's rights?*

It follows the structure of the existing plans (`SDK_PLAN.md`, `GL_PLAN.md`,
`WEBVIEW_PLAN.md`, `FIXES_PLAN.md`, `POSIX_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, and one `.patch`
per phase.

**Baseline:** commit `e79bb90` (M4 update), on top of the completed
`SDK_PLAN.md` and `WEBVIEW_PLAN.md`.

This plan has an unusual first section. Every other plan in this repository
starts with what the code does; this one starts with what the law allows,
because the legal boundary determines the technical design and not the other
way round. Getting that order wrong is how a project like this ends up having
to delete work.

---

## 1. The legal boundary, decided first

**Nothing in this plan requires permission from Microsoft, and nothing in it
uses Microsoft's code.** That is a design constraint, not a hope, and the
phases below are shaped by it.

### 1.1 What is being copied, and why that is lawful

Re-implementing an API means writing your own code that answers to names
somebody else chose. The names, signatures and constant values — the
*declarations* — are what a program links against. The *implementation* behind
them is what this plan writes from scratch.

Three independent grounds make this defensible:

**Interoperability is the settled purpose.** In *Google LLC v. Oracle America,
Inc.*, 593 U.S. 1 (2021), the Supreme Court held that copying the declaring
code of an API — "reimplementation of a user interface" — to let programmers
"put their accrued talents to work in a new and transformative program" was
fair use as a matter of law. The Court assumed copyrightability for argument's
sake and decided on fair use; it did not hold that APIs are uncopyrightable.
That distinction matters and is why the other two grounds are not redundant.

**A functional interface is thin copyright at best.** 17 U.S.C. § 102(b)
excludes any "idea, procedure, process, system, method of operation" from
protection. In the EU, Directive 2009/24/EC Art. 1(2) excludes "ideas and
principles which underlie any element of a computer program, including those
which underlie its interfaces," and Art. 6 expressly permits decompilation for
interoperability. *SAS Institute v. World Programming* (CJEU C-406/10, 2012)
held that functionality, programming language and file formats are not
protected as such.

**There is a permissively licensed source for the declarations.** This is the
practical answer, and it is the one this plan relies on: **mingw-w64**
publishes Win32 API headers and import libraries under public-domain
dedications and ZPL-2.1 / BSD-3-Clause terms. They are already shipped by
Debian, Fedora and MSYS2, and they were produced from public documentation and
clean-room work, not from the Windows SDK.

So the plan does not need to litigate the first two grounds. It takes the
headers from a source that is already licensed for exactly this use.

### 1.2 Apache-2.0 compatibility

This repository is Apache-2.0. Inbound licences must be compatible.

| Source | Licence | Apache-2.0 inbound? |
|---|---|---|
| mingw-w64 headers (`mingw-w64-headers`) | Public domain / ZPL-2.1 / BSD-3-Clause | ✅ yes |
| mingw-w64 `crt`/runtime | ZPL-2.1, some LGPL-2.1 | ⚠️ headers only — see D3 |
| Wine source | **LGPL-2.1-or-later** | ❌ **no** — see D4 |
| ReactOS source | **GPL-2.0 / LGPL-2.1** | ❌ **no** — see D4 |
| Windows SDK headers | Proprietary EULA | ❌ **never** |
| Leaked Windows source | Stolen; no licence | ❌ **never** |

The two entries that will tempt a contributor are Wine and ReactOS, because
both have already solved every problem in this document. Both are copyleft and
**cannot** be copied into an Apache-2.0 tree. Reading them to learn *how* a
thing works is also how a project acquires a provenance problem it cannot later
disprove. D4 makes this a hard rule.

### 1.3 What is forbidden, stated plainly so it is not rediscovered later

- **No Microsoft SDK headers.** Not one line, not "just this struct".
- **No leaked or disassembled Windows code.** ReactOS's 2006 internal audit —
  triggered by an allegation that disassembly had been used — is the case study
  for why this matters. It cost them a repository lockdown and a full audit.
- **No copying from Wine or ReactOS.** Licence-incompatible, as above.
- **No redistributing Microsoft DLLs.** AuraLite ships no `kernel32.dll`,
  `user32.dll`, `msvcrt.dll` or any other Microsoft binary, ever. The user
  supplies the `.exe` they want to run; AuraLite supplies the implementation
  behind the imports.
- **No trademark use.** "Windows", "Win32", "Microsoft" and "MSVC" are
  Microsoft trademarks. This plan uses them nominatively — to say truthfully
  what is being interoperated with — and never in a product name, logo or in a
  way suggesting endorsement. The subsystem is called **`w32`** internally and
  described as "a Win32-compatible personality", not "Windows for AuraLite".

### 1.4 Not legal advice

The author of this plan is not a lawyer and this section is not legal advice.
It is a written record of the reasoning and the sources, so that a reviewer can
check it and a contributor can follow it. Anyone shipping AuraLite commercially
should have counsel review §1.

---

## 2. Where things actually stand

Measured against the tree at the baseline commit, not assumed.

### 2.1 The good news, established first

**AuraLite already produces and understands PE32+.** This was not assumed — it
was checked:

```
$ od -A d -t x1 -N 8 build/boot/BOOTX64.EFI
0000000 4d 5a 78 00 01 00 00 00        # "MZ", e_lfanew = 0x78
$ od -A d -t x4 -j 60 -N 4 build/boot/BOOTX64.EFI
0000060 00000078
```

`boot/uefi/` builds a PE32+ image, and `Makefile:1044` links it with
`lld-link -subsystem:efi_application`. `lld-link` is already in
`REQUIRED_TOOLS`. The project therefore already owns a PE toolchain and a
working example of PE layout.

That reframes the problem. **A PE loader is not exotic here** — it is the same
container the project's own bootloader uses, minus the parts UEFI does not
need: imports, relocations and a subsystem convention.

### 2.2 What exists to build on

| Requirement | State | Evidence |
|---|---|---|
| PE32+ container knowledge | ✅ in-tree | `boot/uefi/`, `lld-link` in `REQUIRED_TOOLS` |
| ELF loader to model on | ✅ mature | `kernel/proc/elf.c`, per-`PT_LOAD` W^X from `p_flags` |
| Per-process address spaces | ✅ working | boot log: `spawned PID 5 ... (CR3=0x2358000)` |
| Windowing to map USER32 onto | ✅ 32 ops | `GUI_OP_CREATE`/`MOVE`/`RESIZE`/`BLIT`/`DRAW_TEXT`/… |
| Drawing to map GDI32 onto | ✅ exists | `ag_fill_rect`, `ag_draw_line`, `ag_draw_text`, `ag_blit_alpha` |
| Widgets | ✅ exists | `ag_add_button`, `ag_add_listbox`, `ag_add_textbox`, … |
| Files/processes for KERNEL32 | ✅ 126 syscalls | `SYS_OPEN`/`READ`/`WRITE`/`MMAP`/`VirtualAlloc`-shaped `SYS_MPROTECT` |
| Wide strings | ⚠️ partial | `wchar.h` has `wcslen`/`wcscpy`/`wcsncpy`; no UTF-16↔UTF-8 conversion |
| `setjmp`/`longjmp` | ✅ exists | `lib/libc/include/setjmp.h` — the basis for a minimal SEH |
| PE loader | ❌ none | no `IMAGE_DOS`/`0x5A4D`/`PE\0\0` anywhere in `kernel/proc/` |
| Import resolution | ❌ none | ELF loader never looks at `PT_INTERP`/`PT_DYNAMIC` |
| Base relocations | ❌ none | ELF loader honours each `PT_LOAD`'s own `p_vaddr` verbatim |
| Win32 calling convention | ❌ none | see 2.4 |

### 2.3 The load-address collision is already solved

`lib/libc/user.ld` fixes every AuraLite program at `0x40000000`. A Win32 `.exe`
typically prefers `0x140000000` (x64) or `0x400000` (x86). Because each process
gets its own address space — proven by the distinct `CR3` values in the boot
log — a PE image can be mapped at its own `ImageBase` without colliding with
anything. Where it cannot, PE base relocations exist precisely to move it, and
unlike ELF the format was designed for that.

**This is the single biggest reason the project is closer to this than it
looks.**

### 2.4 The calling convention is the real work

x86-64 Windows and System V AMD64 differ in ways that cannot be papered over:

| | Windows x64 | System V AMD64 (AuraLite) |
|---|---|---|
| Integer args | `RCX RDX R8 R9` | `RDI RSI RDX RCX R8 R9` |
| Shadow space | 32 bytes, caller-allocated | none |
| Callee-saved | + `RSI`, `RDI`, `XMM6–15` | `RBX RBP R12–R15` |
| Struct return | hidden pointer in `RCX` | hidden pointer in `RDI` |

Every call from a PE image into an AuraLite-implemented Win32 function crosses
that boundary. Clang provides `__attribute__((ms_abi))`, which makes this a
compiler problem rather than an assembly problem — but it must be applied to
*every* exported entry point, and getting it wrong produces corruption that
looks like a random crash three calls later.

### 2.5 Honest scope: what "runs" will mean

A modern `.exe` from the internet will not run. It will import `KERNEL32`,
`USER32`, `GDI32`, `ADVAPI32`, `SHELL32`, `OLE32`, `COMCTL32`, `MSVCRT` or the
UCRT, expect a registry, a console subsystem, DLL search paths, TLS callbacks,
SEH, and often .NET or DirectX. The realistic target for this plan is:

> **A freestanding, statically linked, console or simple-GUI `.exe` built by
> mingw-w64, importing a bounded set of functions this plan implements.**

That is a real Win32 binary — the same file format, the same ABI, the same
imports — but it is not "Windows compatibility". §9 says so plainly, because
the alternative is a plan that promises Photoshop and delivers `MessageBoxA`.

---

## 3. Decisions

### D1. A personality, not an emulator

`w32` is a **subsystem personality**: a PE loader in the kernel plus a
user-space library that implements the imports. No CPU emulation (the binaries
are already x86-64), no virtualisation, no syscall interception of a real
Windows kernel. AuraLite runs the code natively and answers its imports.

### D2. Import resolution in user space, loading in the kernel

The kernel gains the minimum: recognise PE, map sections with W^X from the
section characteristics, apply base relocations, hand control to a user-space
loader stub. Everything else — walking the import directory, binding names,
implementing the functions — is user space, where a bug is a dead process
rather than a dead machine.

This mirrors what the ELF path already does well and keeps the attacker-facing
parser (imports, forwarders, delay-loads) out of Ring 0.

### D3. Declarations come from mingw-w64, vendored and attributed

`w32/include/` is populated from mingw-w64's `mingw-w64-headers`, with the
upstream licence files preserved verbatim and a `PROVENANCE.md` recording the
exact upstream version and commit. **Headers only** — none of mingw-w64's
runtime, some of which is LGPL.

Writing our own `windows.h` from scratch is the alternative. It is rejected: it
would be a worse header, it would drift, and it would put the project in the
position of asserting that its declarations are original when their whole
purpose is to be identical.

### D4. Wine and ReactOS are not to be read while contributing to `w32`

Both are licence-incompatible (§1.2). The rule is deliberately stronger than
"do not copy": a contributor who has read Wine's `user32` cannot easily prove
their `CreateWindowExA` is independent, and provenance is the asset being
protected. Permitted references are: mingw-w64 headers, published Microsoft
*documentation* (learn.microsoft.com — readable, not copyable), the PE/COFF
specification, and observed behaviour of binaries the contributor lawfully
possesses.

Contributors affirm this in the existing CLA flow (`docs/CLA_INDIVIDUAL.md`).

### D5. Map onto what exists; do not build a second GUI

`USER32` and `GDI32` are implemented as translation layers over the existing
GUI syscalls and `libauragui`. `CreateWindowExA` becomes `ag_window_create`;
`FillRect` becomes `ag_fill_rect`; `TextOutA` becomes `ag_draw_text`. Where
AuraLite has no equivalent, the function returns a documented failure rather
than growing a parallel window manager.

The compositor already does the hard part. A second one would be the largest
mistake available here.

### D6. `A` and `W` both exist; `W` is the real one internally

Win32 doubles every string entry point. `w32` implements the `W` (UTF-16)
variant as primary and makes the `A` variant a converting wrapper, because
that is the direction the format actually stores strings and because
`libauragui` takes UTF-8. This requires UTF-16↔UTF-8 conversion, which
`wchar.h` does not have today — it is phase W32-1, not an afterthought.

### D7. One bounded import set, chosen by measurement

Rather than "implement KERNEL32", phase W32-4 implements exactly the imports
that the phase's own test binaries actually reference, discovered by dumping
their import tables. The set grows only when a new gate binary needs it. This
keeps the surface auditable and stops the plan becoming an infinite one.

### D8. Explicitly out of scope, so absence is a decision

Named here so nobody has to guess:

- **No registry.** Functions that need one fail with a documented error.
- **No COM/OLE, no .NET, no DirectX, no WinSock.** Each is a plan of its own.
- **No 32-bit (i386) PE.** AuraLite is x86-64 only; WOW64 is not a goal.
- **No DLL loading initially.** Static `.exe` first; `LoadLibrary` is W32-7 and
  may end in a documented limitation.
- **No console subsystem beyond stdout/stderr** mapped to the existing TTY.
- **No SEH beyond a `setjmp`-based `__try/__except` shim** (W32-6), which is
  not real unwinding and will be documented as such.

---

## 4. Phases

### Phase W32-0 — Provenance and the legal record ✅ DONE

**Objective:** make the licensing position auditable before any code exists.

#### Tasks

- [x] `w32/PROVENANCE.md`: records every file, what it is, and the licence it
      is under, plus an explicit "these contributed nothing" list.
- [ ] Vendor mingw-w64 headers into `w32/include/`. **Deferred to W32-4**, and
      the reason is worth recording: nothing built so far needs an API
      declaration. The parser and the converter are written from the PE/COFF
      specification and the Unicode standard. Vendoring headers before there
      is a caller for them would add a licensing surface for no benefit, so
      `PROVENANCE.md` currently lists the vendored set as empty.
- [x] `w32/LICENSING.md`: the contributor-facing rules, including that Wine and
      ReactOS are forbidden as *references*, not merely as sources.
- [x] `tools/check_provenance.sh`: fails if a file under `w32/` lacks a licence
      header or is not listed in `PROVENANCE.md`, or if any file names Wine or
      ReactOS as a source, or if a `.dll`/`.sys`/`.msi` is ever committed.
- [ ] Add the D4 affirmation to the CLA checklist — deferred with the header
      vendoring, since both concern contributions that do not exist yet.

#### Test gate

- `tools/check_provenance.sh` passes, and `--selftest` proves it fails on a
  planted unrecorded file (negative control, as `test-unit` already does for
  the libc drift check).
- Every vendored file's licence is one of: public domain, ZPL-2.1, BSD-3-Clause,
  MIT. Vacuously true today: nothing is vendored yet.
- No file under `w32/` names Wine or ReactOS as a source, enforced rather than
  asserted.

**Result:** wired into `make test-unit`. Both the check and its negative
control run on every build:

```
[provenance] PASS: 5 source file(s) recorded, no forbidden sources
[provenance] self-test PASS: violation was detected as required
```

**Deliverable:** `w32/LICENSING.md`, `w32/PROVENANCE.md`,
`tools/check_provenance.sh` ✅

---

### Phase W32-1 — UTF-16 and the string layer ✅ DONE

**Objective:** the prerequisite D6 identified, landed before anything depends
on it.

#### Tasks

- [x] The UTF-16 type is a fixed `uint16_t`, not `wchar_t`. This turned out to
      be better than the planned `-fshort-wchar`: a build flag that silently
      changes what a type means is exactly the kind of implicit boundary the
      task warned about, so the core uses an explicit width and leaves
      `-fshort-wchar` to the eventual mingw-w64-facing edge.
- [x] UTF-16 ↔ UTF-8 conversion, both directions, with surrogate pairs.
- [x] Strict rejection instead of U+FFFD substitution — a replacement character
      would quietly turn a hostile filename into a different valid one.
- [x] `w32_utf16_len()` bounded by a caller-supplied maximum, so an
      unterminated buffer cannot run away.
- [ ] Broader `w32_wcs*` helpers: deferred until a caller needs them.

#### Test gate

- Host unit test: round-trip ASCII, BMP, astral (surrogate pairs), and
  malformed input (lone surrogate, truncated sequence, over-long UTF-8) —
  refused, not crashed, no over-read past the buffer.
- Conversion of a 0-length and a 1-byte buffer.

**Result:** `test_w32_utf` 13/13, clean under `-fsanitize=address,undefined`.

Every rejection case is run with the input in an exact-sized heap block, so an
over-read is a genuine out-of-bounds access ASan can see rather than a walk
into a convenient trailing zero. Covered: overlong `C0 80`/`E0 80 80`/`C1 BF`/
`F0 80 80 80`, truncated 2/3/4-byte sequences, bad continuation bytes, stray
`0x80`, 5-byte sequences, UTF-8-encoded surrogates (`ED A0 80`), `U+110000`,
lone high and low surrogates, high-followed-by-high, and high-followed-by-ASCII.

One contract bug was found by the tests and fixed: a measuring call
(`dst == NULL`) returned `ERR_SPACE` instead of `OK`, contradicting the
documented two-pass sizing protocol.

**Deliverable:** `w32/include/w32/w32_utf.h`, `w32/src/w32_utf.c`,
`tests/unit/test_w32_utf.c` ✅

---

### Phase W32-2 — PE32+ parsing, in user space, offline ✅ DONE

**Objective:** understand the container with no kernel risk at all.

#### Tasks

- [ ] Parse DOS header, `e_lfanew`, NT headers, optional header, sections.
- [ ] Parse import directory, base relocation table, exports.
- [ ] `tools/peinfo` — a host tool that dumps all of it.
- [ ] Validate: `Machine == IMAGE_FILE_MACHINE_AMD64`, `Magic == PE32+`.

#### Test gate

- `tools/peinfo build/boot/BOOTX64.EFI` agrees with `llvm-readobj --file-headers`
  on every field. **The project's own EFI binary is the first test fixture** —
  no external file needed, and the reference output is already obtainable:

  ```
  $ llvm-readobj-19 --file-headers build/boot/BOOTX64.EFI
  Format: COFF-x86-64
  ImageFileHeader {
    Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664)
    SectionCount: 3
    OptionalHeaderSize: 240
  ```

  `objdump -f` reports the same file as `pei-x86-64` and is an acceptable
  fallback, so the gate adds no hard new dependency beyond binutils.
- Malformed inputs refused without crash or over-read: `e_lfanew` past EOF,
  section count of 0xFFFF, `SizeOfRawData` beyond the file, relocation block
  with a bogus size, import descriptor with a name RVA outside any section.
- A fuzz corpus of truncated/bit-flipped PEs: no crash, no hang.

**Result:** `test_w32_pe` 20/20, clean under `-fsanitize=address,undefined`.
`tools/peinfo` agrees with `llvm-readobj-19` on all nine compared header fields
of `BOOTX64.EFI`, checked mechanically by `tests/unit/test_w32_peinfo.sh`
rather than by eye.

Beyond the listed cases the gate also covers: PE32 (32-bit) and i386 refusal,
non-power-of-two and inverted alignments, `NumberOfRvaAndSizes` overflow, an
`e_lfanew` that overlaps the DOS header, RVA translation straddling the end of
a section's raw data, odd-sized relocation blocks, and a W^X section pair. The
fuzz sweep parses every 7-byte prefix of a valid image and every third bit of
its header region, walking imports and relocations on anything that parses.

The property worth naming: `BOOTX64.EFI` parses perfectly and is still refused
by `pe_check_loadable()`, because its subsystem is `EFI_APPLICATION`. The
project's own firmware binary can never be launched as a user process.

**Deliverable:** `w32/include/w32/w32_pe.h`, `w32/src/w32_pe.c`,
`w32/tools/peinfo.c`, `tests/unit/test_w32_pe.c`,
`tests/unit/test_w32_peinfo.sh` ✅

---

### Phase W32-3 — The kernel PE loader ✅ DONE

**Objective:** map a PE image into a process the way `elf.c` maps an ELF.

#### Tasks

- [x] `kernel/proc/pe.c`, modelled on `elf.c`: same page-flag derivation, the
      same permission-union rule for a page two sections share, the same
      "zero every new frame before user space sees it".
- [x] Map sections at `ImageBase`; apply base relocations when it is taken.
      The loader also maps the image's own headers read-only at its base,
      because a PE expects to reach them through its module handle.
- [x] W^X from section characteristics, enforced in `pe_check_loadable()` and
      unit-tested without a kernel.
- [x] Only `WINDOWS_CUI`/`GUI` accepted; every EFI subsystem refused.
- [x] The exec path selects a loader by the file's own magic
      (`pe_image_probe()`), not by filename, so a `.exe` that is really an ELF
      is still handled correctly.
- [ ] `execpolicy` integration: not needed yet. A PE goes through the same
      `vfs_open`/`spawn` path as an ELF and is already covered by the existing
      install-directory policy; a PE-specific rule would be a second mechanism
      with nothing extra to say. Recorded rather than silently dropped.

#### One decision worth naming

The relocation buffer is a fixed `PE_MAX_KERNEL_RELOCS` (4096) static array,
and an image needing more is **refused**. The alternative was a heap
allocation on a Ring 0 path that must stay allocation-free on its error
routes. A freestanding mingw-w64 `.exe` produces tens of entries, so the cap
is far above real use, and refusing is safe where silently truncating a
relocation table would not be.

#### Test gate

- A minimal hand-built PE (assembled in-tree, no external dependency) loads,
  runs, and exits with a known code.
- The same PE with `ImageBase` forced to a taken address loads via relocations
  and produces the identical result.
- W^X: a section marked `MEM_WRITE|MEM_EXECUTE` is refused, matching the ELF
  loader's behaviour, with a test that fails without the check.
- Hostile images from W32-2's corpus are refused by the kernel with no fault.
- `test_elf_permissions` unchanged — evidence the shared paths were not
  disturbed.

**Result:** `test_w32_pe_loader` 12/12, registered in `run_all.sh`.

Three fixtures, all built in-tree with `nasm -f win64` + `lld-link` (both
already in `REQUIRED_TOOLS`, since they build `BOOTX64.EFI`), so the gate
depends on no downloaded binary:

| Fixture | What it proves |
|---|---|
| `petest.exe` | Loads at its own `ImageBase`, runs, exits 77 |
| `petest_reloc.exe` | Linked at `0x800000000000` (== `USER_VADDR_TOP`, never honourable) → relocated to the fallback base, **identical output** |
| `petest_efi.exe` | One byte different — subsystem 3 → 10 — and refused |

The marker is printed through a pointer that lives in `.data` and is itself a
relocation target, so identical output from the relocated image is what proves
the fixups were right rather than merely absent.

Two things the tests caught that a smoke test would not have:

* The first relocation fixture only patched `ImageBase` in the header, leaving
  the baked-in pointer inconsistent — the image loaded but printed nothing.
  Fixed by *linking* at an impossible base instead of editing one field, which
  is also a more honest fixture.
* `spawn()` gives each process a fresh address space, so `0x40000000` is free
  even though every native binary is linked there. Forcing the relocation path
  needs a base outside the user range, not merely one that "looks" taken.

Hostile-image handling inherits W32-2's fuzz corpus: `pe_parse()` is the same
code in both, so the kernel path is covered by the host test that already runs
under ASan/UBSan. Boot remains 30 self-test PASS / 0 FAIL with no faults, and
`make test-unit` is unchanged at 110.

**Deliverable:** `kernel/proc/pe.{c,h}`, `w32/tests/petest.asm`,
`tools/mk_pe_efi_variant.py`, `tests/integration/cases/test_w32_pe_loader.sh` ✅

---

### Phase W32-4 — `KERNEL32`: the bounded first import set ✅ DONE

**Objective:** a console `.exe` that prints and exits.

#### Tasks

- [x] `w32/src/kernel32.c`, every export `W32ABI` (= `ms_abi`), spelled the
      same way so a missing annotation reads as a missing token in review.
- [x] 16 exports, exactly what the gate binaries import (D7): `GetStdHandle`,
      `WriteFile`, `ReadFile`, `CreateFileA`, `CloseHandle`, `ExitProcess`,
      `GetLastError`/`SetLastError`, `VirtualAlloc`/`VirtualFree`,
      `GetProcessHeap`/`HeapAlloc`/`HeapFree`, `GetCommandLineA`, `Sleep`,
      `GetTickCount64`.
- [x] A `HANDLE` table mapping to AuraLite fds.
- [x] Win32 error codes, set on every failure path, with one shared
      errno→Win32 mapping so no wrapper invents its own.
- [x] Import binding (`w32/src/w32_bind.c`) against a static export table, in
      user space per D2.
- [ ] `CreateFileW` and `GetCommandLineW`: deferred. Nothing in the gate set
      imports them, and D7 says the surface grows when a binary needs it. The
      UTF-16 layer they will sit on is already there from W32-1.

#### Two things worth naming

**`HANDLE`s are minted from a table, never cast from an fd.** Casting would
make fd 0 indistinguishable from a NULL handle, and would let a program forge
a handle by inventing an integer. The table means an unknown value is
rejected instead of dereferenced, and the test asserts that `(HANDLE)0`,
`(HANDLE)1`, `(HANDLE)3` and `INVALID_HANDLE_VALUE` all fail to resolve.

**Closing a standard handle succeeds and does nothing.** A program that calls
`CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE))` is common; actually closing fd
1 would take stdout away from the rest of the process.

#### Test gate

- A `.exe` that imports `KERNEL32` prints over serial and exits with its own
  status.
- Every implemented function has a failure-path test: bad handle → `FALSE` +
  `GetLastError() == ERROR_INVALID_HANDLE`, not a crash.
- `ms_abi` correctness: a function taking 6 integer args and returning a struct
  by value gets every argument intact. **This is the test that catches the
  convention bug from §2.4, and it is written before the other functions.**

**Result:** `test_w32_abi` 7/7, `test_w32_kernel32` 21/21,
`test_w32_kernel32.sh` 10/10 in the guest.

The ABI test was written first, as the plan required, and it is the reason
this phase is trustworthy. Verifying the convention from C alone is
impossible — a wrong caller and a wrong callee agree with each other — so the
calls come from hand-written assembly that places arguments where the
*Windows* ABI says they go. It covers six integer arguments, a 32-byte struct
returned through the hidden pointer (RCX on Windows, RDI on System V), mixed
widths, and the callee-saved set, which on Windows adds RSI and RDI.

`tests/unit/test_w32_abi_negctl.sh` is its negative control: it rebuilds the
same test with `W32ABI` defined empty and asserts the result **fails**.
Without that, a test that passes proves nothing about a convention whose
failure mode is silent. It does fail — with a segfault — which is exactly the
corruption §2.4 warned about.

Two real bugs the tests caught before anything used them:

* **`GetStdHandle` returned a handle no function could resolve.** The
  selectors are `DWORD`s, so `(HANDLE)(intptr_t)STD_OUTPUT_HANDLE` is
  `0x00000000FFFFFFF5`, while the comparison sign-extended `-11` to
  `0xFFFFFFFFFFFFFFF5`. Every standard handle silently failed to resolve —
  i.e. every `printf` in every guest program would have failed. Fixed by
  comparing the low 32 bits, which is what the value actually is.
* The measuring-call contract bug in W32-1, found the same way.

One limitation stated rather than hidden: `test_w32_abi` is **not** built with
`-fsanitize=address`. ASan's prologue assumes a System V frame and faults
inside an `ms_abi` callee entered from the hand-written caller; the same
binary is correct at `-O0`, `-O1`, `-O2` and `-O3` without it. The ABI shims
allocate nothing, and the code in this phase that *does* manage memory
(`kernel32.c`) is built under ASan+UBSan by `test_w32_kernel32`.

**Deliverable:** `w32/{include/w32,src}/w32_abi.h, w32_errno.*, w32_handle.*,
kernel32.*, w32_bind.*`, `userspace/apps/w32run/`, `w32/tests/kernel32_test.asm`,
`tests/unit/test_w32_{abi,kernel32}.c`, `tests/unit/test_w32_abi_negctl.sh`,
`tests/integration/cases/test_w32_kernel32.sh` ✅

---

### Phase W32-5 — `USER32` + `GDI32` onto the existing compositor ✅ DONE

**Objective:** a window on screen, drawn by a PE binary.

#### Tasks

- [x] `RegisterClassExA`, `CreateWindowExA`, `ShowWindow`, `UpdateWindow`,
      `DestroyWindow` → `ag_window_*`. Window styles map to `AG_WIN_*` flags;
      bits with no equivalent are ignored rather than approximated.
- [x] A message loop: `GetMessageA`/`PeekMessageA`/`DispatchMessageA` over
      `ag_poll_event`, with an explicit translation table covering `WM_PAINT`,
      `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR`, `WM_MOUSEMOVE`, the L/R button
      messages, `WM_SIZE`, `WM_SETFOCUS`/`WM_KILLFOCUS`, `WM_CLOSE` and
      `WM_DESTROY`.
- [x] `WNDPROC` dispatch — an `ms_abi` callback *into* the PE image.
- [x] `BeginPaint`/`EndPaint`, `FillRect`, `TextOutA`, `MoveToEx`/`LineTo`,
      `SetPixel`, `SetTextColor`, `CreateSolidBrush` → `ag_*`.
- [x] `MessageBoxA` over `ag_alert`.
- [ ] `BitBlt` and the `W` (UTF-16) entry points: deferred. Nothing in the
      gate imports them (D7), and `BitBlt` needs an off-screen surface, which
      needs a DC model this phase deliberately does not have.

#### Three things worth naming

**A DC names a window and nothing else.** There is no off-screen surface, no
compatible DC and no object selection, so `CreateCompatibleDC` is *absent*
rather than stubbed — a program that needs it fails at the import, which is
findable, instead of at a later call that silently drew nowhere.

**`COLORREF` is `0x00BBGGRR`, AuraLite is `0x00RRGGBB`.** The conversion is a
named function with its own test, because getting it backwards produces a
picture that looks plausible with red and blue swapped — the kind of bug that
survives a smoke test.

**Brushes carry their colour in the handle.** `CreateSolidBrush` returns the
colour cast to a handle, so there is no object table and `DeleteObject` cannot
leak. That is enough for the one GDI object this phase uses, and it is
honest about being enough rather than pretending to be a GDI object manager.

#### Test gate

- A `.exe` creates a window, paints, and closes cleanly.
- `WNDPROC` is entered with the correct `hwnd`/`msg` — the callback direction
  of the ABI, which W32-4's test does not cover.
- A window owned by a killed PE process is reaped.
- Hostile: a `WNDPROC` pointer outside the image — survivable, no kernel fault.

**Result:** `test_w32_user32` 11/11, registered in `run_all.sh`.

The fixture is a hand-written PE that imports 21 functions across
`KERNEL32`, `USER32` and `GDI32`, registers a class, creates a window, paints
through `BeginPaint`/`FillRect`/`TextOutA`/`LineTo`, and destroys it. Its
`WNDPROC` records which messages arrived and the program's exit status (66) is
reached only if all of them did, so a partial failure is distinguishable from
a crash.

`WM_CREATE` is asserted to arrive **synchronously, from inside
`CreateWindowExA`**, because a program that allocates its state there depends
on that ordering.

The hostile fixture (`tools/mk_pe_badwndproc.py`) is the same `.exe` with one
32-bit displacement rewritten so the `WNDPROC` points outside the image. The
result is exactly what the gate wanted:

```
[EXCEPTION] Page Fault ... from USER mode (cpu1)
[signal] terminate pid=9 by signal 11
[gui] cleaned 1 window(s) for pid 9
```

A user-mode fault, only that process killed, and the compositor reaping the
window it had already created — which is the `gui_cleanup_process` requirement
in the same gate, demonstrated rather than assumed.

One bug found along the way, and it was in the *test*, not the personality:
the fixture's `WNDPROC` prologue reserved `0x60` bytes after four pushes,
leaving `RSP % 16 == 8` at its first call and producing a General Protection
Fault. Windows x64 requires 16-byte alignment at every `CALL`. Worth recording
because it is the same class of mistake `ms_abi` exists to prevent, and here
it was on the hand-written side where no attribute could help.

**Deliverable:** `w32/include/w32/user32.h`, `w32/src/user32.c`,
`w32/tests/user32_test.asm`, `w32/tests/{user32,gdi32}.def`,
`tools/mk_pe_badwndproc.py`, `tests/integration/cases/test_w32_user32.sh` ✅

---

### Phase W32-6 — CRT startup, TLS and a minimal SEH ✅ DONE

**Objective:** the things a real compiler emits that a hand-written `.exe`
avoids.

#### Tasks

- [x] `argc`/`argv` built from the command line with the documented quoting
      rules, in `w32/src/w32_argv.c`.
- [x] TLS directory: TLS callbacks run at startup, TLS index written.
      **The block is per-process, not per-thread** — see the note below.
- [x] `__try`/`__except` on `sigsetjmp`/`siglongjmp`, with
      `SetUnhandledExceptionFilter`.
- [x] Static-initialiser sections (`.CRT$XC*`) run in order.
- [ ] Moving import binding into the kernel exec path: **deferred**. It is
      separable from the CRT work, it is the one part of this phase that
      touches the kernel, and W32-7 (`LoadLibrary`) will change what the
      binder needs to do. Doing it now would mean writing it twice. The
      consequence — `w32run` maps the image RW+X rather than per-section
      W^X — is recorded in `docs/win32.md`.

#### Three things worth naming

**The kernel already delivered faults to user handlers, so SEH needed no
kernel change.** `kernel/arch/x86_64/isr.c` maps `#DE` to `SIGFPE` and `#PF`
to `SIGSEGV` and calls `signal_raise_fault()`, which installs a handler frame
and returns to it. That turned this phase from "modify the fault path" into
"use the fault path that exists" — less code and much less risk. Checking
this first, before designing anything, was the single highest-value step in
the phase.

**`sigsetjmp`, not `setjmp`.** A signal is blocked while its own handler
runs. Jumping out with plain `longjmp` leaves it blocked forever, so the
*first* divide by zero is caught and the *second* kills the process. A test
that faults once cannot see this, which is exactly why the gate faults twice
in a row. AuraLite's libc implements `sigsetjmp`/`siglongjmp` with a real
`sigprocmask` save/restore, so the correct primitive was already available.

**TLS is per-process because GS belongs to the kernel.** On Windows, TLS is
reached through the TEB at `GS:[0x58]`. On AuraLite `IA32_GS_BASE` holds the
per-CPU pointer and the `SYSCALL` stub reads `[gs:...]` directly **with no
`swapgs` anywhere in the tree** (`syscall_entry.asm`, `cpu_local.c`). Giving
user mode its own GS base means introducing `swapgs` on every kernel entry
and exit — a kernel-wide change, not a personality change. So the TLS
callbacks and the TLS index are implemented and the block is per-process,
which is invisible while `w32run` is single-threaded and is the first thing
that breaks when threads arrive. Recorded in `docs/win32.md` rather than
left to be discovered.

**Locals live across a `longjmp` only if they are `volatile`.** GCC's
`-Wclobbered` caught this in the test program itself: counters written inside
a `__try` body and read after a fault had jumped out. It is undefined
behaviour (C11 7.13.2.1), not a false positive, and the fix was to give those
variables static storage rather than to silence the warning.

#### Test gate

- A `.exe` with a static initialiser runs it before the entry point.
- Command-line parsing matches documented Win32 quoting for: quoted args,
  embedded quotes, backslash runs before a quote, empty args.
- A divide-by-zero inside `__try` reaches `__except` rather than killing the
  process; the same fault *outside* `__try` terminates the process cleanly with
  no kernel fault.
- Honest note to record in the phase: this is not table-driven unwinding.
  Destructors of live C++ objects will not run. Say so in `docs/win32.md`.

**Result:** `test_w32_crt` 23/23 and `test_w32_argv` 139/139 (host, under
ASan+UBSan). Both registered; `docs/win32.md` written and linked from
`docs/README.md`.

Command-line parsing is tested against **Microsoft's own published examples**
rather than paraphrased cases — the five-row table from "Parsing C
command-line arguments", asserted verbatim, plus the `a"b"" c d` row. Using
the documented table is also the legally relevant choice (D3): the rules come
from documentation, not from disassembling a CRT. The tests were then checked
for sensitivity by mutating the implementation: disabling the odd-backslash
rule and making `argv[0]` use the general rules each produced failures, so
the suite is not passing by accident.

`argv[0]` is parsed by different rules from every other argument —
backslashes in it are always literal, because it is a path. A parser that
applied the general rule would mangle `C:\dir\` in a way that only shows up
for programs run from certain directories.

The startup order is asserted, not assumed: the fixture's TLS callback
records whether the constructor had already run, so `CRT-ORDER-OK` proves
TLS callbacks ran *before* static initialisers and both before the entry
point.

The hostile fixture (`tools/mk_pe_badtls.py`) points the first TLS callback
outside the image. This is the sharpest hostile case in the personality so
far: the callback array is data straight out of the file and it is followed
*before any of the program's own code runs*, so a loader that dereferenced it
unchecked would hand control to whatever the file named. The loader refuses
before transferring control — strictly stronger than catching a fault
afterwards, which is what the W32-5 WNDPROC case does.

One bug found, again in a fixture rather than the personality: `puts_raw`
used `R12` as scratch without saving it. `R12` is callee-saved in the Windows
x64 ABI, so the caller's value was corrupted and the return path faulted.
That is the second fixture-side ABI defect in three phases (after the W32-5
stack-alignment bug), which is itself the finding: hand-written Windows-ABI
code is where these mistakes live, and it is why the fixtures are kept small
and the personality is written in C with `ms_abi` doing the work.

**Deliverable:** `w32/include/w32/{w32_crt,w32_argv}.h`,
`w32/src/{w32_crt,w32_argv}.c`, `w32/tests/crt_test.asm`,
`userspace/apps/w32run/sehtest.c`, `tools/mk_pe_badtls.py`,
`tests/unit/test_w32_argv.c`, `tests/integration/cases/test_w32_crt.sh`,
`docs/win32.md` ✅

---

### Phase W32-7 — `LoadLibrary`, or a documented refusal

**Objective:** find out whether dynamic loading is reachable, and say so either
way.

#### Tasks

- [ ] `LoadLibraryA/W`, `GetProcAddress`, `FreeLibrary` for `w32`'s *own*
      built-in modules (`kernel32`, `user32`, `gdi32`) — a name table, not a
      loader.
- [ ] Investigate loading a real user-supplied DLL: relocations, imports,
      `DllMain`, per-process module list.
- [ ] Delay-load and forwarder imports: detect and refuse explicitly, or
      support.

#### Test gate

- `GetProcAddress(GetModuleHandle("kernel32"), "WriteFile")` returns the
  implementation and calling through it works.
- A missing export returns `NULL` + `ERROR_PROC_NOT_FOUND`, never a crash.
- If real DLL loading lands: a two-DLL cycle, a self-importing DLL, and a DLL
  whose `DllMain` fails are all handled without leaking the address space.
- **This phase is allowed to end in documentation.** If per-process module
  lists prove to need address-space work the kernel does not have, that is a
  finding, and `TODO.md` records it rather than a half-loader being merged.

**Deliverable:** `patches/W32_7_loadlibrary.patch`

---

### Phase W32-8 — Integration, documentation and the honest matrix

**Objective:** make it usable and make its limits legible.

#### Tasks

- [ ] `docs/win32.md`: supported functions, behaviour notes, the ABI boundary,
      and a blunt statement of what will not run.
- [ ] `docs/status.md` gains a `w32` row.
- [ ] `w32/examples/`: console, GUI, and a deliberately unsupported binary that
      fails with a clear message.
- [ ] `make w32-sdk`: mingw-w64 cross-build instructions, mirroring `make sdk`.
- [ ] Shell: `run app.exe` detects PE by magic and routes to the PE loader.
- [ ] `README.md` gains a `w32` entry with the §1.3 disclaimer.

#### Test gate

- Every example builds with mingw-w64 and runs.
- The unsupported binary produces the documented message and a clean exit, not
  a hang or a fault.
- `docs/win32.md`'s function table is generated from the source, so it cannot
  drift (the `sdk-check` pattern).
- Full `make test` green, including every prior phase's gate.

**Deliverable:** `patches/W32_8_integration.patch`

---

## 5. Order and rationale

| Phase | Why here |
|---|---|
| W32-0 | The licence position must be auditable before code exists, not after |
| W32-1 | D6 makes UTF-16 a dependency of every string entry point |
| W32-2 | Parse offline where a bug is a failed unit test, not a triple fault |
| W32-3 | The kernel change, once the parser it depends on is proven |
| W32-4 | The ABI boundary gets its own gate before any breadth is added |
| W32-5 | The visible payoff, and it reuses the compositor rather than growing one |
| W32-6 | Only now do real compiler-emitted binaries become the target |
| W32-7 | Most likely to end in a limitation; costs least when discovered last |
| W32-8 | Documentation last, when there is something true to document |

**If only one phase is ever built, build W32-2.** It is pure user space, it
needs no kernel change, its first test fixture is a file the repository already
produces, and it answers the question "do we understand this format?" before
anything is risked on the answer.

**If the plan is abandoned after W32-4**, the result is still coherent: a
console-only Win32 personality with a documented import set. That is a
defensible stopping point, which is why the phases are ordered to reach it
early.

---

## 6. Risks

**The calling convention is a silent corrupter.** A missing `ms_abi` compiles,
links, and returns plausible garbage — the failure surfaces later and elsewhere.
W32-4's first test is the ABI test for exactly this reason, and every export
must carry the attribute; a lint in `check_provenance.sh`'s style that greps for
un-annotated exports would be cheap insurance.

**Scope creep is the defining risk of this plan.** Every implemented function
reveals three more that a real binary wants. D7 bounds it by measurement, and
D8 names the big absences, but the pressure is constant and the plan will fail
by dilution before it fails technically.

**Provenance contamination.** One contributor who consults Wine to fix a
stubborn `WM_PAINT` bug can taint a file's history irreversibly. D4 is a rule,
but a rule is not a mechanism. The CLA affirmation and a visible `LICENSING.md`
are the mitigation; there is no way to detect a violation after the fact, which
is precisely why the rule is stated before any code is written.

**The PE parser is attacker-facing.** It reads a wholly untrusted file the user
was told to obtain elsewhere. Every offset, RVA and count is hostile input.
W32-2 and W32-3 list malformed inputs first in their gates, and the fuzz corpus
is not optional.

**A PE loader in the kernel is new Ring 0 attack surface.** D2 keeps imports
out of the kernel, but section mapping and relocation still run privileged. The
mitigation is that `pe.c` is modelled on `elf.c` — same validation habits, same
W^X derivation — so it inherits a reviewed design rather than inventing one.

**"It runs Windows programs" will be over-read.** The moment a screenshot of
`MessageBoxA` exists, the claim will outrun the truth. §2.5 and `docs/win32.md`
exist to be quotable in the other direction. This is a reputational risk, not a
technical one, and it is the most likely to actually occur.

**Trademark drift.** A contributor naming something `windows_compat` or putting
a Windows logo in the GUI creates a problem no code review usually looks for.
§1.3 fixes the internal name as `w32` for this reason.

---

## 7. What this plan does not do

- No registry, COM, OLE, .NET, DirectX, WinSock, or printing.
- No 32-bit PE, no WOW64.
- No table-driven SEH unwinding; the `__try` shim does not run C++ destructors.
- No redistribution of any Microsoft binary, ever.
- No attempt to run a `.exe` obtained from a shop or a download site; the
  target is binaries the user builds with mingw-w64.
- No claim of Windows compatibility, in the README, in `docs/`, or in a commit
  message.
- No ABI stability guarantee for `w32` across releases, for the same reason
  `SDK_PLAN.md` declines to make one for the syscall table.

---

## 8. A note on why this is worth doing at all

The honest case for this plan is not "AuraLite will run Windows software". It
is that a PE loader, a foreign calling convention, an import resolver and a
personality layer are the four things a hobby OS most often never gets to
build, and this repository is unusually close to all four: it already emits
PE32+, already has per-process address spaces, already has a compositor worth
mapping onto, and already has the testing culture to keep an attacker-facing
parser honest.

The Win32 API is also the best-documented large API in existence with a
permissively licensed set of declarations already available — which is why it
is the right target, and why §1 could be written as confidently as it was.

If the outcome is a console `.exe` and a window, with a clear document saying
that is all it is, that is a real result and this plan will have been worth
following.
