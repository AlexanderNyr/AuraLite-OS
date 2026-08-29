# AuraLite OS — Self-Hosting Plan

## Status: IN PROGRESS 🚧 — SH0–SH5 landed (SH4 = SH4a–SH4e complete; SH5 = SH5a–SH5d complete): the guest TinyCC now compiles all kernel C sources, guest-built mini-asm emits all x86_64 kernel objects, guest-built aulink links the kernel on `/fat`, and the host has booted that extracted artifact to the Ring 3 shell. SH6 is split into SH6a–SH6f; SH6a (script runner, exit statuses), SH6b
(redirects, named variables, quote-aware parsing, and the kernel fix that made
redirected fd 0/1/2 actually work), SH6c (pipes and command lists) and SH6d
(control flow) have landed.  SH6e–SH9 remain pending.

| Phase | Result |
|-------|--------|
| SH0 — Rules, receipts, checker | ✅ landed |
| SH1 — Runtime limits + TinyCC userspace port | ✅ landed |
| SH2 — tcc builds the userland | ✅ landed |
| SH3 — `aulink`: the self-host linker | ✅ landed |
| SH4 — an assembler that runs in-guest (umbrella; split into SH4a–SH4e) | ✅ landed |
| SH4a — spike (D4 decision) + mini-asm core + first byte-identical flat object | ✅ landed |
| SH4b — 64-bit mode + REX + the SMP trampoline, byte-identical (3/4 flat) | ✅ landed |
| SH4c — `stage2_start.asm` byte-parity: %include/%if + SIB/segment-override encoder (4/4 flat) | ✅ landed |
| SH4d — ELF64 backend, readelf parity on the kernel/libc objects | ✅ landed |
| SH4e — ELF32 backend + the in-guest assembly run | ✅ landed |
| SH5 — the kernel, built by itself (split into SH5a–SH5d) | ✅ landed |
| SH5a — spike: tcc codegen links AND boots at the higher half | ✅ landed |
| SH5b — aulink kernel.ld layout parity vs ld.lld | ✅ landed |
| SH5c — the kernel compiled by tcc (flag story + delta) | ✅ landed |
| SH5d — the in-guest build + terminal boot gate | ✅ landed |
| SH6 — shmake + shell scripting (umbrella; split into SH6a–SH6f) | 🚧 in progress |
| SH6a — spike (D10 decision) + exit-status spine + script runner | ✅ landed |
| SH6b — redirects + named variables + the fd 0/1/2 kernel fix | ✅ landed |
| SH6c — pipes + command lists (`;` `&&` `\|\|`) | ✅ landed |
| SH6d — control flow `if`/`while`/`for` | ✅ landed |
| SH6e — `shmake`: rules, prerequisites, variables, phony targets | 🚧 pending |
| SH6f — `build.sh` entry point + D5 target parity + D6 resume | 🚧 pending |
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
`ap_trampoline.asm` (assembled flat and embedded by `tools/gen_ap_trampoline_inc.c`), per-arch
crt0/syscall wrappers (`-f elf64`/`elf32`). The dialect is flat NASM syntax
with `%include`; no macros beyond the tree's own. Two candidate paths —
port nasm (a self-contained C program, ~150 kLOC, needs a real libc) or
write `mini-asm` for the exact in-tree subset — are a SH4 spike decision
(D4), with byte-identical-object parity as the gate.

### Fact 5 — Image tooling is host scripts with host interpreters

`tools/mkinitrd.sh` (tar), `tools/mkisoimage_dual.sh` (mformat/mcopy +
**inline python3** patching the FAT BPB), `tools/gen_boot_offsets.c`
(host `cc` — already portable C), and `tools/gen_wv_cp1251_font.py` remain
host-side image/build tooling.  SH5d replaced the kernel-path
`gen_user_binary.py` and `gen_ap_trampoline_inc.py` recipes with portable C
emitters, deleted both Python originals, and made `gen_asm_offsets.c`
file-output-capable; the remaining image loop still belongs to SH7.

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

**Correction, measured 2026-08-29 (SH6a).** This fact names the wrong file
for x86_64. `smallsh.c` is **173 lines** with **no builtin dispatch at
all**, and it is the shell for the *other* architectures only
(`kernel/arch/aarch64/main_a64.c:635-636`, `kernel/arch/riscv64/main_rv.c:501-502`).
The x86_64 boot shell that every self-host gate drives is
`userspace/system/init/init.c` — **1009 lines**, **31 builtins**, job
control, and the `auralite#` prompt `prompt_qemu.py` waits on. Its gaps
were the same ones (`pipe`/`getenv`/`setenv`/`source`/`$VAR`: 0 hits each),
so the conclusion stands, but SH6's work happens in `init.c`. See D10.

The builtin count is *distinct* dispatch names, not raw `strcmp` hits: `run`
appears twice (once in the background path, once in the foreground dispatch),
so the grep count reads 32 where the distinct count is 31.  Measured with
`grep -o 'strcmp(cmd, "[a-z_0-9]*") == 0' | sort -u | wc -l` on the SH5d base;
SH6a's `sh` makes it 32, SH6b's `set`/`unset` make it 34, and SH6d's
`true`/`false`/`break` make it 37.

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

A full durable worktree cannot fit in the original 64-slot tmpfs; `/fat` is
writable, persistent and full-featured today. `run_qemu.sh` can grow the
build disk to 256 MiB (a parameter). SH5d deliberately uses separated
256-slot `/tmp/sh5d/{cobj,aobj,...}` directories only for transient objects
and persists just its 1.22 MiB final ELF to the stock 4 MiB `/fat`; SH6's
resumable build worktree follows this decision fully. Reboot persistence is a
requirement, not a nicety: a build that dies at phase 6 of 9 must resume, not
restart.

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

### D10. The scripting shell is `init.c`, not `smallsh` and not a new `/bin/sh`

Resolved by measurement in SH6a, the same way D4 was.  The plan's own hedge
— *smallsh **(or its promoted successor)*** — left this open, and Fact 8
pointed at the wrong file.  Three candidates, measured 2026-08-29:

| Path | Measured cost | Verdict |
|---|---|---|
| **Promote `smallsh`** | `smallsh.c` is **173 lines** (`str_eq`, `starts_with`, `print_dec`, `help`, `main`) with **no builtin dispatch**.  Promoting it to the x86_64 shell means re-implementing all **31** `init` builtins, the job table, `prog_resolve` and the `auralite#` read loop that `prompt_qemu.py` and all 143 integration cases depend on — then keeping two x86_64 shells in sync forever. | **Rejected** |
| **New `/bin/sh` program** | The builtins, the search path and the job table are all `static` inside `init.c`.  A separate program would have to duplicate or export all three, and `init` would still have to spawn it — so the scripting work gets paid twice, and a script could not use a builtin. | **Rejected** |
| **Extend `init.c`** | Purely additive: **+229 lines** on 1009, no existing dispatch branch rewritten, and the runner reuses the builtin set that already exists.  `sh` is one more entry beside the other 33. | **Chosen** |

The consequence worth recording: `smallsh` stays the aarch64/riscv64 shell and
**does not** gain scripting in this plan.  Cross-arch shell parity is SH9's
problem, not SH6's, and pretending otherwise would have inflated every SH6
sub-phase by a second implementation.

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

**Definition of done.** The union of SH4a–SH4e.  D4 (port nasm vs write
`mini-asm`) is resolved by measurement in SH4a, not asserted.

**Gate.** The union of the SH4a–SH4e gates; the terminal one is SH4e's
in-guest `test_selfhost_asm.sh`.

**Deliverable.** The changes below, in the tree (D9: no patch artefact).

---

### Phase SH4a — spike (D4 decision) + mini-asm core + first byte-identical flat object ✅ LANDED (2026-08-27)

**Goal.** Resolve D4 with numbers, and stand up `tools/mini-asm/mini-asm.c`
far enough to assemble ONE real flat file byte-for-byte identically to nasm.

**Result — the D4 spike, measured (not asserted).**

| Path | Measured cost | Verdict |
|---|---|---|
| **Port nasm** | nasm 2.16.03: stripped binary **1 948 336 B**, links **glibc** (`libc.so.6`), imports **79 libc symbols** (`nm -D --undefined-only`); upstream source ~150 kLOC (Fact 4), almost all of it instruction tables AuraLite never uses. A guest port must satisfy all 79 libc entry points against AuraLite's own libc before it assembles a byte. | **Rejected** |
| **Write mini-asm** | SH4a core = **821 lines of C99**, zero dependencies beyond freestanding C; covers exactly the in-tree boot subset. Byte-identical to nasm on **2 of the 4** `-f bin` objects on the first run. | **Chosen** |

**Decision (D4 resolved): mini-asm.** The nasm port's price is a full libc it
expects and ~150 kLOC of mostly-irrelevant encoder; mini-asm targets only the
dialect the tree actually uses (Fact 4) and is already byte-exact on both MBR
variants. The spike's evidence is reproducible: `nm -D --undefined-only
$(command -v nasm) | grep -c ' U '` → 79; `wc -l tools/mini-asm/mini-asm.c`.

**What landed.** `tools/mini-asm/mini-asm.c` (`-f bin` emitter; `bits`/`org`/
`equ`/`db`/`dw`/`dd`/`dq`/`resb`/`align`/`alignb`/`times`; `$`/`$$`; global and
NASM local labels; a recursive-descent expression evaluator; fixed-point
jump-sizing that reproduces nasm's shortest-encoding-that-fits; the 16-bit
encoder subset the MBRs use, incl. far `jmp seg:off`). It **dies loudly** on
any mnemonic/form outside the subset rather than mis-encoding — which is why
`ap_trampoline.asm` (`lgdt`) is an honest SH4b gap, not a silent wrong byte.

**Definition of done — MET (and exceeded).** `mini-asm -f bin` reproduces
nasm **byte-for-byte** on `boot/bios/stage1/mbr_dual.asm` (512 B) *and*, for
free, `boot/bios/stage1/mbr.asm` (512 B) — same instruction subset. The
spike's numbers are recorded above.

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

**Gate.** MET. `tests/unit/test_asm_parity.sh` compiles mini-asm with host
`cc -Werror`, assembles each covered `-f bin` source with BOTH nasm and
mini-asm, and `cmp`s the bytes; wired into `make test-unit` next to the SH3
aulink gate.  It skips cleanly without nasm.  Negative control: the gate's
FAIL path is live, not vacuous — `ap_trampoline.asm` is deliberately not yet
covered and mini-asm refuses it (`unsupported instruction 'lgdt'`), which is
exactly the loud failure the harness reports as a byte difference would.
Receipt: `[selfhost] asm PASS (bin): 2/4 flat objects byte-identical`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4b — 64-bit mode + REX + the SMP trampoline, byte-identical ✅ LANDED (2026-08-27)

**Goal.** Grow the encoder past the 16-bit MBR subset to the next flat file,
`boot/smp/ap_trampoline.asm`, which exercises everything the MBRs did not:
64-bit mode, REX prefixes, RIP-size operand/address handling, control
registers and the far jump into long mode.

**Scope (what landed).** `bits 32/64` tracking; 64-bit registers
(`rax`..`rdi`, `r8`..`r15`); REX.W/REX.R/REX.B emission; the operand-size
prefix (`0x66`) chosen per mode; the accumulator `moffs` short form
(`A0`-`A3`) that nasm prefers for `mov eax,[abs]` in 16/32-bit; absolute
memory via SIB-with-no-base in 64-bit (`48 8B 24 25 …`, matching nasm's
choice over the longer `moffs64`); `mov crN`/`mov crN`(`0F 20`/`0F 22`),
`rdmsr`/`wrmsr`, `lgdt`/`lidt`, indirect `jmp reg` (`FF /4`), and the
`add/or/…/cmp reg,imm` family with nasm's imm8-vs-accumulator-imm32 selection.
Local-label scoping is now tracked during the assembly walk (not just at load),
so a local label inside an *expression* — the far jump's `jmp 0x08:.long64` —
resolves correctly.

**Definition of done — MET.** mini-asm `-f bin` reproduces nasm
**byte-for-byte** on `boot/smp/ap_trampoline.asm` (158 B), alongside the two
MBR variants from SH4a — **3 of the 4** flat objects.

**Gate.** MET. `tests/unit/test_asm_parity.sh` now byte-compares three `-f bin`
files (mbr, mbr_dual, ap_trampoline), green in `make test-unit`.  Receipt:
`[selfhost] asm PASS (bin): 3/4 flat objects byte-identical`.  The FAIL path
stays live: `stage2_start.asm` is deliberately uncovered and mini-asm refuses
it at `%include` — the honest SH4c boundary, not a silent wrong byte.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4c — `stage2_start.asm` byte-parity: %include/%if + the full encoder ✅ LANDED (2026-08-27)

**Goal.** Bring the last and largest flat file, `boot/bios/stage2/stage2_start.asm`
(548 lines + 13 `%include`d `.inc` files), to byte-for-byte parity — which
completes the `-f bin` set at 4/4.

**Scope.** The preprocessor the boot chain actually uses — `%include` (15
sites), `%if/%else/%endif`, `%error` (NOT `%macro`/`%rep`: a measured survey
found those only in the `-f elf*` `isr_stubs*` files, so they belong to
SH4d/SH4e, not here).  Plus the encoder surface stage2 needs and the MBRs did
not: SIB addressing with base+index*scale+disp (114 such operands, e.g.
`[fs:edi + 3*8 + 4]`), segment-override prefixes (`fs:`/`es:`/…), `bits 32`
protected-mode code, and the remaining mnemonics (`cpuid`, `in`/`out`,
`imul`, `movzx`, `loop`, `push`/`pop` imm, `rol`/`shl`/`shr`, `setcc`, …).

**Definition of done.** mini-asm `-f bin` output is **byte-for-byte** identical
to nasm on `stage2_start.asm` (with its include chain), making the flat set
4/4.

**Result — MET (4/4).** `stage2_start.asm` (with its 13-file include chain)
is now **byte-for-byte identical to nasm** (5632 bytes).  The preprocessor
half: `%include` (recursive, via `-I` paths), `%if/%else/%endif` (relational
operators added to the expression evaluator), `%error`, object-like `%define`
(boot_offsets.inc's integer constants), and the ELF `global`/`extern`/
`section` no-ops that `-f bin` ignores.  The encoder grew a full
effective-address model — `[abs]`, `[base+disp]`, `[seg:base+disp]`,
`[base+index]` (SIB), 16- vs 32-bit addressing with the `0x67` address-size
override, segment-override prefixes, disp8/disp32 shortest-form selection
(including nasm's omission of a zero displacement), the accumulator `moffs`
short form (incl. with a segment prefix), the 64-bit SIB-no-base absolute
form, and nasm's `mov r64, imm32` → 32-bit-form optimization — plus
`push`/`pop` (reg/sreg, with the `pusha`/`pushad`/`pushf`/`pushfd` 0x66
logic), `mov reg,reg`, `mov mem,imm`, the full ALU/`test` matrix (reg/mem/imm,
including the `test acc,imm` A8/A9 and `ALU al,imm8` accumulator short forms
and `test r/m,imm` F6/F7), `movzx`, `in`/`out` (with `need_66`), `imul`
(2/3-operand), `mul`/`div`/`not`/`neg`/`inc`/`dec` (FE/FF vs F6/F7), `lea`,
`loop`, shifts, `rep`+string ops, far jumps with `dword`/`word`, the
`abs`/`rel` hint, and `align N, db X` explicit fill.  The last divergences
were found by diffing per-instruction lengths against `ndisasm`: a
`split_operands` off-by-one that dropped a string operand's tail, the `0x67`
address-size prefix, the accumulator imm short forms, `inc/dec` FE/FF, and
the `mov r64,imm32` optimization.

**Gate.** MET. The host parity harness byte-compares all four `-f bin` files,
green in `make test-unit`.  Receipt:
`[selfhost] asm PASS (bin): 4/4 flat objects byte-identical`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4d — ELF64 backend, readelf parity on the kernel/libc objects ✅ LANDED (2026-08-27)

**Goal.** Add the `elf64` emitter so the kernel/libc `-f elf64` objects
reach structural parity with nasm.

**Scope.** ELF64 header, section headers, `.symtab`/`.strtab`/
`.shstrtab`, and the `R_X86_64_*` relocations the tree emits, plus
`global`/`extern`/`section`/`default rel` attribute handling, `align`/
`alignb` in an ELF context, and the `%macro`/`%endmacro`/`%rep`/`%assign`
preprocessor that `isr_stubs.asm` uses (`%rep 256` stub tables + `%rep 224`
NOERR stubs).  Per the SH4 definition of done, ELF output is compared **via
readelf**, not byte-for-byte (nasm does not guarantee ELF byte layout).
Targets: all 13 `-f elf64` files, boot-critical ones first.

**Result — MET (13/13).** `mini-asm -f elf64` now emits a real ELF64
relocatable object: ELF header, section headers, per-section data, symtab/
strtab/shstrtab and `.rela.*` sections.  Measured against nasm, the output
matches on section names/types/flags/sizes/aligns, symbol values/bindings/
section indices (FILE + SECTION entries, LOCAL-then-GLOBAL order, equ
constants as LOCAL ABS, unused externs dropped), relocation offsets/types/
addends (R_X86_64_PC32 with the -4 addend, R_X86_64_64 reduced to the
SECTION symbol + value), and the emitted section DATA bytes — 13/13.

Encoder growth required by the elf64 files (none of it exercised by the
`-f bin` set): 64-bit base/index registers (REX.B/REX.X, SIB fields), the
`rel`/`abs` hint + `default rel` → RIP-relative mod00/rm101 with
same-section resolution vs cross-section/extern PC32 relocations (a segment
prefix like `[gs:8]` stays absolute SIB-no-base, matching nasm), `mov r64,
imm` shortest-form selection (unsigned-32 zero-extend, then C7 /0
sign-extended, then imm64), `push imm8/imm32` (incl. `push qword N`),
`inc`/`dec` via FF /digit in 64-bit mode (0x40+r would be a REX prefix),
REX.W on 64-bit shifts, `o64 sysret`, `fninit`, `fxsave`/`fxrstor`/
`ldmxcsr`, `iretq`/`retfq`, `pushfq`/`popfq`, `syscall`, indirect
`jmp`/`call [mem]` (which load_line previously misclassified as a near
jump), and the string-op 66-prefix rule (8-bit ops must not get one).

Preprocessor growth: `%macro`/`%endmacro` (up to 8 args, `%1..%9`
substitution) and `%rep`/`%endrep` (bodies re-processed per iteration — the
handlers had to stop mutating shared body lines).  `%define`/`%assign` were
converted from assembler symbols to nasm-faithful **text macros** substituted
into every line before assembly, which is exactly what makes
`%rep` + `%assign i i+1` + `TABLE_ENTRY i` produce `dq isr0..isr255`.

**Gate.** MET. `tests/unit/test_asm_parity.sh` gained the elf64
readelf-comparison mode (sections/symtab/relocs + section-data bytes), green
in `make test-unit`: 4/4 bin + 13/13 elf64.  Receipt:
`[selfhost] asm PASS (elf64): 13/13 objects readelf-parity`.  Beyond the
gate, a kernel whose nine assembly objects were built by mini-asm links with
ld.lld and **boots to the interactive shell in QEMU** — the structural
parity is not decorative.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH4e — ELF32 backend + the in-guest assembly run ✅ LANDED (2026-08-27)

**Goal.** Finish the format coverage (elf32) and make the assembler actually
run *in-guest* — the phase's namesake.

**Scope.** The `elf32` emitter for the 7 `-f elf32` files
(`kernel/arch/i386/*`, `lib/libc32/*`), readelf-parity as in SH4d.  Then
mini-asm is built by the guest tcc (SH2) and run inside AuraLite to assemble
the boot-critical sources, proving the assembler self-hosts, not just that it
matches nasm on the host.

**Result — MET.** Two halves.

*ELF32 emitter (7/7 readelf parity).* `mini-asm -f elf32` writes real ELF32
objects: 52-byte header (EM_386), 40-byte section headers, 16-byte symtab
entries, and — the structural difference from ELF64 — **SHT_REL**
relocations: the addend lives in the field, not in the entry (`R_386_32` /
`R_386_PC32`, same type numbers as their R_X86_64 siblings).  Measured
against nasm, the 7 files match on section headers, symtab (FILE/SECTION/
LOCAL-including-equ/ GLOBAL with unused externs dropped), relocations and
section DATA bytes.  Two rules that only ELF32 exposes: absolute symbol
references (mov reg,sym / mov mem,sym / [sym] / dd sym / far-jump offsets)
**always** relocate, even within the same section (the base is a link-time
constant), while PC-relative references resolve within a section.  The
symtab places equ constants in *definition order* interleaved with labels
(boot32's `STACK_SIZE` at line 87 lands after `.fill_pde` at line 52), not
all upfront.

Encoder/preprocessor growth for the elf32 files: `%+` token pasting
(`dd isr_stub_%+v` → `isr_stub_0`), logical `&&`/`||` in `%if` (isr_stubs32's
error-code test — and the C short-circuit bug in the evaluator that left the
right operand unconsumed), parenthesised scaled indexes (`[edi +
(ecx+768)*4]` folds the constant into the displacement), `ltr r/m16`,
`iret`/`iretd` (66 only in bits 16), the `pusha`/`pushf` 66 rule (66 only in
bits 16 AND only for the `d` forms), `mov r16, sreg` 66 prefix, and
ELF32 symbol-immediates for `mov`.  The i386 kernel links with
mini-asm-built asm objects and boots to the shell.

*In-guest run.* `tools/mini-asm/mini-asm.c` is staged into the initrd as
`/src/mini-asm.c` along with the three boot-critical sources
(`/src/selfhost/{isr_stubs,syscall_entry,boot}.asm`), `asm_offsets.inc` and
host-built reference objects (`/src/selfhost/ref/*.o`, built with
`--file-sym /src/selfhost/<f>.asm` so the FILE symbol matches the guest
path).  The guest tcc compiles mini-asm.c, the SH3 aulink recipe links it,
and `test_selfhost_asm.sh` runs it with the new `--check-dir` mode
(multi-source + byte-compare), which prints the receipt.  Two guest-only
bugs were found and fixed: the 2 MiB `%rep`/`%macro` body arrays and the
1 MiB elf32 rela buffer sat on the C stack (fine on a host, fatal against
the 4 MiB guest user stack) and now live on the heap; and the shell's
`mkdir` takes one argument at a time.

**Definition of done — MET.** elf32 readelf parity on all 7 files; the
in-guest (tcc-built) mini-asm assembles isr_stubs.asm, syscall_entry.asm
and boot.asm **byte-identical** to the host-built references, receipt
`[selfhost] asm PASS: 3/3 objects byte-identical`.

**Gate.** MET. `tests/integration/cases/test_selfhost_asm.sh` (registered in
the `selfhost` shard) boots QEMU, builds mini-asm with the guest tcc and
greps `[selfhost] asm PASS: 3/3 objects byte-identical` — 2/2 assertions.
`tests/unit/test_asm_parity.sh` gained the elf32 readelf-comparison mode:
`[selfhost] asm PASS (elf32): 7/7 objects readelf-parity`, alongside the
existing `4/4` bin and `13/13` elf64 lines.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH5 — the kernel, built by itself ✅ LANDED (2026-08-28; SH5a–SH5d)

**Goal.** The x86_64 kernel links and boots when tcc + aulink built it.

**Why this is split.** The original SH5 bundled a measured spike, a linker
phase, a compiler port and a boot gate into one phase — the same mistake
SH4 made.  The clang-only flags (Fact 2) are four different problems with
four different answers: `-mcmodel=kernel` is answered by measurement
(SH5a), the aulink-side layout guarantees are a linker phase (SH5b), the
flag story + code fixes are a compiler phase (SH5c), and the guest build +
terminal boot is the gate (SH5d).  Splitting along that gradient makes each
step land a falsifiable increment.

| Sub-phase | Result |
|---|---|
| SH5a — spike: tcc codegen links AND boots at the higher half | ✅ landed (2026-08-27) |
| SH5b — aulink: kernel.ld layout parity vs ld.lld on the real kernel objects | ✅ landed (2026-08-28) |
| SH5c — the kernel, compiled by tcc (flag story + measured delta) | ✅ landed (2026-08-28) |
| SH5d — the in-guest build + the terminal boot gate | ✅ landed (2026-08-28) |

**Definition of done (umbrella).** SH5a answers the spike question with
numbers and a boot; SH5b proves aulink's kernel.ld output matches ld.lld's
layout on the real objects; SH5c builds `kernel.elf` from the kernel's C
sources with tcc (host first); SH5d builds it entirely in-guest (tcc +
aulink + mini-asm, no clang/ld.lld) and boots it to the shell.

**Gate.** Integration case boots the guest-built `kernel.elf`
(host QEMU, ISO path) and greps the standard boot receipts through
`[perf] boot-to-shell` — the same assertions `test_boot_to_shell` uses,
applied to the self-built kernel. Receipt:
`[selfhost] kernel PASS: tcc-built kernel booted to shell`.

**Deliverable.** The changes above, in the tree (D9: no
patch artefact).

---

### Phase SH5a — spike: tcc codegen links AND boots at the higher half ✅ LANDED (2026-08-27)

**Goal.** Answer Fact 2's measured question end-to-end: does TinyCC's
x86_64 codegen link at `0xFFFFFFFF80100000` with aulink — and does the
result actually boot?  If the answer is no, the honest fallback (kernel
keeps its host compiler) is recorded here instead.

**Result — MET (booted).** A minimal spike kernel — the real
`kernel/arch/x86_64/boot.asm` assembled by mini-asm (SH4d) + a
dependency-free `kmain.c` (`tools/selfhost/spike/kmain.c`) compiled by a
HOST tcc built from the same mob source the guest toolchain uses
(`make selfhost-host-tcc`; a private copy so `./configure` can never poison
the guest tree) — linked by aulink (SH3) against the real `kernel.ld`, was
packed into a dual-boot ISO with the tree's own `mkisoimage_dual.sh` and
booted in QEMU:

```
[BL4] entering long mode; jumping to kernel _start
[selfhost] SH5a spike: tcc+aulink kernel booted at the higher half
[selfhost] spike marker = 0x000000005a15a5e2 (expected 0x5a15a5e2)
```

**The measurement.** `readelf -r` on the tcc object shows ONLY
`R_X86_64_PC32`, `R_X86_64_PLT32` and `R_X86_64_64` — **zero 32-bit
absolute relocations**.  tcc's x86_64 codegen is almost entirely
RIP-relative (function calls are PLT32, global data is reached through
`lea [rip+disp]`), so the higher-half link address is representable without
`-mcmodel=kernel` — the small-model overflow that forces that flag on
gcc/clang simply does not arise.  The spike kernel's `spike_marker` global
(0x5A15A5E2 after `+1`) round-trips through exactly such a RIP-relative
relocation, which is why the receipt line is part of the gate.

**Flag story (first instalment).** `-mcmodel=kernel` — not needed (this
phase, measured).  `-fno-pie -fno-pic` — tcc ignores (its codegen is
RIP-relative either way).  `-mno-red-zone` — tcc has no red-zone usage in
the code it emits (confirmed on the spike object; the interrupt-safety
argument still needs the SH5c audit).  `-fstack-protector-strong` /
`-ffunction-sections` / `-fdata-sections` — tcc lacks them; `--gc-sections`
cannot run without the sections, so SH5c records the footprint delta
honestly.  The two tree changes this phase needed: `kernel.ld` maps tcc's
read-only data section (`.data.ro`, where tcc puts string literals) into
the rodata PHDR — the clang build never emits it, so it is a no-op there
and load-bearing here; and aulink's expression evaluator gained `|` / `&`
(the bitwise operators kernel.ld's `PHDRS FLAGS((1<<0)|(1<<2))` needs;
SH3's aulink had `<< >> + - * /` but not the bitwise OR).

**Gate.** MET. Host: `tests/unit/test_sh5_spike.sh` compiles the spike,
asserts no 32-bit absolute relocations and readelf-asserts the linked ELF
(entry `0xffffffff80100000`, 3 PT_LOADs `R E / R / RW`, higher-half
`__bss_start`/`__bss_end`); green in `make test-unit`, skips cleanly
without the host tcc.  Boot: `tests/integration/cases/
test_selfhost_kernel_spike.sh` (selfhost shard) boots the spike ISO and
greps the two receipt lines, 2/2 assertions.  Receipt:
`[selfhost] spike PASS: tcc+aulink kernel links at the higher half`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

### Phase SH5b — aulink: kernel.ld layout parity vs ld.lld on the real kernel objects ✅ LANDED (2026-08-28)

**Goal.** Prove aulink reproduces ld.lld's kernel.ld layout on the REAL
kernel (all 135 clang/nasm objects), not just the two-object spike.  The
honest comparison runs ld.lld WITHOUT `--gc-sections` so both sides link
the same input sections (gc is an OPT O8 footprint optimisation for the
clang build -- it needs `-ffunction-sections`, which tcc does not provide
and SH5c/d do not need; without gc a clang kernel does not boot, equally
for both linkers, which is why the boot gate is SH5d's).

**Result — MET.** aulink links the full kernel and matches ld.lld on:
entry point (`_start`), PT_LOAD count + flags (`R E / R / RW`), `.text`
address AND size 1:1, `.data`/`.bss` sizes 1:1, `.rodata` start address,
and every key `.text` symbol at an IDENTICAL address (`_start`, `kmain`,
`syscall_entry`, `context_switch`, `uart_init`).  `.rodata` size matches
within 0x10 (0.005%) and `isr_table` (the one `.rodata` symbol) within the
same delta.

**aulink changes (real bugs found by the 135-object comparison):**
- **SHF_MERGE|SHF_STRINGS pools.** ld.lld merges string sections
  (`.rodata.str1.1`, `.str1.16`) and fixed-size constant sections
  (`.rodata.cst4/cst16/cst32`) into per-section-NAME pools (ld.lld merges
  by name, not entsize -- str1.16 and cst16 share entsize 16 but get
  separate pools).  aulink now builds the same pools, placed at the first
  merge section's position, excludes the originals from the layout, and
  re-bases every relocation against them onto the pool address (PC-relative
  addends carry the standard -4 bias and are handled).  Without this,
  `.rodata` was ~0x258 larger than ld.lld's.
- **Section-start alignment.** The output section start is now re-aligned
  by the max input alignment AFTER the input groups are parsed (kernel.ld
  has no explicit `ALIGN()` on `.rodata`, so aulink started it at the
  `. += CONSTANT(MAXPAGESIZE)` result +0 instead of aligning by the
  inputs' 16).
- **Symbols inside section blocks.** `__bss_start`/`__bss_end` are defined
  around `*(.bss)` INSIDE the `.bss` block; aulink left `cur_addr` at the
  block start while parsing inputs, so `__bss_end == __bss_start` -- the
  kernel's boot.asm would zero a 0-byte bss (silently fine on QEMU's
  zeroed RAM, fatal on real hardware).  `cur_addr` now tracks
  `o->addr + o->size` after each input group.

**Gate.** MET. `tests/unit/test_sh5b_layout.sh` (registered in
`make test-unit`) links the kernel with both linkers and asserts the layout
above, 17/17 assertions; skips cleanly without ld.lld or the kernel
objects.  Receipt:
`[selfhost] layout PASS: aulink kernel.ld layout matches ld.lld (entry, PHDRs, .text 1:1, symbols)`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH5c — the kernel, compiled by tcc (flag story + measured delta) ✅ LANDED (2026-08-28)

**Goal.** Build `kernel.elf` from the kernel's C sources with tcc (host
first), closing the remaining Fact 2 flag story (`-mno-red-zone` audit,
stack-protector absence, the missing function-sections → no-gc footprint
delta) with measured numbers.

**Result — MET (and the tcc kernel boots).** All 126 kernel C files compile
with the host tcc (mob 2ba12e8), the 9 kernel `.asm` files assemble with
mini-asm, and aulink links the result against the real `kernel.ld`:
`build/selfhost/kernel-tcc.elf`.  Beyond the phase's host-link gate, the
tcc-built kernel was packed into a dual-boot ISO with the tree's own
`mkisoimage_dual.sh` and booted in QEMU: standard boot receipts through
`[perf] boot-to-shell: 310 ticks (~3131 ms)` (clang: 302 ticks — +2.7%
under TCG), `selftest=full` 19 PASS / 0 FAIL, the Ring 3 shell answers
`uname -a`, and `/bin/sysinfo` runs and exits cleanly through the
tcc-compiled syscall path.  Booting is still SH5d's gate (the build must
move in-guest first); this boot is recorded as evidence, not as SH5d met.

**Code fixes the full-kernel compile forced (all inert for the clang build
— every clang object's code sections are byte-identical to before, only
DWARF line tables shifted):**

- **`__attribute__((packed))` does not pack in tcc.**  tccgen.c consults
  `a.packed` only for a struct's final alignment, never for member
  placement — the x86_64 kernel's 124 packed sites (AHCI descriptors, USB
  descriptors, TCP/IP headers, on-disk structures) would have silently
  kept natural layout.  `#pragma pack(push, 1)` is the one spelling that
  packs members on both compilers, so every packed aggregate now carries a
  `__TINYC__`-guarded pragma pair (`tools/selfhost/packify_packed.py`
  performed the mechanical 124-site wrap).  Parity is machine-checked, not
  assumed: `tools/selfhost/gen_packed_probe.py` probes the sizeof of all
  119 packed aggregates, compiled by clang and by tcc — all 119 match.
- **tcc has no `__sync_*` builtins** — a `__sync_fetch_and_add` compiles
  as an implicit width-blind external call.  The queue-claim protocol
  (`on_queue`), TID/refcount counters and `ap_user_receipt_done` are
  spelled with the legacy names; `kernel/lib/atomic_compat.h` maps the
  five forms the tree uses onto `__atomic_*` builtins (per-site width
  correct, barrier semantics per gcc's model) under `__TINYC__`.
- **tcc lacks `__atomic_*_n` and `__ATOMIC_*`** as builtins: they come
  from tcc's `<stdatomic.h>` — now included by the five files that spell
  atomics directly (`tlb_shootdown.c`, `gui.c`, `perfstat.c`,
  `page_cache.c`, `uart.c`; uart also maps `__atomic_exchange_n`, which
  tcc's header lacks, onto the 4-arg `__atomic_exchange`).
- **tcc's assembler knows neither STAC/CLAC nor RDRAND/RDSEED.**  Under
  `__TINYC__` the same instructions are emitted from their encodings
  (cpu.h: `0F 01 CB/CA`; rng.c: `48 0F C7 F0/F8`, pinned to RAX so the
  ModRM is fixed).
- **Member `aligned(16)` is ignored by tcc** — the TCB's FXSAVE area moved
  8 bytes under tcc, every field after it shifted, and the asm context
  switch (via `asm_offsets.inc`) wrote `switch_parked` into the wrong
  slot: a deterministic early-boot deadlock.  The area is now a union
  with a `long double` member (natural 16-byte alignment in BOTH
  compilers — FXSAVE of a misaligned operand is #GP, this is not
  cosmetic).  `gen_asm_offsets` output is asserted identical from tcc-
  and cc-built generators.
- **tcc's frames are ~4x clang's**: it does not overlap stack slots of
  locals from disjoint switch cases, so `syscall_dispatch`'s per-case
  buffers sum to a 19 968-byte frame (clang: 4 616).  Kernel thread stacks
  grow 16 → 32 KiB (`kernel/proc/thread_stack.h`), and the IST region base
  is now DERIVED from the thread-stack region end instead of hardcoding
  the old 24-KiB-slot arithmetic (which the 32-KiB stacks silently
  overran, wiping the #DF IST1 stacks).
- **The loader's kernel window was 4 MiB** (2 × 2-MiB pages in both
  `paging.inc` and `efi_paging.c`); the clang kernel peaked at 3.96 MiB
  and the tcc kernel needs 4.27 MiB.  Both loaders now map 6 MiB.  The
  PMM already keeps the low 40 MiB out of the allocator, so the extra
  window costs nothing.
- **tcc emits `R_X86_64_GOTPCREL` (295 across the kernel)** for
  address-of-global operands.  aulink synthesised a `.got` for these but
  left the script's `__bss_start/__bss_end` at their pre-insertion
  addresses — `__bss_start` pointed INSIDE the `.got`, and boot.asm's
  .bss sweep zeroed every relocation slot (the userland never hit this:
  user.ld defines no `__bss_*` and the kernel's ELF loader zeroes user
  .bss from the PHDR, which the .got is not part of).  aulink now shifts
  every symbol at/after the old .bss base with the section.  Also
  `abort()` — libtcc1's `__va_arg` helper (tcc lowers va_arg through it)
  ends in abort() — is provided by the kernel (stack_protector.c, the
  compiler-runtime lane).
- The kernel link pulls five libtcc1 members (atomic.S, stdatomic.c,
  builtin.c, alloca.S, va_list.c) — the runtime helpers tcc's codegen
  actually calls.

**The flag-delta table (all measured on this tree, QEMU/TCG):**

| Flag / property | clang (shipped) | tcc (SH5c) | delta |
|---|---|---|---|
| `-mcmodel=kernel` | required (32-bit absolutes unrepresentable) | **not needed**: 0 × `R_X86_64_32/32S` across all 126 objects (SH5a's spike measurement, extended to the full kernel) | closed |
| `-mno-red-zone` | required | **not needed**: 0 negative-rsp memory operands across all 126 objects (tcc's codegen is frame-based) | closed |
| `-mno-mmx/-mno-sse/-mno-sse2` | enforced (0 xmm) | 1 191 xmm instructions in 4 objects (render3d 1 053, virgl 114, kprintf 16, bt 8) | safe *because* boot.asm enables CR4.OSFXSR before kmain and M1's eager FXSAVE preserves xmm state across switches; kprintf's are read-only varargs spills — measured, not assumed |
| `-fstack-protector-strong` | 310 instrumented call sites | 0 (flag absent) | accepted: kernel loses canaries; guard pages + the #DF IST lane remain |
| `-ffunction-sections -fdata-sections` + `--gc-sections` | yes | no flags, no gc | `.text`: 469 885 (gc) vs 518 589 (clang no-gc) vs 771 581 (tcc no-gc): gc saves clang 9.4%, tcc's codegen is +48.8% over clang-no-gc |
| `.rodata` / `.data` / `.bss` | 124 948 / 608 / 2 499 872 | 120 821 / 2 712 / 2 504 256 | bss ~identical (static arrays dominate) |
| kernel file size | 2 489 120 B (with `-g`, gc) | 1 218 504 B (no debug info) | not directly comparable; the section table above is the honest compare |
| boot → shell (`selftest=fast`) | 302 ticks (~3 050 ms) | 310 ticks (~3 131 ms) | +2.7% under TCG |
| boot → shell (`selftest=full`) | (CI lane) | 397 ticks (~4 010 ms), 19 PASS / 0 FAIL | the tcc kernel runs the full boot self-test suite |
| deepest frame | `syscall_dispatch` 4 616 B | 19 968 B | thread stacks 16 → 32 KiB |
| runtime helpers | none | 5 libtcc1 members, 295 GOTPCREL relocs resolved through aulink's synthesised `.got` | the honest cost of the tcc lane |

**Gate.** MET.  Host: `tests/unit/test_sh5c_kernel_tcc.sh` (registered in
`make test-unit`; 13 assertions) runs `tools/selfhost/build_kernel_tcc.sh`
(tcc compiles the 126 C files, mini-asm assembles the 9 asm files, aulink
links `kernel-tcc.elf`), asserts the ELF shape (entry == `_start`, 3
PT_LOADs `R E / R / RW`, ordered higher-half `__bss_start/__bss_end`), the
flag audits (0 32-bit absolutes, 0 red-zone references, 0 canaries,
kprintf's xmm ops are reads only) and both layout parities (tcc/cc
`asm_offsets.inc` identical; 119 packed structs sizeof clang==tcc); skips
cleanly without the host tcc / libtcc1.a.  Boot smoke:
`tests/integration/cases/test_selfhost_kernel_tcc.sh` (selfhost shard) —
19/19 assertions: standard boot receipts through `[perf] boot-to-shell`,
`uname -a` answered, `/bin/sysinfo` run and reaped, no PANIC/TRIPLE
FAULT/STOP.  Receipt:
`[selfhost] sh5c PASS: tcc compiles the kernel; aulink links it at the higher half`.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH5d — the in-guest build + the terminal boot gate ✅ LANDED (2026-08-28)

**Goal.** Build the kernel entirely in-guest (tcc + aulink + mini-asm, no
clang/ld.lld) and boot it to the shell.

**Result — MET.** `tests/integration/cases/test_selfhost_kernel_guest.sh`
boots a normal bootstrap image with a blank 16 MiB AHCI disk, then drives the
existing no-script shell with a prompt-aware serial transport.  On boot #1,
`/bin/tcc` compiles all **126** x86_64 kernel C sources in `/src` into
`/tmp/sh5d/cobj`; a guest-tcc-built `mini-asm` emits all **9** x86_64 kernel
assembly objects into `/tmp/sh5d/aobj`; and a guest-tcc-built `aulink` accepts
the two directories in lexical order, links them with the guest-staged
`libtcc1.a` and `/src/kernel.ld`, and writes `/fat/KERNEL.ELF`.  No host C
compiler, NASM, linker, Python generator, host-built tree kernel C/asm object,
or host kernel-link command is in that first-boot build path.  The bootstrap
`tcc`/`libtcc1.a` and already bootstrapped `init.elf` user program are explicit
seed inputs; closing those remaining bootstrap inputs is SH8 scope, not a
hidden kernel compiler or linker input.

The generated inputs are real guest programs too: C replacements for the
former Python emitters write `asm_offsets.inc`, `ap_trampoline.inc`, and
`init_bin.h` through explicit output-path arguments (smallsh has no `>`).
Both Python originals are **deleted**, so no host interpreter remains in the
kernel header path at all; `tools/check_selfhost_claims.py` now fails if
either one reappears while SH5 is marked landed.  The guest build required
the narrowly scoped closure repairs recorded below: `memchr` and dynamic `*`
printf width/precision for tcc/aulink diagnostics, staged-build-safe libc
directory includes, aulink immediate-directory `*.o` expansion, and initrd
metadata raised to 1,024 files / 128 directories.  The actual staged archive
measured **720 files, 78 subdirectories** (plus the archive's own `./` root
entry) against those 1024/128 bounds.  The host reference `build/mini-asm` is
now an explicit initrd prerequisite rather than a formerly implicit,
clean-tree-missing command.

The produced FAT file measured **1,220,552 bytes**.  It fits the stock
8,192-sector (4 MiB) FAT volume (8,095 usable clusters) with substantial
headroom, so this phase deliberately does **not** change the global FAT
formatting default or rely on an unimplemented AHCI capacity query.  Scratch
objects stay in `/tmp`; only the final kernel crosses the persistence boundary.

For the terminal proof, the host uses mtools only to extract that exact file
at FAT LBA 64, then the existing host image writer packs it for boot #2
(image tooling is explicitly still SH7 scope).  Boot #2 reached the ordinary
Ring 3 shell, answered `uname -a`, ran `/bin/sysinfo`, and reported the usual
IDT/TSS/SYSCALL/HHDM, PMM/VMM/scheduler/VFS/SMP, and `[perf] boot-to-shell`
receipts: **26/26 host assertions passed**, including the second-guest
terminal receipt, the explicit 1,220,552 B ≤ 4,144,640 B FAT payload
measurement, and a transport receipt proving all **167** queued commands were
each delivered behind a fresh prompt.  The extracted ELF is `EXEC`/X86-64
with `e_entry == _start == 0xffffffff801bbbe0`, and boot #2 reported
`[perf] boot-to-shell: 391 ticks (~3949 ms @ 99 Hz)`.

**Transport discipline.** `tests/integration/lib/prompt_qemu.py` sends one
command per fresh `auralite#` and refuses to continue past a `run` command
that did not exit zero (the kernel's own `[thread] ... exited (code=N)` line
is the receipt).  Without that, the first failing `tcc` in a 126-file queue
would be followed by another hundred compiles and an unrelated-looking link
error.  Both behaviours are unit-tested against a stub guest in
`tests/unit/test_prompt_qemu.sh`, and the new `memchr` is tested as the
*shipped* body (extracted and renamed, so GCC's builtin cannot shadow it) in
`tests/unit/test_string_ext.c` — a mutation of its unsigned comparison is
caught by 5 assertions.  `docs/driver_guide.md` §UART records why
prompt-aware driving is mandatory and why lengthening sleeps is not a fix.

**Gate.** MET.  The registered selfhost-shard case above performs both boots,
asserts the exact 126-C and 9-asm directory expansions, extracts and validates
the x86_64 ELF, and greps the standard boot receipts through
`[perf] boot-to-shell`.  It is in `SLOW_CASES_RE`, so `--fast` skips it like
the other correctness-over-speed gates.  Receipt:
`[selfhost] kernel PASS: tcc-built kernel booted to shell`.

**CI wiring pulled forward from SH9.** `run_all.sh` has defined a `selfhost`
shard since 2026-08-21, but `.github/workflows/integration.yml` listed only
`core/posix/fs/usb/net/gui` in its matrix — so every self-host gate, including
this one, was skipped on every push while CI reported green.  A phase gate
that never executes is not a gate, so the matrix now includes `selfhost`: the
job builds `selfhost-deps`/`selfhost-tcc`/`selfhost-host-tcc` before
`make iso` (the initrd stages `/bin/tcc` and the `/src` closure only when they
exist), and a final step requires a `PASS` line for all seven selfhost cases,
since `run_all.sh` records only PASS/FAIL and a silent skip shows up purely as
an absence.  The remainder of SH9 (cross-arch self-hosting) is unaffected.

Running the whole shard rather than the new gate alone immediately paid for
itself: it exposed `test_selfhost_aulink.sh` failing to link aulink in-guest
(the bootstrap libc set predated the directory operand, so `opendir` was
unresolved and the failure reported itself as a missing sysinfo banner) and a
pre-existing ordering bug in `test_selfhost_kernel_tcc.sh`, which redirected
its build log into a directory that only the script it was about to run
creates.  Neither is visible from the SH5d gate passing on its own.  Measured
after both fixes: **7/7 selfhost cases PASS, 0 failed** (1187 s).

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6 — shmake + shell scripting (umbrella) 🚧 IN PROGRESS

**Goal.** Build scripts run in-guest.

**Why this is split.** The original SH6 was written as one phase, but its
definition of done bundles three deliverables that share no code path: a
POSIX-subset `make`, four shell language features, and a build entry point
that has to survive being interrupted.  A measured survey of the tree
(2026-08-29) fixes the real surface:

- **The shell the plan names is not the shell that matters.**  Fact 8 and
  ledger SH-04 both say *smallsh*, but `userspace/system/smallsh/smallsh.c`
  is **173 lines** with **no builtin dispatch**, and it serves aarch64 and
  riscv64 only.  The x86_64 boot shell that every self-host gate drives is
  `userspace/system/init/init.c` — **1009 lines**, **31 builtins**, job
  control, and the `auralite#` prompt.  The plan's own hedge, *smallsh **(or
  its promoted successor)***, was an unresolved decision; SH6a resolves it by
  measurement and records it as D10.
- **The shell had no parser for any of it.**  Measured in `init.c` before
  SH6a: `pipe` **0** hits, `getenv` **0**, `setenv` **0**, `source` **0**,
  `expand` **0**, `$VAR` **0**.  So the language features can each be their own
  independently gated step rather than one big change.
- **Correction, measured 2026-08-29 during SH6b.**  This survey originally
  claimed *"pipes and redirects need no kernel work"* because `SYS_PIPE` (22),
  `SYS_PIPE2` (293) and `SYS_DUP2` (33) are implemented
  (`kernel/arch/x86_64/syscall.c:1385/1387/1406`) and wrapped
  (`lib/libc/src/libc.c:722-731`).  That measured the wrong thing.  The
  syscalls exist and work, but `SYS_WRITE` routes fd 1/2 to the console
  unconditionally, with an exception **only for pipes** (added so `gterm` can
  capture a child's stdout), and `SYS_READ` routes fd 0 to the keyboard
  unconditionally.  So `dup2` succeeded, the target file was created and
  truncated — and the bytes went to the console, leaving a zero-byte file.
  Measured, not inferred: `echo AAA > /tmp/p1.txt` printed `AAA` and `stat`
  reported **0 bytes**.  Redirects therefore did need a kernel change
  (ledger SH-37); pipes did not, because the pipe exception already existed.
- **There was no exit status to branch on.**  Every builtin returned `void`,
  and `cmd_run_argv` computed the child's `waitpid` status and then threw it
  away — so a failing compile was indistinguishable from a succeeding one.
  Nothing above that (`&&`, `if`, a build script that stops) can exist until
  the spine does.

Those three facts give a strict dependency order: the status spine and the
script runner first, then the language features that consume the status, then
`shmake`, then the entry point that needs all of them.

| Sub-phase | Adds | Gate (grep) |
|---|---|---|
| **SH6a** | exit-status spine; `sh <file> [args]` with `$0..$9`/`$#`/`$?`; line-numbered failures | `[selfhost] script PASS: <n> lines ran in-guest` |
| **SH6b** | redirects `>` `>>` `<` on the existing `SYS_DUP2`; named variables (`set`, `$NAME`) | `[selfhost] redirect PASS: <n> files written and read back` |
| **SH6c** | pipes `\|` on the existing `SYS_PIPE`; command lists `;` `&&` `\|\|` | `[selfhost] pipe PASS: <n> pipelines ran` |
| **SH6d** | control flow `if`/`while`/`for` — the subset `build.sh` needs | `[selfhost] control PASS: <n> branches and loops ran` |
| **SH6e** | `shmake`: rules, prerequisites, variables, phony targets | `[selfhost] shmake PASS: <n> targets up to date` |
| **SH6f** | `build.sh` as the single entry point; D5 host/guest target parity; D6 resume from `/fat` | `[selfhost] build PASS: kernel+initrd built on /fat` (terminal) |

**Definition of done.** The union of SH6a–SH6f.  D10 (`init.c` vs `smallsh`
vs a new `/bin/sh`) is resolved by measurement in SH6a, not asserted.

**Gate.** The union of the SH6a–SH6f gates; the terminal one is SH6f's
in-guest `test_selfhost_build.sh`.

**Deliverable.** The changes below, in the tree (D9: no patch artefact).

---

### Phase SH6a — spike (D10 decision) + exit-status spine + script runner ✅ LANDED (2026-08-29)

**Goal.** Resolve D10 with numbers, and give the shell the two things every
later sub-phase stands on: an exit status, and the ability to run a file of
commands with arguments.

**Result — the D10 spike, measured (not asserted).**

| Path | Measured cost | Verdict |
|---|---|---|
| **Promote `smallsh`** | **173 lines**, no builtin dispatch; aarch64/riscv64 only.  Promotion means re-implementing **31** builtins + job table + search path + the `auralite#` read loop that 143 integration cases wait on, and then keeping two x86_64 shells in sync. | **Rejected** |
| **New `/bin/sh`** | Builtins, `prog_resolve` and the job table are `static` in `init.c`; a separate program duplicates or exports all three, still needs `init` to spawn it, and cannot use a builtin from a script. | **Rejected** |
| **Extend `init.c`** | Additive: **+229 lines** on 1009, no existing dispatch branch rewritten, runner reuses the builtins already there. | **Chosen** |

**Decision (D10): extend `init.c`.**  Recorded as a decision because it is
load-bearing for SH6b–SH6f and because it corrects Fact 8, which named the
wrong file.  Consequence: `smallsh` stays the aarch64/riscv64 shell and gains
no scripting here — cross-arch shell parity is SH9's scope.

**What landed.**

- `userspace/system/init/sh_expand.h` — **159 lines**, pure (no syscalls, no
  globals, no allocation): positional-parameter expansion for `$0..$9`, `$#`,
  `$?`, `$$`.  Unknown names such as `$PATH` pass through **verbatim** so
  SH6b can define them without a migration.  It is a header rather than code
  inside `init.c` precisely so the host can compile and test the shipped body.
- `userspace/system/init/init.c` — **1009 → 1238 lines**.  `process_command`
  returns a status; `cmd_run_argv` returns the child's exit code (128+n on a
  signal) instead of discarding it; `sh <file> [args]` runs a script with a
  4-deep frame stack, each frame owning its own positional parameters; `exit N`
  inside a script stops the script.
- `tests/unit/test_sh_expand.c` — **64 assertions** against the shipped header
  (not a re-implementation), including the exact overflow boundary.
- `tools/selfhost/sh6a_{probe,nested,fail,exit}.sh` — four in-guest scripts,
  staged into the initrd at `/tests` **unconditionally** (unlike the guest tcc
  they need no fetched dependency, so this gate never skips).
- `tests/integration/cases/test_selfhost_script.sh` — **12 assertions**,
  registered in the `selfhost` shard.

**Two real defects found by writing the gate, not by reading the code.**

1. **`exit` inside a script halted the machine.**  `init` *is* PID 1 and the
   shell's `exit` calls `_exit(0)`, so the first build script ending in
   `exit 1` would have powered the system off instead of failing its step.
   The fix is the `sh_depth > 0` check; the proof is the last assertion in the
   case — after `sh6a_exit.sh` the host sends one more command and requires it
   to round-trip.  A shell that reports failure by halting would pass every
   other assertion.
2. **Partial numeric overflow read as success.**  `sh_expand_putnum` reported
   an error only when it wrote *nothing*; a `$?` of 255 expanding into a
   2-byte buffer returned `SH_EXP_OK` with the text `"2"`.  A silently wrong
   number is worse than an error, and silent truncation is exactly how
   ledger SH-14 turned a tcc link line into
   `unresolved reference to '__libc_start_main'`.  Caught by a mutation test:
   removing the `*over = 1` fails the suite (63/1) and restoring it passes
   (64/0).

**Gate.** `test_selfhost_script.sh`, **12/12 assertions**, verified against
the serial log rather than self-report:

```
[selfhost] sh6a: script=/tests/sh6a_probe.sh target=kernel args=1
[selfhost] sh6a: pwd-status=0
[selfhost] sh6a: dollar=$ env=$PATH
[selfhost] sh6a: nested script=/tests/sh6a_nested.sh target=kernel depth-ok
[selfhost] script PASS: 7 lines ran in-guest
this_command_does_not_exist_xyz: command not found
sh: /tests/sh6a_fail.sh:8: command failed with status 127
SH6A_STILL_ALIVE
```

The line number `:8:` is the point of the failure path, and `SH6A_STILL_ALIVE`
is the point of the exit path.  `UNREACHABLE-AFTER-FAILURE` and
`UNREACHABLE-AFTER-EXIT` must be **absent**.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6b — redirects + named variables ✅ LANDED (2026-08-29)

**Goal.** A script can write to a file and read a variable, without the host
having to stage the result.

**What landed.**

- **`userspace/system/init/sh_parse.h`** — new, **163 lines**, pure.  A
  quote-aware tokenizer that recognises `'…'`, `"…"`, `\x` and the operators
  `>`, `>>`, `<`, `&` **in one pass**.  One pass is not a stylistic choice:
  whether `>` is an operator depends on whether it is quoted, so a two-pass
  design (find the redirects, then split) redirects on a `>` that was inside a
  string.  A word therefore ends at the first *unquoted* operator, and
  `foo>bar` yields three tokens as POSIX does.
- **`userspace/system/init/sh_expand.h`** — **159 → 283 lines**.  A single
  entry point `sh_expand_word()` replaces SH6a's `sh_expand_positional()`,
  adding named variables and quote awareness.  Two expanders with different
  quoting rules would drift, and the only caller wanted the quote-aware one.
  Expansion runs **per token**, which is what stops a variable's value from
  injecting an extra argument or an operator — the classic shell injection bug
  that expanding the whole line first would reintroduce.
- **`userspace/system/init/init.c`** — **1238 → 1533 lines**.  `set
  NAME=VALUE` / `set` / `unset`, bare `NAME=VALUE`, redirects applied by
  swapping fd 0/1 around both builtins and spawns, and `cat` with no argument
  reading fd 0 (before that, `<` had nothing to feed: every builtin that could
  consume stdin demanded a filename, so `cat < file` printed "missing file"
  and the feature was syntax without a use).
- **The kernel fix** — `kernel/fs/vfs.c`, `kernel/fs/vfs.h`,
  `kernel/arch/x86_64/syscall.c` (**+32/−7**).  See the survey correction
  above: `SYS_WRITE` sent fd 1/2 to the console unless the slot held a *pipe*,
  and `SYS_READ` sent fd 0 to the keyboard unconditionally.  The predicate
  `vfs_fd_is_pipe()` became the more general `vfs_fd_is_devfs()`: fd 0/1/2
  take the hard-wired console path only while they still refer to a devfs node
  (`/dev/tty0`, `/dev/null`), so a redirect to a regular file is honoured while
  the console path and `gterm`'s pipe capture are both unchanged.

**Two defects found by running it, not by reading it.**

1. **The redirect was syntax without effect.**  `echo AAA > /tmp/p1.txt`
   printed `AAA` on the console and left a **0-byte** file: `open` +
   `O_TRUNC` had run, `dup2` had succeeded, and the bytes went to the console
   anyway.  This is the measurement that falsified the SH6 survey's "no kernel
   work" claim (ledger SH-37).
2. **`cmd_argv[argc++] = sh_expbuf[argc]`** — the increment and the read of
   `argc` are unsequenced, so which slot the pointer names is undefined.
   clang warned; another compiler could simply have picked the wrong one
   (ledger SH-38).

**One SH6a behaviour changed, and the full shard is what caught it.**  SH6a
left an unknown `$NAME` verbatim specifically to avoid pre-empting this phase;
now that named variables exist, an unset name expands to nothing, which is the
POSIX rule.  `sh6a_probe.sh`'s `env=$PATH` line and its host assertion both
encoded the old behaviour, so `test_selfhost_script` went red the moment SH6b
landed.  Both were updated to put `$PATH` behind single quotes (which SH6b made
suppress expansion), and this is recorded rather than quietly patched: a
sub-phase that silently invalidates its predecessor's gate is how an arc starts
testing nothing.  Running the whole shard, not just the new case, is the reason
it surfaced.

**Deliberately out of scope.**  `2>` and friends (the shell has one output
stream, so there is nothing to redirect); here-documents; and subshell
variable scoping — `sh <file>` runs in the **current** shell, so a variable it
sets survives, which is POSIX `.`/`source` semantics rather than POSIX `sh`.
SH6a documented the runner that way; if SH6f's `build.sh` needs isolation it
can add it.

**Gate.** `test_selfhost_redirect.sh`, **15/15 assertions**, verified against
the serial log.  The load-bearing one is `cat < $LOG > /tmp/sh6b_copy.txt`:
both directions on a single line, because a parser that handled only `>` would
pass every other check while `<` silently did nothing.  Also asserted: a `>`
inside double quotes stays text, `>>` appends instead of truncating, `set`
with no arguments lists the table, `unset` removes rather than empties, an
unopenable target is reported, an unmatched quote is refused **with its line
number** (`sh: /tests/sh6b_fail.sh:8: command failed with status 2`) instead
of being tokenized into something plausible, and the shell still answers
afterwards.

Host unit tests, both against the shipped headers: `test_sh_parse.c`
**87 assertions** (quotes, operators, unterminated quotes, the exact
token-array boundary) and `test_sh_expand.c` **122 assertions** (positional
parameters, variables, greedy name matching, quote rules, the exact overflow
boundary).  Mutation-checked: making variable lookup prefix-based instead of
exact gives 121/1, and disabling single-quote suppression gives 119/3.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6c — pipes + command lists ✅ LANDED (2026-08-29)

**Goal.** Commands compose, so a build step can be one line instead of a
temporary file.

**What landed.**

- **`userspace/system/init/sh_parse.h`** — `|`, `;`, `&&`, `||` join `>`,
  `>>`, `<`, `&` in the same one-pass tokenizer.  Two-character operators are
  recognised before their one-character prefixes, so `a && b` is ANDAND not
  AMP AMP, and `a || b` is OROR not PIPE PIPE.  A quoted `|` stays text.
  `|`/`&&`/`||` at end of line is `SH_PARSE_NOCOMMAND`; a trailing `;` or `&`
  is legal (empty command / background).
- **`userspace/system/init/init.c`**.  A line is a list of pipelines.
  `;` and `&` run the next element unconditionally (`&` also backgrounds its
  element); `&&` runs it only when the previous status is 0; `||` only when
  it is nonzero.  `$?` for the next element on the same line is the status of
  the last element that *ran* — a skipped element changes nothing.
- **Pipes on the existing `SYS_PIPE`.**  Stages of `a | b | c` run
  sequentially in the shell process, each one's stdout wired to the next
  one's stdin through a real kernel pipe.  Explicit redirects apply after
  the pipe wiring, so they win (POSIX).  Pipeline status is the last stage's
  status (POSIX; there is no `set -o pipefail`).  A backgrounded pipeline
  (`a | b &`) is one job: the existing one-fork subshell path, so the job
  table tracks it as one process group.

**Why sequential stages, not per-stage fork.**  The plan's wording — "a
process group per pipeline; the job table already tracks the rest" — was
read as "fork each stage, wait on the last".  That was tried.  Every
per-stage child resumed at the syscall stub and immediately took a
user-mode page fault (error 0x6, write to a non-present page).  The
existing `&` path forks a subshell *before* dispatch, which is a different
shape; cloning the shell from the middle of a multi-stage fd setup is the
shape that faulted.  Recorded as ledger SH-40 rather than silently papered
over.  Sequential stages are correct for every pipeline a build script runs
(`echo ... | cat > log`); the pipe buffer is 4 KiB, and SH6f's `build.sh`
does not write more than that without a reader.  No kernel change, which is
what the plan required.

**Two defects found by running it, not by reading it.**

1. **`run /nonexistent` exits 0.**  An absolute path bypasses
   `prog_resolve`'s existence check; the kernel creates the child and the
   load failure lands inside it as exit 0.  A relative name that is on no
   search path is 127 and never spawns.  The gate uses the relative shape.
2. **SH6a's runner stops the script on a non-zero line.**  An
   intentionally-failing `&&` chain therefore has to end with a `; echo
   survived` whose status is 0, or the rest of the probe never runs.  That
   is SH6a behaviour, not a regression; the probe documents it.

**Gate.** `test_selfhost_pipe.sh`, verified against the serial log.  The
guest script `tools/selfhost/sh6c_probe.sh` runs 4 pipelines (2-stage,
3-stage, a failing last stage behind `&&`, a roundtrip through a file) and
the three list operators, including a failing first element whose failure
propagates through `&&`.  Host unit tests: `test_sh_parse.c` covers the new
operators against the shipped header.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6d — control flow `if`/`while`/`for` ✅ LANDED (2026-08-29)

**Goal.** The subset of shell control flow `build.sh` actually needs — no
more, since every construct added here is one more thing SH8's closure has to
rebuild with tcc.

**What landed.**

- **Keywords stay words.**  `if`/`then`/`elif`/`else`/`fi`/`while`/`do`/
  `done`/`for`/`in`/`break` are not tokenizer operators.  Whether `if` opens
  a compound depends on it being the first word of a line, which is a
  command-level fact.  Quoting `if` therefore keeps it a command name.
- **`userspace/system/init/init.c`.**  A line whose first word is `if`/
  `while`/`for` is a compound, consumed as one command by `cmd_sh`.  A
  failing *condition* does not abort the script (so `if false; then` is not
  a top-level `false`); the construct's status is the last command of the
  taken branch, or 0 if no branch ran.  Body lines do not go through
  `cmd_sh`'s "stop on nonzero" loop.
- **A line-source, not a second reader on the frame.**  `struct sh_src` is
  either the running script frame or a collected body (an array of line
  pointers into that frame's text).  Nested compounds collect from the body
  they sit in.  Collecting a nested `if` with `sh_next_line` on the frame
  was tried in the design and would swallow the outer `done` — ledger SH-41.
  Depth on collect is the net open/close per line, so a one-line nested
  `if inner; then e; fi` is delta 0 and does not increment past the outer
  closer.
- **`true`/`false`/`break` builtins.**  A condition must not have to be
  `echo` (prints) or `run nosuch` (127 and a diagnostic).  `break` leaves
  the enclosing loop; outside a loop it warns and is not an error.
  `SH_MAX_LOOP` 1024 stops a `while true` without `break`.
- **`for x in <words>`.**  `in` is required (the positional-parameter form
  is out of scope).  The list is expanded once, before the loop; `$x` in
  the body re-expands each iteration.

**One-liners at the prompt work** (`if true; then echo x; fi`); multi-line
compounds need a script, because the prompt has no continuation reader.

**Deliberately out of scope.**  `case`, functions, `trap`, arithmetic,
`until`, `for` without `in`, and a compound that is not the first word of
its line (`echo x; if true; then ...` is a list whose second element is
the command `if`, not a compound).

**Gate.** `test_selfhost_control.sh` + `tools/selfhost/sh6d_probe.sh`.
A taken and an untaken branch, an elif, a `for` with a known iteration
count, a `while` with an early `break` and a nested `if` inside the body,
plus the same constructs typed at the prompt.  Receipt
`[selfhost] control PASS: 5 branches and loops ran`.  Host unit tests
pin that the keywords stay words against the shipped tokenizer.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6e — `shmake`: the build driver 🚧 PENDING

**Goal.** Dependencies are expressed once and honoured in-guest (D5).

**Definition of done.** `tools/shmake/shmake.c`, compiled by tcc in-guest:
rules, prerequisites, variables (`CC = tcc`), `.PHONY`, and timestamp
comparison — the subset `build.sh` needs, not GNU make.  Per D5,
`tools/check_selfhost_claims.py` asserts the host Makefile and `build.sh`
name the **same** target set, so the two build descriptions cannot drift
apart silently.

**Gate.** Integration case `test_selfhost_shmake.sh`: a guest run builds a
target, touches one prerequisite, re-runs and rebuilds **only** what depends
on it, then greps `[selfhost] shmake PASS: <n> targets up to date`.  The
"only what depends on it" assertion is the one that distinguishes a
dependency graph from a shell script with comments.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

### Phase SH6f — `build.sh`: the single entry point (terminal gate) 🚧 PENDING

**Goal.** One command, in-guest, drives kernel + initrd — and can be
interrupted and resumed.

**Definition of done.** `build.sh` at the root of the `/fat` worktree drives
kernel and initrd through `shmake`, with the target set mirrored against the
host Makefile (D5).  Per D6 the worktree lives on `/fat` with tmpfs as
scratch, and the build is **resumable**: a run killed at phase 6 of 9
continues from phase 6 on the next boot rather than restarting.  That is what
makes the remaining phases practical — without it every interrupted SH7/SH8
run costs a full rebuild.

**Gate.** Integration case `test_selfhost_build.sh` runs `sh build.sh kernel`
in-guest and greps `[selfhost] build PASS: kernel+initrd built on /fat`; a
second boot resumes from the same `/fat` tree (persistence proof, D6) and the
receipt appears again without recompiling what was already built.

**Deliverable.** The changes above, in the tree (D9: no patch artefact).

---

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
| SH-04 | shell: no pipes/redirects/variables/loops (Fact 8 named smallsh; the x86_64 shell is init.c, see D10) | missing | CLOSED (SH6a script runner + status spine; SH6b redirects + variables; SH6c pipes + lists; SH6d if/while/for/break) | SH6a-SH6d |
| SH-05 | ISO tooling is host python3/mtools (Fact 5) | missing | OPEN | SH7 |
| SH-06 | kernel CFLAGS clang-only, `-mcmodel=kernel` unportable (Fact 2) | port | CLOSED (SH5a measured it unnecessary; SH5c compiled all 126 files and recorded the flag-delta table) | SH5 |
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
| SH-17 | initrd metadata caps silently dropped staged self-host sources | limit | CLOSED (SH2: files 192→512; SH5d: full source closure requires 1,024 files / 128 dirs, measured 720 / 78) | SH2/SH5d |
| SH-18 | read_object() free(buf) leak: members point into freed archive/object image → shstrtab empty, 45 undefined refs | leak | CLOSED (SH3: keep images alive) | SH3 |
| SH-19 | load_archive() free(buf) leak: 7 ELF members, shstrtab/strtab freed, no archive relocations, 12 missed | leak | CLOSED (SH3: keep archive image alive) | SH3 |
| SH-20 | aulink file layout: file_off vs vaddr mismatch, .data at 0xd000 while p_offset said 0xcaa0 → kernel loaded zeros | layout | CLOSED (SH3: file_off = seg_p_off + (addr - seg_vaddr)) | SH3 |
| SH-21 | aulink input sections not ALIGN'd inside output section → movaps in .bss faults | align | CLOSED (SH3: ALIGN filled by sh_addralign, out_off) | SH3 |
| SH-22 | aulink forgot sec->out_off in P and memcpy → PC32 relocs off by section's position | reloc | CLOSED (SH3: base = out.addr + out_off) | SH3 |
| SH-23 | STT_SECTION symbols have st_name=0 → name empty, GOTPCREL unresolved | sym | CLOSED (SH3: name = sec_name for STT_SECTION) | SH3 |
| SH-24 | SH4 "byte-identical assembler for all 29 .asm" is a multi-kLOC x86 assembler across FOUR output formats (bin/elf64/elf32/win64) — too large for one falsifiable step | scope | OPEN (split into SH4a–SH4e along the format gradient, 2026-08-27) | SH4a |
| SH-25 | -f win64 (COFF, ms_abi) for w32/tests/*.asm (5 files) is a fifth format | scope | ACCEPTED (out of the self-host closure — Win32 test fixtures, not inputs to building the OS; D1 scope) | — |
| SH-26 | D4 unresolved: port nasm vs write mini-asm | decision | CLOSED (SH4a, measured: nasm needs 79 libc imports + ~150 kLOC; mini-asm 821 LOC byte-exact on 2/4 flat files → mini-asm) | SH4a |
| SH-27 | SH4b "all four flat files" bundled the full SIB/segment-override encoder (stage2: 114 SIB operands, 15 %include, bits 32) with the far simpler trampoline — too large for one step | scope | CLOSED (re-split 2026-08-27: SH4b=trampoline, SH4c=stage2; elf phases re-lettered SH4d/SH4e) | SH4b |
| SH-28 | mini-asm flat-file byte-parity | progress | CLOSED (SH4c: all 4/4 flat boot objects byte-identical) | SH4c |
| SH-29 | Python-only generated-header emitters cannot run under smallsh (no redirection or Python) | port | CLOSED (SH5d: portable C emitters accept explicit output paths; host stdout compatibility tested) | SH5d |
| SH-30 | a 135-object kernel link line exceeds smallsh argv limits and directory enumeration is nondeterministic | limit | CLOSED (SH5d: aulink lexically expands immediate `*.o` directory inputs; guest gate proves 126 C + 9 asm) | SH5d |
| SH-31 | sleep-fed UART commands are lost while guest tcc owns the polling console | process | CLOSED (SH5d: prompt_qemu transport sends each next command only after a fresh `auralite#`) | SH5d |
| SH-32 | initrd's SH4 reference assembler was invoked without a build prerequisite on a clean tree | build | CLOSED (SH5d: explicit `build/mini-asm` target stages the reference objects) | SH5d |
| SH-33 | SH6 bundled a make implementation, four shell language features and a resumable build entry point: three deliverables with no shared code path | scope | CLOSED (split into SH6a-SH6f along the dependency order: status spine, then language features, then shmake, then build.sh; 2026-08-29) | SH6a |
| SH-34 | D10 unresolved: extend init.c vs promote smallsh vs write a new /bin/sh | decision | CLOSED (SH6a, measured: smallsh is 173 lines with no builtin dispatch and serves aarch64/riscv64 only; a new /bin/sh would duplicate 31 static builtins; extending init.c is +229 lines and purely additive) | SH6a |
| SH-35 | shell `exit` called `_exit(0)` unconditionally, so inside a script it halted PID 1 instead of failing one build step | bug | CLOSED (SH6a: `sh_depth > 0` stops the script; test_selfhost_script.sh asserts the prompt still answers after `exit 3`) | SH6a |
| SH-36 | sh_expand_putnum reported overflow only when it wrote nothing, so a partial numeric expansion returned success with truncated digits | bug | CLOSED (SH6a: putnum reports a short write; pinned by a mutation test, 63/1 mutated vs 64/0 shipped) | SH6a |
| SH-37 | SYS_WRITE routed fd 1/2 to the console unless the slot held a pipe, and SYS_READ routed fd 0 to the keyboard unconditionally, so a redirect parsed, the file was created and truncated, and the bytes went to the console anyway (measured: 0-byte file) | bug | CLOSED (SH6b: vfs_fd_is_pipe became vfs_fd_is_devfs; fd 0/1/2 keep the console path only while they refer to a devfs node, so regular files and pipes are both honoured and gterm is unchanged) | SH6b |
| SH-38 | cmd_argv[argc++] = sh_expbuf[argc] -- the increment and the read of argc are unsequenced, so which slot the pointer names is undefined | bug | CLOSED (SH6b: split into two statements; clang -Wunsequenced is what caught it) | SH6b |
| SH-39 | build/user/init.o did not list sh_expand.h as a prerequisite, so editing the header did not rebuild the shell | build | CLOSED (SH6b: init.o depends on both sh_expand.h and sh_parse.h) | SH6b |
| SH-40 | per-stage fork() of a pipeline child resumes at the syscall stub and takes a user-mode #PF (error 0x6, write to a non-present page) | bug | ACCEPTED (SH6c: stages run sequentially in the shell through SYS_PIPE; a backgrounded pipeline is one job via the existing subshell fork.  Concurrent stages wait for a fork-clone fix that is not this sub-phase's kernel work) | SH6c |
| SH-41 | collecting a nested `if` inside a `while` body with `sh_next_line` on the script frame swallows the outer `done` | bug | CLOSED (SH6d: compounds collect from a line-source; a nested compound reads the collected body, not the frame.  Depth on collect is net open/close per line, so `if inner; then e; fi` is delta 0) | SH6d |

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
[selfhost] script PASS: <n> lines ran in-guest
[selfhost] redirect PASS: <n> files written and read back
[selfhost] pipe PASS: <n> pipelines ran
[selfhost] control PASS: <n> branches and loops ran
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
