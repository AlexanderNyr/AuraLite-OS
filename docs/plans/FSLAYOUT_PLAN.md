# AuraLite OS — Filesystem Layout and Application Installation Plan

## Status: COMPLETE ✅ — phases F0–F5 all done

This document answers two questions that keep coming up:

> *Will apps always live in `userspace/`, or will there be a separate `apps/`
> folder?*

> *Make it so programs can only be installed into specific directories.*

They are two different questions, and conflating them is the main risk here.
The first is about the **source tree**; the second is about the **runtime
filesystem**. This plan separates them, because they have different costs and
different payoffs, and doing the second one properly is worth far more than
doing the first.

It follows the structure of the existing plans (`GL_PLAN.md`,
`HARDENING_PLAN.md`, `POSIX_PLAN.md`): dependency-ordered phases, a definition
of done and a test gate for every phase, and one `.patch` per phase.

**Baseline:** commit `a422a93` (OpenGL phase G13).

---

## 1. Where things actually stand

Measured, not assumed.

### The source tree

`userspace/` holds **42 directories** with no internal structure:

```
apm  argv_echo  browser  calc  clock  editor  elfperm  execve_child  fdtest
fetch  fifolinktest  glcube  glgears  gltest  guess  gui-about  gui-audio
gui-browser  gui-calc  gui-edit  gui-files  gui-launcher  gui-sysmon
gui-taskmgr  gui-term  gui-theme  gui-usb  hello  http  init  life  matrix
p10test  play  proctest  selftest  snake  stackguard  sysinfo  tcpserver
timestest  udptest
```

Three quite different things are mixed together:

| Kind | Examples | Count |
|---|---|---|
| Real applications | `calc`, `editor`, `browser`, `gui-*` | ~20 |
| Test programs | `fdtest`, `p10test`, `proctest`, `elfperm`, `stackguard`, `timestest`, `udptest`, `fifolinktest`, `selftest`, `argv_echo`, `execve_child`, `gltest` | 12 |
| Demos | `glcube`, `glgears`, `matrix`, `life`, `snake`, `guess` | 6 |
| System | `init` | 1 |

`init` is special: it is the shell, and it is embedded in the kernel image as
well as in the initrd.

### The runtime filesystem

Everything is **flat in the root**. From the Makefile's initrd rule:

```
/init  /hello  /calc  /sysinfo  /editor  /http  /clock  /guess  /snake
/browser  /selftest  /proctest  /fdtest  /p10test  /argv_echo  /execve_child
/gltest  /glcube  /glgears  /gcalc  /gedit  /gfiles  /gterm  /gsysmon
/gabout  /gtaskmgr  /glaunch  /apm  /play  /gaudio  /gbrowser  /gusb
/tcpserver  /elfperm  /udptest  /timestest  /fifolinktest  /stackguard
/matrix.pkg  /life.pkg  /fetch.pkg
```

41 entries, all siblings of `/dev`, `/proc` and `/tmp`.

### There is already a package manager, and it already has an install path

`userspace/apm/apm.c` is not a stub — it installs packages:

```c
{ "matrix", "1.0", "...", "/matrix.pkg", "/tmp/matrix" },
{ "life",   "1.2", "...", "/life.pkg",   "/tmp/life"   },
{ "fetch",  "2.1", "...", "/fetch.pkg",  "/tmp/fetch"  },
```

So the answer to "where do installed programs go?" is currently **`/tmp`**,
which is the one directory guaranteed to be wiped. That is the strongest
argument in this document for doing something: the package manager works, and
it installs into scratch space.

### What already supports directories, and what does not

This is the part that decides how expensive the plan is, so it was checked
rather than guessed.

| Component | Supports paths with `/`? | Evidence |
|---|---|---|
| USTAR format | ✅ | `tar --format=ustar` writes `./apps/foo` into the 100-byte name field |
| `kernel/fs/initrd.c` parser | ✅ | stores the whole name after stripping `./`; never splits on `/` |
| `kernel/fs/vfs.c` resolution | ✅ | longest-prefix mount match, then the FS's own lookup |
| `tools/mkinitrd.sh` | ❌ | `cp "$INPUT_DIR"/*` — flattens, and its own comment says *"flat — no subdirectories yet"* |
| `initrd_readdir()` | ⚠️ | returns every entry with its full name, so `ls /` would list `apps/calc` rather than a directory |
| Shell `run` | ⚠️ | passes the string straight to `spawn()`; no search path |

**The kernel does not need to change.** A file packed as `apps/calc` will be
found by `open("/apps/calc")` today. What needs to change is the packaging
script, the readdir presentation, and the ~30 hardcoded paths in userspace.

### Hardcoded paths

```
"/apm"  "/gabout"  "/gaudio"  "/gbrowser"  "/gcalc"  "/gedit"  "/gfiles"
"/glaunch"  "/glcube"  "/glgears"  "/gsysmon"  "/gtaskmgr"  "/gterm"
"/gusb"  "/hello"  ...
```

Plus the launcher's table in `gui-launcher/glaunch.c` and the `apm` repo table.
Roughly 30 sites. Every one is a place a move can break something silently.

---

## 2. Decisions

### D1. Two separate changes, in this order: runtime first, source tree second

The runtime layout is what users and the package manager see, and it is where
the real defect is (`apm` installing into `/tmp`). The source tree layout is
cosmetics — a `git mv` and a Makefile edit that changes nothing observable.

Doing the runtime first also de-risks the source move: once paths are resolved
through a search path rather than hardcoded, moving directories around cannot
break a launcher entry.

### D2. Adopt a small, conventional runtime layout — not a novel one

```
/bin        core system programs   (shell, hello, apm)
/apps       applications           (calc, editor, gui-*)
/demos      demonstrations         (glcube, glgears, matrix, snake)
/tests      test programs          (gltest, fdtest, p10test, ...)
/pkg        package archives       (matrix.pkg, life.pkg, fetch.pkg)
/opt        installed packages     ← where apm installs, and it PERSISTS
```

Why these and not `/usr/bin`: AuraLite has no multi-user story, no `/usr`
split rationale, and inventing one would be cargo-culting. Six directories
with obvious names beat a faithful reproduction of a 1980s Unix hierarchy that
exists for reasons this OS does not have.

`/tests` in the shipped image is deliberate: the in-OS test programs are how
the integration suite works, and hiding them would break
`tests/integration/cases/*.sh`.

### D3. Installation is *restricted*, and the restriction is enforced in the kernel

The request was "programs can only be installed into specific directories."
There are two ways to read that, and only one is worth building:

- **Convention.** `apm` chooses to write to `/opt`. Any program can write
  anywhere; the rule is documentation. Cheap, and worth nothing against a
  buggy or hostile installer.
- **Enforcement.** The kernel refuses to create an executable file outside the
  permitted directories. A rule that cannot be bypassed by the program it
  governs.

This plan builds the second, because the first is not a feature — it is a
comment. The mechanism is deliberately narrow: a small allowlist consulted at
`open(O_CREAT)`/`execve` time, not a capability system.

### D4. Compatibility is a phase, not an afterthought

Moving `/gcalc` to `/apps/gcalc` breaks every script, every launcher entry and
every integration test that names it. So the search path (F2) lands **before**
the move (F3), and the move keeps root-level aliases until F5 removes them
deliberately.

---

## 3. Phases

### Phase F0 — Directory support in the initrd toolchain ✅ DONE

**Objective:** make it *possible* to ship a file at `apps/calc`. Nothing moves
yet.

The kernel already handles this; the packaging script does not.

#### Tasks

- [x] `tools/mkinitrd.sh`: recursive copy preserving subdirectories. Also
      added: a hard failure on paths at or beyond the 100-byte USTAR name
      limit, and `--sort=name` for a reproducible archive.
- [x] `kernel/fs/initrd.c`: a **derived** directory view. Directories are not
      stored; every `/`-terminated path prefix is registered at parse time and
      `readdir()` enumerates immediate children, collapsing deeper entries.
      A tree is the right answer for a writable filesystem; for an image
      parsed once and never mutated, derivation costs one pass and no
      invalidation logic.
- [x] `INITRD_MAX_FILES` 64 → 192, plus `INITRD_MAX_DIRS` 32. The aliases F3
      keeps roughly double the entry count, so 64 would have been hit
      mid-plan.
- [x] Proved end to end with `/etc/motd`, without moving any program.

#### What it found

`initrd_readdir()` returned every entry's full path from every read — latent
because no subdirectory had ever been packed. `lookup()` also had no notion of
a trailing slash, so `/etc/` would not have resolved.

#### Test gate

- A file packed at `apps/probe` is readable as `/apps/probe` from the shell.
- `ls /` shows `apps` as a directory, not `apps/probe` as a file.
- `ls /apps` shows `probe`.
- All 41 existing programs still resolve at their current root paths.
- `make test-unit` and the full integration suite unchanged.

**Result:** `tests/unit/test_initrd_dirs.c` 19/19 (compiles the real parser,
not a copy); `test_initrd_dirs.sh` 8/8; `make test-unit` 70/70 binaries;
`test_boot_to_shell` 17/17, `test_shell_commands` 10/10, `test_tmpfs` 9/9 —
no regressions. Documented in `docs/filesystem.md`.

#### Deliverable

`patches/FS_F0_initrd_dirs.patch` ✅

---

### Phase F1 — Enforced installation directories ✅ DONE

**Objective:** the kernel refuses to create an executable outside an allowlist.

This is the phase that answers the actual request, and it is deliberately
before the reorganisation — the restriction is worth having whatever the
layout ends up being.

#### The mechanism

A single allowlist consulted in the VFS when a file is created with an execute
bit, or when `chmod` would add one:

```
/opt        installed packages
/tmp        scratch (kept, because the shell and tests use it)
```

Everything shipped in the initrd is read-only, so `/bin`, `/apps`, `/demos`
and `/tests` need no entry — nothing can write there anyway.

#### Why in the VFS and not in `apm`

An installer that enforces its own rules constrains only itself. The check
belongs where every path goes through it, which is `vfs_open()` — the same
place the existing permission checks live.

#### Tasks

- [x] `kernel/fs/execpolicy.{c,h}` — its own translation unit, not vfs.c, so
      the host test compiles the shipping predicate rather than a copy.
- [x] Enforced on `open(O_CREAT)` with an execute mode bit, and on `chmod` /
      `fchmod` **adding** one. "Adding", not "having": a chmod that leaves an
      already-executable file executable is not an installation.
- [x] Returns `-EPERM` and logs the reason.
- [x] `apm` installs to `/opt/<name>`, mode 0755.
- [x] `/opt` exists as a second tmpfs volume with its own file table.
      **It does not persist** — see below.

#### What it found

**A pre-existing kernel bug: `EPERM` is 1, so `-EPERM` is `-1`**, which is the
sentinel `vfs_errno()` replaces with a fallback. `SYS_OPEN` and `SYS_OPENAT`
both wrapped `vfs_open()`, so every refusal reached userspace as `ENOENT`.
Nothing had noticed because nothing in `vfs_open()` had ever returned EPERM.

Also caught before shipping: the create check was first written *after*
`ops->create()`, leaving an empty file behind on every refusal.

And a discovery that changes what the tests prove: **the VFS does not
canonicalise paths at all**. `/tmp/../evil` is split at the `/tmp` mount and
the remainder given to tmpfs, which rejects names with a slash — so traversal
already failed for an incidental reason. The policy's own handling is proved
by the host test; the integration probe proves the policy is consulted.

#### Departure from the plan: /opt does not persist

The plan asked for a location that survives. `/opt` is tmpfs, so it does not.
Persistence needs a writable disk present on every boot, and the persistent
filesystems here mount only when their device exists — the choice was between
a location that is always there and one that always survives. F1 took the
first, because the defect that mattered was `apm` writing into a directory
whose purpose is to be wiped. Recorded in `TODO.md`.

#### Test gate

- `apm install matrix` puts the program in `/opt/matrix` and it runs.
- Creating an executable in `/`, `/apps` or `/etc` is refused with `EPERM`.
- Creating a **non**-executable file anywhere still works — this is not a
  read-only filesystem.
- `chmod +x` on a file outside the allowlist is refused.
- A program already running is unaffected.
- Host unit tests for the predicate itself, including the traversal cases
  (`/opt/../etc/evil`), which is exactly where an allowlist gets bypassed.

**Result:** `tests/unit/test_execpolicy.c` 25/25, `/insttest` 11/11,
`test_install_dirs.sh` 10/10, `make test-unit` 71/71 binaries, no regressions
across `test_initrd_dirs`, `test_boot_to_shell`, `test_shell_commands`,
`test_tmpfs`, `test_permissions`, `test_selftest`. Both the policy and its
canonicalisation were reverted in turn to confirm the tests detect them.

#### Deliverable

`patches/FS_F1_install_dirs.patch` ✅

---

### Phase F2 — A search path ✅ DONE

**Objective:** `run calc` works wherever `calc` lives, so the move in F3
cannot break anything.

#### Tasks

- [x] The list lives in **libc** (`libc/src/progpath.c`), not the shell —
      the GUI launcher launches programs too, and two copies of a search list
      is two things to update in F3, of which the second gets forgotten.
      Order: `/bin:/apps:/demos:/tests:/opt:/`.
- [x] `cmd_run()`, the **background** `run` path and the bare-command
      fallback all resolve through the same function. Leaving one hardcoded
      is how `run calc` and `run calc &` end up behaving differently.
- [x] An explicit path (anything containing `/`) bypasses the search.
- [x] `gui-launcher` stores names, not paths — twelve hardcoded paths removed.

#### A choice worth stating: `/` is searched last

After F3 ships compatibility aliases at the root, a program in its proper
directory must win over its own alias. Searching `/` first would mean the
aliases silently shadowed the real layout and F5 would then "break" things
that had been resolving to the wrong place all along.

#### Test gate

- `run calc` works while `calc` is still at `/calc`.
- `run calc` still works after F3 moves it to `/apps/calc` — the same test,
  unchanged, passing on both sides of the move. That is the point of ordering
  it this way.
- `run /apps/calc` works by absolute path.
- A name in no directory reports a clear "not found", naming what was searched.

**Result:** `tests/unit/test_progpath.c` 15/15 against a stub filesystem
(which makes the search *order* observable — the real filesystem only shows
which lookup won); `test_search_path.sh` 7/7; `make test-unit` 72/72
binaries; no regressions across `test_install_dirs`, `test_initrd_dirs`,
`test_boot_to_shell`, `test_shell_commands`, `test_jobcontrol`,
`test_userspace_apps`. Breaking the path joining so the root candidate became
`//calc` fails 3 of the 15 host checks.

#### Deliverable

`patches/FS_F2_search_path.patch` ✅

---

### Phase F3 — Move the runtime layout ✅ DONE

**Objective:** programs live in the directories from D2.

By now the search path exists, so this is a packaging change rather than a
flag day.

#### Tasks

- [x] Makefile packs into `bin/ apps/ demos/ tests/ pkg/`, driven by four
      name lists rather than 43 hand-written `cp` lines.
- [x] Root-level aliases for every moved program — as **hard links**, not
      copies. 43 duplicated binaries would take the image from 5 MB to 10 MB;
      a USTAR type-`1` entry is one 512-byte header. 5.1 MB → 5.3 MB.
- [x] `gui-launcher` and the shell needed no changes at all: F2 had already
      removed every hardcoded path from them. `apm` names `/pkg/*.pkg`
      through its root aliases.
- [x] Documented in `docs/filesystem.md`.

#### What it found

**`tar --sort=name` made the alias the real file.** When two names are hard
links, tar writes whichever it reaches first as the entry and the other as a
type-`1` link — and `./apm` sorts before `./bin/apm`. Both names resolved, so
nothing failed; the trap was laid for F5, where dropping the aliases would
have left every canonical path dangling. `mkinitrd.sh` now archives nested
paths first.

The kernel also needed hard-link and explicit-directory support, neither of
which the plan anticipated: it assumed aliases would be copies.

#### Test gate

- Every program runs at its new path.
- Every program still runs at its old root path (the aliases).
- The integration suite passes unmodified — the aliases are what make this
  possible, and it is the evidence the move is safe.
- `ls /`, `ls /apps`, `ls /demos` show a sensible hierarchy.

**Result:** `test_runtime_layout.sh` 12/12; **`test_search_path.sh` 7/7
unmodified across the move**, which is what ordering F2 first bought;
`test_initrd_dirs` 26/26 host and 8/8 in QEMU; `make test-unit` 72/72
binaries; `test_boot_to_shell` 17/17, `test_shell_commands` 10/10,
`test_install_dirs` 10/10, `test_userspace_apps` 4/4.

One `test_selftest` run tripped the kernel stack-protector under `-smp 2`;
three consecutive re-runs were clean. It matches the pre-existing SMP
instability in `TODO.md` and is reported rather than ignored.

#### Deliverable

`patches/FS_F3_runtime_layout.patch` ✅

---

### Phase F4 — Reorganise the source tree ✅ DONE

**Objective:** answer the first question — `userspace/` gains structure.

```
userspace/
├── system/     init
├── apps/       calc, editor, browser, gui-*
├── demos/      glcube, glgears, matrix, life, snake, guess
└── tests/      gltest, fdtest, p10test, ...
```

This is deliberately last. It changes nothing observable, and doing it before
F0–F3 would mean rewriting the Makefile twice.

#### Tasks

- [x] 43 `git mv`s into `system/ apps/ demos/ tests/` (1 / 21 / 7 / 14).
- [x] Makefile source paths, and four stale references in `kernel/fs/vfs.c`,
      `kernel/proc/scheduler.h` and `docs/opengl.md`.
- [x] A `README.md` per group. Each records something learned rather than
      restating the obvious: `system/` warns that `init` has two build sites,
      `apps/` that a non-interactive program must exit (two integration tests
      were rewritten because `calc` and `editor` swallowed the commands after
      them), `tests/` that "skipped" and "failed" are different answers.
- [x] `README.md` updated for both the source tree and the F3 runtime layout.

#### Test gate

- `make iso` and `make test-unit` produce byte-identical output to before the
  move. This is checkable and worth checking: it is what "cosmetic" means.

**The gate as written cannot be met, and the reason matters.** `tar` stores
mtimes, so no two builds of the initrd are byte-identical — before this
change or after it. The hash comparison would have failed on an unmodified
tree.

What was checked instead: the initrd was extracted before and after the move
and compared with `diff -r`. **All 85 files identical.** That is the claim
the gate was reaching for.

**Result:** `make test-unit` 72/72 binaries; `test_boot_to_shell` 17/17,
`test_runtime_layout` 12/12, `test_search_path` 7/7, `test_install_dirs`
10/10, `test_shell_commands` 10/10, `test_userspace_apps` 4/4.

#### Deliverable

`patches/FS_F4_source_tree.patch` ✅

---

### Phase F5 — Remove the compatibility aliases ✅ DONE

**Objective:** one location per program.

Separate from F3 so that the aliases can live through at least one full test
cycle, and so that removing them is a decision with its own diff rather than
a detail buried in a large one.

#### Tasks

- [x] Root-level aliases no longer created. 85 initrd entries → 43.
- [x] Fixed what broke, and logged it. The list is below, and it is the real
      output of this phase.

#### The inventory

| Site | Was | Found by |
|---|---|---|
| `kernel/fs/vfs.c` self-test | `/init` | grep |
| `kernel/proc/process.c` self-test | `/hello`, `/execve_child` | grep |
| **`kernel/gui/gui.c` start-menu table** | 10 × `/g*` | grep |
| `init.c` — `apm` and the GUI launcher | `spawn("/apm")`, `spawn("/glaunch")` | grep |
| **`gui-usb` buttons** | `spawn("/gfiles")`, `spawn("/gterm")` | grep |
| `execve_child`, `fdtest`, `selftest`, `insttest` | `/argv_echo`, `/hello` | grep |
| `sysinfo` banner text | `run /calc` | failing test |
| `test_shell_commands`, `test_initrd_dirs` | `ls /` shows programs | failing test |
| `test_syscalls`, `test_execve_args` | old paths in assertions | failing test |
| **`test_process_cleanup`, `test_memory_reaping`** | paths inside **regexes** | failing test |
| 35 integration scripts | `run /name` | mechanical |

The mechanical rewrite matched string literals and `run /name` and **missed
paths inside regular expressions** — `reaped '/hello'`, `'/proctest' \(tid`.
Two cases failed for that reason. The lesson is narrow and real: a
find-and-replace over a test suite is not a substitute for running it.

**The two other bold entries had no test that would ever have caught them.** The kernel's
start menu and `gui-usb`'s two buttons are never clicked by any suite, so a
stale path there is a menu item that silently does nothing. Section 5 of this
plan predicted exactly this — "grep only finds string literals" — and grep is
in fact what found them; what it could not have done is tell us they were
broken.

#### A test that could not have failed

`test_runtime_layout.sh` first asserted the absence of `reaped '/hello'`. The
shell creates a thread for a failed spawn and the kernel reaps it, so the line
appears either way. Replaced with the positive `spawn: '/hello' not found`.
Worth recording: an assertion that cannot fail is worse than no assertion,
because it looks like coverage.

#### Test gate

- Full suite passes with no root-level program entries.
- `ls /` shows only directories and system mounts.

**Result:** 12 integration cases green — `test_boot_to_shell` 17/17,
`test_shell_commands` 9/9, `test_runtime_layout` 11/11, `test_search_path`
7/7, `test_install_dirs` 10/10, `test_initrd_dirs` 8/8, `test_userspace_apps`
4/4, `test_selftest` 6/6, `test_syscalls` 4/4, `test_execve_args` 16/16,
`test_fd_isolation` 15/15, `test_permissions` 7/7. `make test-unit` 72/72.

#### Deliverable

`patches/FS_F5_drop_aliases.patch` ✅

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| F0 | Nothing else is possible until the packaging tool can make a directory |
| **F1** | **The actual request, and independently valuable — `apm` installs to `/tmp` today** |
| F2 | Must precede F3, or the move breaks every caller |
| F3 | The visible reorganisation, made safe by F2 |
| F4 | Cosmetic; last because it would otherwise be redone |
| F5 | A deliberate, separate decision |

**If only one phase is ever built, build F1.** The layout is tidiness; the
package manager writing executables into scratch space is a defect.

---

## 5. Risks

**Path assumptions nobody remembers.** ~30 hardcoded paths were found by
grep, and grep only finds string literals. Something will be constructing a
path at run time. The aliases in F3 and the deliberate breakage in F5 are the
mitigation: F5 is where the remainder surfaces, on purpose, with a test suite
watching.

**`initrd_readdir()` is the real work in F0.** It currently returns flat
names and the fix is a presentation change to a function every `ls` goes
through. Small, but not trivial, and worth its own tests.

**The allowlist is security-relevant.** An allowlist compared as a string
prefix is bypassed by `/opt/../etc`. The check must run after canonicalisation,
and the test gate names that case specifically.

**`init` is embedded in the kernel image**, not just the initrd. Moving it
touches the build in a second place.

---

## 6. What this does not do

- No `/usr` split, no multi-user permissions, no per-app sandboxing. Those are
  separate concerns and this plan does not pretend to address them.
- No package format, signing or dependency resolution. `apm` stays a
  demonstration; F1 only makes it install somewhere sane.
- No dynamic linking. Programs remain static ELFs.
