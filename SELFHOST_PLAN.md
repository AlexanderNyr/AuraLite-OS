# AuraLite OS — Self-Hosting Plan

## Status: IN PROGRESS 🚧 — SH0–SH3 landed; SH4 split into SH4a–SH4d (see below); SH5–SH9 pending

| Phase | Result |
|-------|--------|
| SH0 — Rules, receipts, checker | ✅ landed |
| SH1 — Runtime limits + TinyCC userspace port | ✅ landed |
| SH2 — tcc builds the userland | ✅ landed |
| SH3 — `aulink`: the self-host linker | ✅ landed |
| SH4 — an assembler that runs in-guest (umbrella; split into SH4a–SH4d) | 🚧 pending |
| SH4a — spike (D4 decision) + mini-asm core + first byte-identical flat object | 🚧 pending |
| SH4b — `-f bin` byte-parity across all four boot files | 🚧 pending |
| SH4c — ELF64 backend, readelf parity on the kernel/libc objects | 🚧 pending |
| SH4d — ELF32 backend + the in-guest assembly run | 🚧 pending |
| SH5 — the kernel, built by itself | 🚧 pending |
| SH6 — shmake + shell scripting | 🚧 pending |
| SH7 — image tooling in C | 🚧 pending |
| SH8 — bootstrap closure | 🚧 pending |
| SH9 — cross-arch + CI wiring | 🚧 pending |

This document answers:

> *AuraLite builds itself with clang, ld.lld, nasm, python3, tar, mtools and
> rustc running on a Linux host. What would it take for AuraLite to build
> itself **with itself** — a bootable ISO produced inside the guest, by
> programs the guest built, with no host toolchain in the loop?*

It follows the structure of the existing plans (`OPT_PLAN.md`,
`MATURITY_PLAN.md`, `USB_PLAN.md`): dependency-ordered phases, a definition
of done and a test gate for every phase, and a machine-checked claim checker
(`tools/check_selfhost_claims.py`) so the status table cannot drift from the
tree the way AUDIT_A7 caught other plans drifting.

**Baseline** (measured on this machine, 2026-08-26; commit `969a531` HEAD):

```
build/kernel.elf       2 485 960 bytes
build/kernel32.elf       378 136 bytes
build/initrd.tar       3 727 360 bytes (98 files, 10 subdirectories)
build/auralite.iso    51 380 224 bytes (hybrid BIOS+UEFI)
boot → shell                ~2 868 ms @ 99 Hz (QEMU TCG, -smp 4, selftest=fast)
make iso (cold)             ~47 s host wall-clock
make test-unit              ~157 s host wall-clock
REQUIRED_TOOLS      clang ld.lld nasm cc python3 tar mformat mcopy lld-link rustc
```

A self-hosting plan is the easiest kind to write badly, because "the OS
builds itself" is cheap to claim and expensive to verify — the guest is
exactly the party you cannot trust. One rule above all others, inherited
from `OPT_PLAN.md`: **every phase lands with a serial receipt the HOST
greps.** The final proof of every phase is a line printed by the guest and
asserted by a host-side integration case; the final proof of the whole plan
is the host booting an ISO the guest built.

---

## 1. Where things actually stand

Measured against the tree. Line numbers are from the baseline commit; if
they drift, the *claims* are what the phases answer to.

### Fact 1 — Every tool in the build is a host program

`Makefile:21` declares `REQUIRED_TOOLS := $(CC) $(LD) $(AS) $(HOST_CC) python3
tar mformat mcopy lld-link rustc` — clang, ld.lld, nasm, a host cc for the
offset generators, python3 for the image/offset generators, tar for the
initrd, mtools for the ESP, lld-link for the EFI app, rustc for `rsbr`.
None of these can run on AuraLite today. The self-host toolchain is a
replacement set, not a port of one tool.

### Fact 2 — The kernel CFLAGS are clang-specific, and one flag is load-bearing

`Makefile:27-38`: `--target=x86_64-elf -mcmodel=kernel -mno-red-zone
-fno-pie -fno-pic -fstack-protector-strong -ffunction-sections
-fdata-sections`. TinyCC supports none of the clang-only spellings and does
not implement `-mcmodel` at all. `-mcmodel=kernel` exists because the kernel
links at `0xFFFFFFFF80100000` (higher half, `kernel.ld`): GCC/clang's small
model emits 32-bit absolute relocations that cannot represent that address.
**Whether TinyCC's x86_64 codegen — which is almost entirely RIP-relative —
is linkable at the higher-half link address is a measured question, not an
assumed one** (SH5 spike; the V0 pattern of this tree applies).

### Fact 3 — Linking is `ld.lld -T kernel.ld --gc-sections`

TinyCC has a built-in linker but **no linker-script support**. `kernel.ld`
is small and stable (ENTRY, PHDRS, section placement, `_start`/stack
symbols); `--gc-sections` + `-ffunction-sections` are host-side footprint
optimisations (OPT O8), not semantics. The plan therefore builds **`aulink`**,
a purpose-built ELF linker with a `kernel.ld`-subset parser (SH3), rather
than porting lld (LLVM-scale) or forking tcc's linker.

### Fact 4 — Assembly is NASM, in a small, stable dialect

The tree assembles ~30 files: `boot/bios/stage1/*`, `boot/bios/stage2/*`
(flat binaries, `-f bin`), `kernel/arch/*/isr_stubs*.asm`,
`syscall_entry.asm`, `context*.asm`, `boot.asm`, `boot32.asm`,
`ap_trampoline.asm` (generated from `ngen_ap_trampoline_inc.py`), per-arch
crt0/syscall wrappers (`-f elf64`/`elf32`). The dialect is flat NASM syntax
with `%include`; no macros beyond the tree's own. Two candidate paths —
port nasm (a self-contained C program, ~150 kLOC, needs a real libc) or
write `mini-asm` for the exact in-tree subset — are a SH4 spike decision
(D4), with byte-identical-object parity as the gate.

### Fact 5 — Image tooling is host scripts with host interpreters

`tools/mkinitrd.sh` (tar), `tools/mkisoimage_dual.sh` (mformat/mcopy +
**inline python3** patching the FAT BPB), `tools/gen_boot_offsets.c`
(host `cc` — this one is already portable C), `tools/gen_user_binary.py`,
`tools/ngen_ap_trampoline_inc.py`, `tools/gen_wv_cp1251_font.py`,
`tools/mkapkg.c`. Each needs a C twin that runs in-guest before the loop
can close (SH7).

### Fact 6 — Runtime limits a real compiler workload hits today

| Limit | Value | Where | Why it matters |
|---|---:|---|---|
| Exec image cap | **1 MiB** | `kernel/proc/process.c:771` `SPAWN_MAX_IMAGE` | a static TinyCC binary is ~1.5–2 MiB → `spawn` refuses |
| User stack | **1 MiB** | `USER_STACK_SIZE`, 4 sites (`syscall.c:262`, `guard.c:14`, `process.c:60`, `user.c:26`) | tcc/make recursion depth wants 4–8 MiB |
| tmpfs files | **64 per volume** | `kernel/fs/tmpfs.c:29` `TMPFS_MAX_FILES` | the source tree is 1269 files; a build tree cannot live in tmpfs |
| Open FDs | **64** | `VFS_MAX_FDS` / `OPEN_MAX` | probably adequate; verified by spike, not assumed |
| Path length | 256 | `VFS_PATH_MAX` | adequate for a /fat worktree |

The first three are one-line constants that SH1 raises (with the honest
note that 1 MiB was an arbitrary cap, and that raising it is a deliberate
widening of the exec surface, not a free lunch).

### Fact 7 — The libc already has the POSIX surface a C toolchain needs

Verified in-tree: `execve`/`execv`/`execvp` (`lib/libc/src/libc.c:582`),
`posix_spawn` (`lib/libc/src/posix_spawn.c`), `mmap`/`munmap`/`brk`, signals
with SA_RESTART, termios, stdio over VFS fds, `strtod` (P10), `qsort`,
`getenv`, `regex`, `fnmatch`. TinyCC's own POSIX needs (open/read/write/
mmap/stat/getenv/setjmp/time) are inside the shipped libc today — the port
is a link, not a libc rewrite. TinyCC does not require `fork`/`exec` for a
single `tcc -o out in.c` invocation (it compiles and links in one process),
which sidesteps the simplified `fork` semantics for the compiler itself;
`posix_spawn` covers the build-driver side.

### Fact 8 — The shell has no pipes, redirects or scripting

`userspace/system/smallsh/smallsh.c` (verified): `run <path>` dispatches to
`spawn()` with argv marshalling (`kernel/proc/process.c` snapshots
argv/envp, `EXEC_MAX_ARGS`), but there is **no pipe, redirect, variable,
loop or script support** (0 hits). Build scripts cannot run until SH6.
The spawn/argv machinery is already a solid foundation to build on.

### Fact 9 — Persistent writable storage exists and is exercised

`/fat` (FAT32: subdirs, LFN read+write, mkdir/rmdir/rename, FSInfo) and
`/ext2` (own in-kernel mkfs) are ✅ in `docs/status.md`. QEMU test disks are
created by `tools/run_qemu.sh`: `disk.img` 16 MiB (FAT32) + `ext2.img`
8 MiB. A self-host worktree wants a larger build disk (e.g. 256 MiB FAT32)
— a QEMU invocation parameter, not a kernel change (D6).

### Fact 10 — Licensing precedent already exists in this tree

TinyCC is **LGPL-2.1**; AuraLite is Apache-2.0. This is the same class of
problem `doom/doomgeneric_auralite.c` already solved: GPL-2.0 DOOM sources
are **not vendored** — `make doom` fetches and builds them on the user's
machine, and the repo ships only AuraLite's own platform layer. The
self-host toolchain follows the same rule (D8): no TinyCC source in this
repository; `make selfhost-deps` fetches it once and the user's own build
produces the binaries. The repo ships the port/glue only.

---

## 2. Decisions

### D1. Definition of self-hosting

Two stages, both measured:

- **Stage 1 (target loop):** AuraLite, running in QEMU on its own kernel,
  compiles, links and packages a bootable `auralite.iso` from the source
  tree on `/fat`, using only binaries it built itself. The host then boots
  that ISO to the shell and greps the standard boot receipts.
- **Stage 2 (closure):** the toolchain itself (tcc, aulink, shmake,
  mini-asm) is rebuilt on AuraLite by earlier stages of itself — the
  classic bootstrap chain — and the full loop runs twice in a row from a
  clean worktree with no host tools.

x86_64 is the first target; i386/riscv64/aarch64 are SH9 (the kernels
already cross-build from one tree, and TinyCC upstream carries codegen for
all four — but "upstream carries it" is a fact about tcc, not about our
build, so SH9 spikes it).

### D2. The compiler is TinyCC — not GCC, not LLVM, not a new compiler

TinyCC is the standard choice for hobby-OS self-hosting: ~80 kLOC of C, no
configure, single-pass, self-hosting by design, one small binary, codegen
for x86_64/i386/riscv64/aarch64 upstream. Its POSIX surface is inside
AuraLite's libc (Fact 7). GCC is ~10× the size with a configure/bootstrap
mountain; LLVM is out of the question; writing a C compiler is a decade,
not a plan phase.

### D3. The linker is `aulink`, purpose-built

`kernel.ld` + `lib/libc/user.ld` are small, stable, and fully enumerated in
the tree. `aulink` parses the subset they use (ENTRY, PHDRS, section
placement, ALIGN, symbols) and emits ELF64/ELF32. Gate: a host unit test
compares `aulink` output against `ld.lld` output segment-for-segment via
readelf for every link in the tree — parity is the definition of done.
`--gc-sections` is deliberately not required (footprint nicety, not
semantics).

### D4. The assembler is a spike decision: port nasm vs `mini-asm`

Both paths are real: nasm is a self-contained C program that has been built
with small libcs, and the in-tree dialect is a small stable subset (Fact 4)
that a purpose-built assembler (~2–4 kLOC) could cover exactly. The spike
(SH4) measures both against the same gate: **byte-identical objects for
every `.asm` file in the tree**, host-side, before anything runs in-guest.

### D5. The build driver is `shmake`, not GNU make

The 167 KB Makefile is GNU-make syntax with generated-file gymnastics; full
GNU make compatibility is a project of its own and is explicitly NOT this
plan's job. Instead: a self-host build description (`build.sh` + a small
POSIX-subset `shmake`) drives the same target set, and
`tools/check_selfhost_claims.py` asserts the two descriptions name the same
targets — the drift ratchet this tree already applies to plans, applied to
build descriptions.

### D6. The worktree lives on `/fat`, tmpfs is scratch only

1269 source files cannot fit in 64-slot tmpfs (Fact 6); `/fat` is writable,
persistent and full-featured today. `run_qemu.sh` grows the build disk to
256 MiB (a parameter). Reboot persistence is a requirement, not a nicety:
a build that dies at phase 6 of 9 must resume, not restart.

### D7. The host is the judge

Every phase's gate is a host-side test that boots QEMU and greps a serial
receipt. The guest's own claims are never the evidence. This is the rule
that makes the plan's "it works" assertions falsifiable.

### D8. Licensing follows the DOOM precedent

TinyCC (LGPL-2.1) is never vendored. `make selfhost-deps` fetches the
upstream source once into `build/`, the user's own machine compiles it, and
the repository ships only AuraLite's glue (the libc port layer, `aulink`,
`shmake`, `mini-asm`, the build description — all Apache-2.0). The
resulting ISO is a user-local artefact, exactly as with `make doom`.

### D9. No `.patch` artefacts, no patch checks

Phase deliverables are code, tests and docs **in the tree** — nothing
else.  The historical plans shipped one `.patch` per phase and their
checkers asserted the file exists; this plan neither ships patch files
nor asserts them.  The precedent is `check_rinet2_claims.py`, which
removed its `os.path.exists("patches/RINET2_Y*.patch")` claims because a
patch on disk proves a file exists, not that code works: it could be
satisfied by `touch`-ing empty files while every real deliverable was
broken, and it could fail while every deliverable passed.  The gates
that actually prove a phase landed are the ones this plan already names
— unit tests, the host-side integration cases and their greppable
receipts — and `tools/check_selfhost_claims.py` asserts only those.

---

## 3. Phases

Each phase has a **Goal**, a **Definition of done**, a **Gate** (a test that
fails the build/CI if unmet), and a **Deliverable** — the code, tests and
docs landing in the tree (D9: no patch artefact). Phases must land
with the tree green; nothing in this plan may break an existing unit or
integration case.

### Phase SH0 — Rules, receipts, checker ✅ LANDED

**Goal.** The plan, its receipt contract and its drift ratchet exist before
any code changes.

**Definition of done.** `SELFHOST_PLAN.md` at the repo root; the receipt
list in §8; `tools/check_selfhost_claims.py` passes and its `--selftest`
proves it can fail.

**Gate.** `python3 tools/check_selfhost_claims.py` → `OK`; `--selftest` →
`SELFTEST OK`; both printed in `make test-unit` output once wired (SH9).

**Deliverable.** This document + `tools/check_selfhost_claims.py`.
(Companion tree repair, separate from the phase arc: the RTL8139
integration case was committed without a `group_re()` shard
registration, which made `run_all.sh` refuse to start at all; the
registration landed in the `net` shard with this phase. Verified:
`--check-groups` passes.)

### Phase SH1 — Runtime limits + TinyCC userspace port ✅ LANDED

**Goal.** AuraLite can run a real compiler, and TinyCC runs as a user
program.

**What landed (2026-08-26):**
- **Limits raised:** `SPAWN_MAX_IMAGE` 1 MiB → 16 MiB (the cap was
  arbitrary; tcc needs ~10 MiB); `USER_STACK_SIZE` 1 MiB → 4 MiB at all
  four sites (`syscall.c`, `guard.c`, `process.c`, `user.c` — the guard
  classifier and both stack mappers must agree); `TMPFS_MAX_FILES` 64 →
  256. `/bin/sysinfo` prints the new values.
- **Guest TinyCC 0.9.28rc** (mob @ `2ba12e8`, pinned in the build
  receipt): fetched by `make selfhost-deps` (D8: never vendored),
  cross-built with clang against AuraLite's libc, linked by ld.lld with
  `lib/libc/user.ld` into a ~1.3 MiB static ELF. `tccrun.c` is excluded
  (`-run` unsupported; `tools/selfhost/tcc_glue.c` stubs
  `tcc_run`/`tcc_run_free`); `tcctools.c` is #included by tcc.c upstream.
  Build targets: `make selfhost-deps selfhost-tcc`. Staged into the
  initrd as `/bin/tcc` + `/apps/tcc/{include,libtcc1.a}` (CONFIG_TCCDIR).
- **libc additions the port needed:** `<sys/time.h>` (tcc.h includes it;
  `gettimeofday` already existed), `time_extra.c` (gmtime/localtime/
  mktime/asctime/ctime/strftime — `__DATE__`/`__TIME__` need localtime;
  civil-from-days algorithms, no timezone), `ldexpl` (tccpp float
  literals), dlfcn stubs + `RTLD_DEFAULT` (tccelf calls
  `dlsym(RTLD_DEFAULT, …)`; dynamic linking does not exist — honest
  NULLs, not pretending).
- **The bug SH1 exposed, fixed in the same phase (pre-existing O6
  leak):** the size-class cache classified ANY freed block ≥ 4 KiB into
  the 4 KiB class, so every freed `SPAWN_MAX_IMAGE` buffer was parked in
  the cache and never returned to the heap — the 64 MiB kernel heap lost
  one full buffer per spawn and OOM'd after ~4 spawns (silently: the
  kmalloc-failure path in `spawn_thread` was a bare `thread_exit()`).
  `sizeclass_for_payload()` now returns -1 for payloads ≥ 2× the largest
  class (blocks within a class's own range still recycle), the silent
  OOM prints a `[proc] spawn: … OOM` line plus a `[heap]` dump, and
  `tests/unit/test_sizeclass.c` pins both directions (8191 recycles,
  8192/1 MiB/16 MiB fall through). Verified in-guest: 5+ spawns in a
  row, including repeated `run hello` and tcc compiles, all full-frame.
- **In-guest proof (the SH1 gate):** the integration case
  `test_selfhost_tcc.sh` boots the ISO, runs `tcc -v`, compiles
  `/tests/selfhost_hello.c` with `run tcc -nostdlib -o /tmp/h
  /tests/selfhost_hello.c` (tcc's own ELF linker produces the binary,
  entry `0x401688`, 2123 bytes), runs `/tmp/h`, and greps the §8 receipt.
  **4/4 assertions pass.** Registered in `run_all.sh` under a new
  `selfhost` shard (arrived with the phase, ahead of SH9's CI wiring —
  the RTL8139 lesson: an unregistered case breaks the runner).
- **Shell note discovered on the way:** `init.c` reads at most
  `INPUT_MAX` 256 bytes per line, so the original "write hello.c via the
  shell" plan was fragile (long lines split across serial reads). The
  source is staged in the initrd (`/tests/selfhost_hello.c`); the
  compile/run still happens entirely in-guest.

**Gate.** `test_selfhost_tcc.sh` (4/4): `/bin/tcc` staged, `tcc version`
printed, `[selfhost] tcc PASS: 1 binary built and run` printed by the
guest-built binary, `/tmp/h` exists. Skips with a note when
`make selfhost-deps` was not run. The raised limits are visible in
`/bin/sysinfo` (`Exec limit : 16 MiB`, `User stack : 4 MiB`).

**Deliverable.** The changes above, in the tree: limits, libc growth,
the sizeclass fix, the toolchain wiring, the integration case.  No patch
artefact is shipped or asserted (D9).

### Phase SH2 — tcc builds the userland ✅ LANDED

**Goal.** The compiler is not a toy: it rebuilds AuraLite's own userland.

**What landed (2026-08-26):**
- **The whole pipeline runs in-guest, no host tool after boot:**
  `/bin/tcc` assembles `tools/selfhost/tcc_crt0.s` (crt0 stack decode +
  `syscall()` wrapper + `__sigreturn`, GNU as syntax, tcc's own
  assembler), compiles `libc.c` + `malloc.c` + `env.c` +
  `string_extra.c` + `stdlib_extra.c` + `tcc_builtins.c` against the
  staged headers under `/src/libc/include`, and links the apps with
  `/apps/tcc/libtcc1.a`.  The rebuilt `sysinfo` **runs** in Ring 3 and
  prints its full banner (Process ID included); a receipt program
  (`tools/selfhost/userland_ok.c`) prints the §8 line
  `[selfhost] userland rebuild PASS: 2 binaries (sysinfo, editor)`.
  `editor` is rebuilt too (compiles + links; interactive use is left to
  SH6's scripting).
- **Sources are staged into the initrd** under `/src` (libc include tree
  with `../` relative includes flattened to root-relative for tcc, the
  `.c` sources, `tcc_crt0.s`, `tcc_builtins.c`, `sysinfo.c`, `editor.c`,
  `userland_ok.c`), plus a self-contained `/apps/tcc/include/stdint.h`
  (the repo's `stdint.h` is an `#include_next` wrapper; tcc needed a
  real next header).
- **Four real bugs found and fixed along the way** (each recorded in the
  ledger):
  - **malloc misalignment (SH-13):** the 24-byte `block_meta` header
    made every `malloc()` payload 8-aligned; clang-built code in the
    guest tcc does 16-byte `movaps` on malloc'd buffers -> user-mode
    `#GP(0)` while compiling.  Header padded to 32 bytes: payloads are
    now 16-aligned (`max_align_t`), for every program, not just tcc.
  - **Shell argv truncation (SH-14):** `init.c` capped `MAX_ARGS` at 8
    and `INPUT_MAX` at 256, so a tcc link line naming 9+ objects was
    silently truncated and the linker reported
    `unresolved reference to '__libc_start_main'` for a libc.o it never
    saw.  Raised to 32/512.
  - **tcc ignores `__attribute__((naked))` (SH-15):** a C+inline-asm
    `_start` gets a prologue (`push rbp; mov rsp,rbp; sub $0,rsp`)
    inserted first, so the stack decode read the saved rbp as argc and
    the program page-faulted on garbage argv/envp.  The crt0 is
    therefore a `.s` file — tcc assembles it with no prologue by
    construction.
  - **stdint.h missing in the guest (SH-16):** tcc ships no `<stdint.h>`
    in CONFIG_TCCDIR; the staged `tools/selfhost/stdint.h` provides the
    full freestanding set.
  - **initrd file table (SH-17):** `INITRD_MAX_FILES` was 192; the
    staged `/src` tree blew it and the kernel silently dropped
    `/tests/selfhost_hello.c` (a `[initrd] WARNING: file table full`
    line that broke the SH1 gate).  Raised to 512.
- **Host-side harness:** the same link was validated with the host tcc
  first (7 objects, 74 KB ELF, entry 0x405410), so guest iterations
  debugged only guest-specific issues.

**Gate.** `test_selfhost_userland.sh` (**4/4**, ~5 min in QEMU/TCG):
rebuilds crt0 + libc + apps in-guest, runs the rebuilt `sysinfo`
(greps `System Information`, `Process ID`) and the receipt program
(greps `userland rebuild PASS`), and checks `/tmp/sysinfo` exists.
Registered in the `selfhost` shard (137 cases).  Skips with a note when
`make selfhost-deps` was not run.

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

### Phase SH3 — `aulink`: the self-host linker ✅ LANDED

Goal. Link without ld.lld, in-guest.

What landed (2026-08-26).

* `tools/aulink/aulink.c` — 1500-line portable C99 ELF64 linker (tcc-compatible,
  no host-only headers):
  script subset (ENTRY, PHDRS FLAGS with <<, SECTIONS with ALIGN/CONSTANT,
  KEEP/SORT_BY_INIT_PRIORITY wrappers, COMMON, /DISCARD/), RELA x86_64
  (64/32/32S/PC32/PLT32/16/8 + GOTPCREL with a .got synthesized before .bss),
  .a archive support (all ELF members), and the two freed-buf leak fixes:
  `read_object()` and `load_archive()` both kept pointers into a `free(buf)`'d
  image — gcc's allocator happened not to reuse the block, our libc did, so
  `.rela` names went empty (`rela-name=''`) and archive relocations vanished
  (12 missed from libtcc1.a).  Fix: keep the images alive.  Layout bug
  `.data` at file 0xd000 while `p_offset + (vaddr - p_vaddr)` said 0xcaa0 is
  fixed: file_off(sec) = seg_p_off + (addr - seg_vaddr).  Input sections
  inside an output section are now ALIGN'd by sh_addralign and placed at
  out_off, so `movaps` in .bss (16-byte) no longer faults (#GP on 0x4000ccec).
  Section-name fix for STT_SECTION symbols (st_name=0) and SHN_ABS handling
  for FILE symbols (0xfff1) unblock symbol resolution (45 undefined refs).
* `tests/unit/test_aulink.sh` — host parity vs ld.lld (entry, PT_LOAD flags
  order, section Name+Type presence for the common subset, _start address) —
  `ALL TESTS PASSED`.  Warnings about local symbols >= sh_info are harmless
  (all symbols currently written as GLOBAL; sh_info=1).
* `tests/integration/cases/test_selfhost_aulink.sh` — in-guest: tcc compiles
  aulink.c (staged as /src/aulink.c) against the guest-built libc
  (crt0.s + libc.c + tcc_builtins.c + malloc/env/string_extra/stdlib_extra/
  stdio_extra.c), tcc links aulink with libtcc1.a, aulink links the SH2
  objects with the real user.ld, the kernel runs the aulink-linked sysinfo —
  3/3.  Gate: `stat /tmp/sysinfo-au` + banner greps.

Bugs found along the way (SH-18..SH-20, all closed by SH3):
  SH-18: read_object() free(buf) leak — fixed (keep alive).
  SH-19: load_archive() free(buf) leak — same class, 7 ELF members, shstrtab
         empty, no archive relocations applied, 12 missed.
  SH-20: aulink file layout — file_off vs vaddr mismatch, .data at 0xd000
         while p_offset said 0xcaa0 → kernel loaded zeros, PF on 0x400073fc
         (RIP 0x400073fc, CR2 0x40014212 — NX fault on .rodata).

**Gate.** `make test-unit` runs `test_aulink.sh` (parity) and
`test_selfhost_aulink.sh` boots QEMU and greps the banner.

### Phase SH4 — an assembler that runs in-guest (umbrella) 🚧 PENDING

**Goal.** Assemble `boot/` and `kernel/` `.asm` without nasm.

**Why this is split.** The original SH4 was written as one phase, but its
own definition of done — *byte-identical parity for every `.asm` in the
tree* — is a multi-thousand-line x86 assembler, not a step.  A measured
survey of the tree (2026-08-27) fixes the real surface:

- **29 `.asm` files**, in **four** output formats, not one:
  - `-f bin` (flat, no relocations, no symbol table — pure bytes at `org`):
    **4 files** — `boot/bios/stage1/mbr.asm` (162),
    `boot/bios/stage1/mbr_dual.asm` (104),
    `boot/bios/stage2/stage2_start.asm` (548, `%include`s 13 `.inc`, uses
    `%if/%else`), `boot/smp/ap_trampoline.asm` (120, generated).
  - `-f elf64`: **13 files** — `kernel/arch/x86_64/*` (5), `kernel/proc/*`
    (4), `lib/libc/crt/*` (3) + `lib/libc/src/syscall.asm`.
  - `-f elf32`: **7 files** — `kernel/arch/i386/*` (5), `lib/libc32/*` (2).
  - `-f win64` (COFF, `ms_abi`): **5 files** — `w32/tests/*`.  These are
    Win32-personality *test fixtures*, not inputs to building the OS; they
    are scoped OUT of the self-host closure (ledger SH-25).
- **Preprocessor surface** (shared by every file): `%include`, `%define`,
  `%assign`, `%macro`/`%endmacro` (one-arg), `%rep`/`%endrep` (incl. `%rep 256`),
  `%if/%else/%endif`, `%error`.
- **Encoder surface**: ~80 distinct mnemonics including the awkward ones
  (`o64` prefix, `ldmxcsr`, `fxsave`/`fxrstor`, `wrmsr`/`rdmsr`, `rep`
  prefixes), across `bits 16/32/64`.

The four formats have very different byte-parity costs.  `-f bin` has no
relocations and no symbol table, so its output is pure encoder+preprocessor
bytes — the strictest bar (byte-for-byte) is also the most achievable there.
The ELF formats need a header/section/symtab/relocation emitter whose byte
layout nasm does not guarantee, so the plan's own bar for them is *readelf
parity*, not byte parity.  Splitting along that gradient makes each step
land a measured, falsifiable increment instead of one big-bang claim.

**Definition of done.** The union of SH4a–SH4d.  D4 (port nasm vs write
`mini-asm`) is resolved by measurement in SH4a, not asserted.

**Gate.** The union of the SH4a–SH4d gates; the terminal one is SH4d's
in-guest `test_selfhost_asm.sh`.

**Deliverable.** The changes below, in the tree (D9: no patch artefact).

---

### Phase SH4a — spike (D4 decision) + mini-asm core + first byte-identical flat object 🚧 PENDING

**Goal.** Resolve D4 with numbers, and stand up `tools/mini-asm/mini-asm.c`
far enough to assemble ONE real flat file byte-for-byte identically to nasm.

**Scope.** The spike measures both D4 paths concretely: (a) *port nasm* —
upstream size and its libc footprint (nasm needs a real libc; the guest libc
is AuraLite's own), recorded as a cost; (b) *mini-asm* — the in-tree dialect
surface measured above, recorded as a cost.  The decision is written into
this section with the numbers (OPT O0 discipline: the measurement is part of
the deliverable).  Then the mini-asm core lands: tokenizer, expression
evaluator, label resolver (two-pass), the preprocessor subset
(`%include`/`%define`/`%assign`), the x86 encoder for the mnemonics the
target file actually uses, and the `-f bin` emitter (`org`, section
concatenation, `times`/`align`/`resb`, `db`/`dw`/`dd`/`dq`).

**Definition of done.** `mini-asm -f bin` reproduces nasm's output
**byte-for-byte** on the simplest real flat file
(`boot/bios/stage1/mbr_dual.asm`, 104 lines).  The spike's numbers (both
paths) are written here.

**Gate.** Host parity harness (a `tests/unit/test_asm_parity.sh` or
`tools/check_mini_asm.py`) byte-compares mini-asm vs nasm on the covered
file(s) and is wired into `make test-unit`; it fails the build on any byte
difference.  Receipt: `[selfhost] asm PASS (bin): 1/4 flat objects
byte-identical`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4b — `-f bin` byte-parity across all four boot files 🚧 PENDING

**Goal.** Extend the encoder and preprocessor until every `-f bin` file in
the tree is byte-identical to nasm.

**Scope.** Grows SH4a's core to cover: the full `%macro`/`%endmacro`
(one-arg) and `%rep`/`%endrep` expansion (`isr_stubs*` patterns), the
`%if/%else/%endif` conditional assembly and the 13-file `%include` chain in
`stage2_start.asm`, `%error`, `alignb`, and the remaining 16-bit real-mode
encoder cases the boot code needs (BIOS INT calls, `o64`/operand-size
prefixes, `seg`/`org` fixes).  Targets: `mbr.asm`, `mbr_dual.asm`,
`stage2_start.asm` (+ its 13 includes), `ap_trampoline.asm`.

**Definition of done.** mini-asm `-f bin` output is **byte-for-byte**
identical to nasm on all four flat files.

**Gate.** The SH4a host parity harness now byte-compares all four `-f bin`
files and is green in `make test-unit`.  Receipt:
`[selfhost] asm PASS (bin): 4/4 flat objects byte-identical`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4c — ELF64 backend, readelf parity on the kernel/libc objects 🚧 PENDING

**Goal.** Add the `elf64` emitter so the kernel/libc `-f elf64` objects
reach structural parity with nasm.

**Scope.** ELF64 header, program/section headers, `.symtab`/`.strtab`/
`.shstrtab`, and the `R_X86_64_*` relocations the tree emits
(`64`/`PC32`/`32S`/`PLT32`/`GOTPCREL`), plus `global`/`extern`/`section`
attribute handling and `align`/`alignb` in an ELF context.  Per the SH4
definition of done, ELF output is compared **via readelf**, not
byte-for-byte (nasm does not guarantee ELF byte layout).  Targets: the 13
`-f elf64` files, boot-critical ones first (`isr_stubs.asm`,
`syscall_entry.asm`, `boot.asm`).

**Definition of done.** For every `-f elf64` file, `readelf -SW`/`-sW`/`-rW`
of the mini-asm object matches nasm's on section names/types/flags, symbol
names/bindings/section indices, and relocation types/addends.

**Gate.** The host parity harness gains an elf64 readelf-comparison mode,
green in `make test-unit` on the 13 files.  Receipt:
`[selfhost] asm PASS (elf64): <n> objects readelf-parity`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4d — ELF32 backend + the in-guest assembly run 🚧 PENDING

**Goal.** Finish the format coverage (elf32) and make the assembler actually
run *in-guest* — the phase's namesake.

**Scope.** The `elf32` emitter for the 7 `-f elf32` files
(`kernel/arch/i386/*`, `lib/libc32/*`), readelf-parity as in SH4c.  Then
mini-asm is built by the guest tcc (SH2) and run inside AuraLite to assemble
the boot-critical sources, proving the assembler self-hosts, not just that it
matches nasm on the host.

**Definition of done.** elf32 readelf parity on the 7 files; AND an in-guest
mini-asm assembles the boot-critical files with the recorded receipt.

**Gate.** Integration case `tests/integration/cases/test_selfhost_asm.sh`
(registered in the `selfhost` shard) boots QEMU and greps
`[selfhost] asm PASS: <n> objects byte-identical` for the in-guest assembly
of the boot-critical files (isr_stubs, syscall_entry, boot).

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH5 — the kernel, built by itself 🚧 PENDING

**Goal.** The x86_64 kernel links and boots when tcc + aulink built it.

**Definition of done.** SH5 spike first: does tcc's RIP-relative codegen
link at `0xFFFFFFFF80100000` with `aulink`? If yes: `kernel.elf` built
entirely in-guest (tcc + aulink, no clang/ld.lld), with the clang-only
flags (Fact 2) either ported (tcc equivalents), dropped with a measured
footprint delta recorded, or replaced by aulink-side layout guarantees. If
the spike says no, the honest fallback is recorded in the ledger: the
kernel keeps its host compiler while userland self-hosts — a documented
partial, not a silent one.

**Gate.** Integration case boots the guest-built `kernel.elf` (host QEMU,
`-kernel` path or ISO from SH7) and greps the standard boot receipts
through `[perf] boot-to-shell` — the same assertions `test_boot_to_shell`
uses, applied to the self-built kernel. Receipt:
`[selfhost] kernel PASS: tcc-built kernel booted to shell`.

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

### Phase SH6 — shmake + shell scripting 🚧 PENDING

**Goal.** Build scripts run in-guest.

**Definition of done.** `shmake` (POSIX-subset make, C, in-guest): rules,
prerequisites, variables, phony targets — the subset `build.sh` needs.
smallsh (or its promoted successor) gains pipes, redirects, environment
variables and `for`/`if` — the subset `build.sh` needs. `build.sh` is the
single entry point that drives kernel + initrd on `/fat` (targets
mirrored with the host Makefile per D5).

**Gate.** Integration case `test_selfhost_build.sh` runs `sh build.sh
kernel` in-guest and greps `[selfhost] build PASS: kernel+initrd built on
/fat`; a second boot resumes from the same `/fat` tree (persistence proof,
D6).

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

### Phase SH7 — image tooling in C 🚧 PENDING

**Goal.** The ISO is assembled in-guest.

**Definition of done.** C twins for the host-only pieces (Fact 5):
`mkinitrd` (USTAR writer — the format the kernel already parses),
`mkiso` (MBR + GPT + FAT32 ESP writer replacing mformat/mcopy/python;
the BPB patch python3 does inline becomes a `mkiso` flag), `sha256sum`
(reuse the SHA-256 already in `libatls` — one implementation, tested
once), plus `gen_boot_offsets` twin (the existing one is already portable
C). `sh build.sh iso` produces `auralite.iso` on `/fat`.

**Gate.** Host boots the guest-built ISO (test `test_selfhost_iso.sh`)
and greps the standard boot receipts to the shell. This is the first
end-to-end proof of Stage 1; the receipt is
`[selfhost] iso PASS: auralite.iso built in-guest` on the guest side and
the normal boot receipts on the host side.

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

### Phase SH8 — bootstrap closure 🚧 PENDING

**Goal.** No host toolchain anywhere in the loop, twice in a row.

**Definition of done.** Stage 2 (D1): tcc rebuilt by tcc on AuraLite
(chain tcc₀ → tcc₁ → tcc₂, each stage's binary hash recorded in the
receipt), then aulink/shmake/mini-asm rebuilt the same way; from a clean
`/fat` worktree the full `sh build.sh iso` loop runs twice consecutively
without any host tool being invoked. The `__DATE__`/`__TIME__` in the
kernel banner means the two ISOs are not byte-identical — the gate is
**functional reproducibility** (both boot to the shell with identical
receipt sets), stated honestly rather than pretending byte-parity.

**Gate.** `test_selfhost_closure.sh` (long-running, slow-shard):
`[selfhost] FULL LOOP PASS (2/2 clean loops)`.

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

### Phase SH9 — cross-arch + CI wiring 🚧 PENDING

**Goal.** The self-host loop covers the other three kernels and stops
drifting.

**Definition of done.** Spike: can the in-guest x86_64 tcc emit
i386/riscv64/aarch64 code (upstream tcc has the codegens; whether the
single self-hosted binary carries them is measured, not assumed)? Where
yes: the four-kernel build closes. Where no: per-arch toolchain binaries
built on AuraLite by the same closure chain. Regardless of the spike:
`tools/check_selfhost_claims.py` is wired into `make test-unit`
(`UNIT_TESTS`), the self-host integration cases are registered in
`tests/integration/run_all.sh` (a new `selfhost` shard — the
`FIX_RTL8139_SHARD` incident is the standing precedent for why
unregistered cases are unacceptable), and the slow closure case goes to
the CI shard runner.

**Gate.** `make test-unit` runs the checker; `--check-groups` passes with
the new shard; the four-kernel in-guest build produces kernels that boot
(the a64/rv/i386 boot receipts from `docs/status.md`).

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

---

## 4. Order and rationale

The dependency chain is strict:

```
SH1 limits+tcc ─► SH2 userland ─► SH3 aulink ─► SH4 asm ─► SH5 kernel
                                                              │
SH6 shmake+shell ◄────────────────────────────────────────────┘
SH7 mkimg  ─►  SH8 closure  ─►  SH9 cross+CI
```

The compiler comes first because it is the keystone: every other tool
(aulink, shmake, mini-asm, mkimg) is C that tcc must compile. The linker
precedes the assembler because userland can link with aulink while nasm
still assembles (SH2/SH3 land with host-nasm objects); the assembler is
needed before the kernel can close (SH4 → SH5). The kernel is the hardest
link in the chain (Fact 2) and is deliberately placed after the userland
is proven, so a kernel-codegen failure degrades to "userland self-hosts,
kernel documented as host-compiled" instead of blocking everything. The
shell/make phase can be deferred to after the kernel because a C build
driver could drive SH5; scripting is needed only when `build.sh` becomes
the orchestrator (SH6). Image tooling closes Stage 1 (SH7), closure proves
Stage 2 (SH8), and cross-arch + CI prevent rot (SH9).

## 5. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | tcc x86_64 codegen not linkable at the higher-half address (Fact 2) | SH5 spike first; fallback is a documented partial (host-compiled kernel), not a silent divergence |
| R2 | libc gaps that only real compiler workloads expose (unusual stdio modes, stat fields, signal edge cases) | host-side parity build first (tcc against our libc source, run on the host) so gaps surface as host tests, not in-guest surprises |
| R3 | Raising `SPAWN_MAX_IMAGE` widens the exec surface | deliberate and commented; the cap was arbitrary (Fact 6); the raise is bounded (16 MiB), not unbounded |
| R4 | 16 MiB `/fat` disk too small for a source tree + build tree | D6: 256 MiB build disk is a QEMU parameter; FAT32 limits (4 GiB files) are irrelevant at this scale |
| R5 | Licensing (TinyCC is LGPL-2.1) | D8: DOOM precedent — never vendored; fetched and built on the user's machine; repo ships glue only |
| R6 | Scope: this is a multi-phase arc, not a weekend | each phase lands green and independently shippable; the ledger (§7) tracks the open rows so an interrupted arc is visible, not lost |
| R7 | The guest-built ISO must not be trusted (it was built by the thing under test) | D7: every receipt is asserted host-side; the final gate boots the guest ISO on the host |
| R8 | Drift between the host Makefile and `build.sh` | D5 + checker: `check_selfhost_claims.py` asserts both name the same target set |

## 6. What this plan does not do

- **No GCC, binutils or LLVM ports.** TinyCC is the compiler; `aulink` is
  the linker; nothing LLVM-scale is contemplated.
- **No GNU make compatibility.** The 167 KB Makefile stays host-side;
  `build.sh`/`shmake` are its in-guest sibling, kept in sync by the
  checker, not by reimplementing GNU make.
- **No self-hosted Rust.** `rsbr`/`rustes` remain host-built until a
  self-hosted rustc exists, which is a different project (ledger SH-08).
- **No POSIX shell completion.** smallsh gains the scripting subset
  `build.sh` needs, not a bash.
- **No GUI build environment, no networked package manager for the
  toolchain.** Sources arrive on the build disk or via one `make
  selfhost-deps` fetch (D8), exactly like doom.
- **No byte-identical reproducibility.** `__DATE__`/`__TIME__` make that a
  fiction; the gate is functional reproducibility (identical receipt sets
  across two clean loops), stated honestly (SH8).

## 7. Ledger

The machine-checked rows of the self-hosting debt. `check_selfhost_claims.py`
asserts each row has four fields and that ACCEPTED rows cite a decision.

| Row | Debt | Class | Status | Phase |
|---|---|---|---|---|
| SH-01 | exec-image cap 1 MiB (`SPAWN_MAX_IMAGE`) blocks a real compiler | limit | CLOSED (16 MiB, SH1) | SH1 |
| SH-02 | user stack 1 MiB (4 sites) too shallow for compiler recursion | limit | CLOSED (4 MiB, SH1) | SH1 |
| SH-03 | `TMPFS_MAX_FILES` 64 per volume; source tree is 1269 files | limit | CLOSED (256, SH1) | SH1 |
| SH-04 | smallsh: no pipes/redirects/variables/loops | missing | OPEN | SH6 |
| SH-05 | ISO tooling is host python3/mtools (Fact 5) | missing | OPEN | SH7 |
| SH-06 | kernel CFLAGS clang-only, `-mcmodel=kernel` unportable (Fact 2) | port | OPEN | SH5 |
| SH-07 | `OPEN_MAX` 64 — adequacy unknown until the spike | limit | CLOSED (tcc ran on 64 fds, SH1) | SH1 |
| SH-08 | rustc/rsbr not self-hostable in this plan | accepted | ACCEPTED (D1 scope) | — |
| SH-09 | TinyCC LGPL-2.1 vs Apache-2.0 tree | licensing | ACCEPTED (D8, DOOM precedent) | — |
| SH-10 | integration runner: new cases must be shard-registered or the runner refuses to start (the RTL8139 incident) | process | OPEN (selfhost shard arrived in SH1; full CI wiring in SH9) | SH9 |
| SH-11 | boot `__DATE__`/`__TIME__` makes byte-reproducibility impossible | accepted | ACCEPTED (SH8 definition) | SH8 |
| SH-12 | O6 sizeclass cache parks any ≥4 KiB freed block in the 4 KiB class — heap loses one SPAWN_MAX_IMAGE buffer per spawn (pre-existing) | leak | CLOSED (SH1: payload ≥ 2× largest class falls through to heap_free; test_sizeclass pins it) | SH1 |
| SH-13 | malloc payloads 8-aligned (24-byte header): clang code in the guest tcc faults on 16-byte movaps (`#GP(0)` compiling libc.c) | align | CLOSED (SH2: 32-byte header, 16-aligned payloads) | SH2 |
| SH-14 | shell caps argv at 8 / line at 256: tcc link lines silently truncated -> `unresolved reference to '__libc_start_main'` | limit | CLOSED (SH2: MAX_ARGS 32, INPUT_MAX 512) | SH2 |
| SH-15 | tcc ignores `__attribute__((naked))`; C inline-asm `_start` gets a prologue and decodes the stack wrong | port | CLOSED (SH2: crt0 is a .s file, no prologue) | SH2 |
| SH-16 | guest tcc ships no `<stdint.h>` (repo's is an `#include_next` wrapper) | missing | CLOSED (SH2: self-contained stdint.h staged in /apps/tcc/include) | SH2 |
| SH-17 | initrd file table caps at 192 entries; the /src tree blew it and silently dropped /tests/selfhost_hello.c | limit | CLOSED (SH2: INITRD_MAX_FILES 512) | SH2 |
| SH-18 | read_object() free(buf) leak: members point into freed archive/object image → shstrtab empty, 45 undefined refs | leak | CLOSED (SH3: keep images alive) | SH3 |
| SH-19 | load_archive() free(buf) leak: 7 ELF members, shstrtab/strtab freed, no archive relocations, 12 missed | leak | CLOSED (SH3: keep archive image alive) | SH3 |
| SH-20 | aulink file layout: file_off vs vaddr mismatch, .data at 0xd000 while p_offset said 0xcaa0 → kernel loaded zeros | layout | CLOSED (SH3: file_off = seg_p_off + (addr - seg_vaddr)) | SH3 |
| SH-21 | aulink input sections not ALIGN'd inside output section → movaps in .bss faults | align | CLOSED (SH3: ALIGN filled by sh_addralign, out_off) | SH3 |
| SH-22 | aulink forgot sec->out_off in P and memcpy → PC32 relocs off by section's position | reloc | CLOSED (SH3: base = out.addr + out_off) | SH3 |
| SH-23 | STT_SECTION symbols have st_name=0 → name empty, GOTPCREL unresolved | sym | CLOSED (SH3: name = sec_name for STT_SECTION) | SH3 |
| SH-24 | SH4 "byte-identical assembler for all 29 .asm" is a multi-kLOC x86 assembler across FOUR output formats (bin/elf64/elf32/win64) — too large for one falsifiable step | scope | OPEN (split into SH4a–SH4d along the format gradient, 2026-08-27) | SH4a |
| SH-25 | -f win64 (COFF, ms_abi) for w32/tests/*.asm (5 files) is a fifth format | scope | ACCEPTED (out of the self-host closure — Win32 test fixtures, not inputs to building the OS; D1 scope) | — |

## 8. Receipt strings (the greppable contract)

Host integration cases assert exactly these lines; the checker asserts the
plan still lists them, so a renamed receipt fails the build:

```
[selfhost] tcc PASS: <n> binaries built and run
[selfhost] userland rebuild PASS: <n> binaries
[selfhost] aulink PASS: <n> ELF linked, layout parity OK
[selfhost] asm PASS (bin): <n>/4 flat objects byte-identical
[selfhost] asm PASS (elf64): <n> objects readelf-parity
[selfhost] asm PASS: <n> objects byte-identical
[selfhost] kernel PASS: tcc-built kernel booted to shell
[selfhost] build PASS: kernel+initrd built on /fat
[selfhost] iso PASS: auralite.iso built in-guest
[selfhost] FULL LOOP PASS (2/2 clean loops)
```

## Appendix A — syncing the docs after each phase

Same rule as `MATURITY_PLAN.md` Appendix: every phase updates, in the same
commit as its patch: `docs/status.md` (the affected rows move ✅/🧪),
`docs/driver_guide.md` (new tools), this plan's status table and ledger,
and `CHANGELOG.md` (the repo's dated, per-series changelog style). The
checker enforces the status table; the rest is the commit discipline the
tree already follows.

## Appendix B — checker wiring

`tools/check_selfhost_claims.py` (SH0 deliverable) already runs standalone:
`python3 tools/check_selfhost_claims.py` → `OK`, `--selftest` proves the
negative control. SH9 wires it into `make test-unit` (the `UNIT_TESTS`
variable), alongside the other `check_*_claims.py` tools, and registers
the self-host integration cases in `tests/integration/run_all.sh` under a
new `selfhost` shard.
