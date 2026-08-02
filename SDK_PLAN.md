# AuraLite OS — Third-Party Application Support Plan

## Status: COMPLETE ✅ — phases S0–S6 all done

This document answers one question:

> *How does someone who is not working inside this repository write, build,
> ship and install an application for AuraLite OS?*

It follows the structure of the existing plans (`GL_PLAN.md`,
`FSLAYOUT_PLAN.md`, `HARDENING_PLAN.md`, `POSIX_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, and one
`.patch` per phase.

**Baseline:** commit `9cb1ed0` (test_progpath include fix), on top of the
completed `FSLAYOUT_PLAN.md`.

---

## 1. Where things actually stand

Measured by experiment, not assumed. Every claim in this section was checked
against the tree at the baseline commit.

### The good news, established first

**An out-of-tree program already builds, loads and runs.** This was not
assumed — it was done:

```
$ cc -ffreestanding -fno-pie -fno-pic -O2 -I . -I libc/include \
     -c ~/sdk_probe/myapp.c -o myapp.o          # compiled outside the tree
$ ld.lld -nostdlib -static -T libc/user.ld ... myapp.o <26 objects> -o myapp.elf
$ # packed into the initrd, booted in QEMU:
MYAPP: hello from a third-party app
```

That result decides the shape of this plan. **The kernel needs no changes to
run third-party code.** `kernel/proc/elf.c` validates the ELF header, honours
each `PT_LOAD`'s own `p_vaddr`, and enforces W^X from the segment flags; it
does not require a particular load address or a particular producer. What is
missing is everything around that: a way to get the headers, a way to link
without knowing 26 object filenames, a way to ship the result, and a way to
install it.

So this is a **packaging and ergonomics** problem, not a kernel problem. That
is worth stating plainly, because the instinct would be to start with a
loader.

### What an out-of-tree build needs today

| Requirement | State | Evidence |
|---|---|---|
| Headers | ✅ available | 70 headers in `libc/include/`, plus `libauragui/include/auragui.h` and `libgl/include/GL/` |
| Compile flags | ⚠️ undocumented | `-ffreestanding -fno-pie -fno-pic -I . -I libc/include`, discoverable only by reading the Makefile |
| Linker script | ✅ exists | `libc/user.ld`, fixed load at `0x40000000` |
| **Runtime objects** | ❌ **no library** | `USER_COMMON` is **26 separate `.o` files** in `build/user/`; there is no `.a`, and `grep 'ar rcs' Makefile` finds nothing |
| A build to copy | ❌ none | every app is a hand-written pair of Makefile rules |
| Packaging format | ❌ none | `.pkg` files are bare ELF copies, no metadata |
| Install path | ✅ exists | `/opt`, enforced by the kernel (FSLAYOUT F1) |
| Getting a file in | ⚠️ rebuild only | a new program means editing the Makefile and rebuilding the ISO |

The single biggest obstacle is the fourth row. Linking the probe above
required naming all 26 objects explicitly:

```
crt0.o syscall.o libc.o malloc.o sigreturn.o setjmp.o compat.o pthread.o
rwlock.o barrier.o spin.o dirent.o regex.o env.o getopt.o pwd.o utsname.o
resource.o math_extra.o stdio_extra.o stdlib_extra.o string_extra.o
posix_extra.o posix_spawn.o q10_stubs.o progpath.o
```

Nobody outside this repository can be expected to know that list, and it
changes whenever libc grows a file.

### Everything is statically linked at a fixed address

`libc/user.ld` places every program at `USER_BASE = 0x40000000`. There is no
dynamic loader: `kernel/proc/elf.c` never looks at `PT_INTERP` or
`PT_DYNAMIC`, and `libc/include/dlfcn.h` exists but is a stub.

This is a real constraint on what "third-party application" can mean, and it
is not a small one. It also is not, today, a problem: one process per address
space means two programs at the same virtual address never collide.

### `spawn()` still does not forward arguments

```c
pid_t spawn(const char *path) {
    return syscall(SYS_SPAWN, (uint64_t)path, 0, 0, 0, 0, 0);
}
```

The kernel side reads one path and nothing else. `execve()` *does* pass
`argv`/`envp` correctly — `crt0.asm` decodes a full System V initial stack and
`/tests/argv_echo` proves it end to end. So the ABI is there and only `spawn`
is short.

The existing workaround is a convention: pass arguments through a file
(`/tmp/apm.args`, `/tmp/glcube.frames`). That is fine for programs inside this
repository that agreed on it. It is not something to hand a third party.

### `apm` cannot install anything it was not compiled with

```c
#define MAX_PKG 3
static struct package repo[MAX_PKG] = {
    { "matrix", "1.0", ..., "/pkg/matrix.pkg", "/opt/matrix" },
    ...
};
```

The repository is a **compile-time array**. `apm` installs correctly, into the
right place, with the kernel enforcing the destination — and it can only ever
offer those three packages. A third-party application cannot be added to it
without rebuilding the OS, which defeats the purpose.

### There is no way to get a file into a running system

Everything reaches the OS through the initrd, which is built by the Makefile
and embedded in the ISO. `/tmp` and `/opt` are tmpfs — writable, but empty at
boot and wiped on reboot. `/disk`, `/fat` and `/ext2` mount only when their
device is attached.

So even a perfectly packaged application has no route in, short of rebuilding
the image. This is the phase most likely to be underestimated.

---

## 2. Decisions

### D1. Ship an SDK, do not invent a runtime

The measured result above says third-party code already runs. The work is to
make it *reachable*: static libraries, headers, a linker script, a documented
flag set, and an example. Anything that looks like a dynamic loader, a
plugin ABI or a language runtime is out of scope, and would be work spent
before the cheap thing had been tried.

### D2. Static libraries, not a package of object files

`libaurac.a`, `libauragui.a`, `libaGL.a`. An archive is the one artefact a
linker command line can name without knowing what is inside it, and it makes
"libc grew a file" invisible to everyone downstream.

This is the single change that turns a 26-object link into `-laurac`.

### D3. The SDK is built from the same sources, never copied

A `sdk/` directory of duplicated headers would be wrong within a week. `make
sdk` **assembles** a staging tree from `libc/include`, `libauragui/include`,
`libgl/include` and `libc/user.ld`, and the assembly is checked by building
the example against it. A drift between the SDK and the OS then breaks the
build rather than a user's program.

### D4. A package is a file with a header, not a renamed ELF

`.pkg` files today are `cp foo.elf foo.pkg`. A real package needs at minimum a
name, a version and the payload's length and checksum — otherwise `apm` cannot
report what it is about to install, cannot detect truncation, and cannot tell
two versions apart.

The format will be deliberately boring: a fixed-size textual header followed
by the ELF. Not tar, not zip, not JSON — the kernel has no decompressor and
the parser has to be small enough to audit.

### D5. `apm` reads a repository from the filesystem

The compile-time array becomes a file. `apm` scans `/pkg` for `.apkg` files
and reads their headers, so dropping a package in makes it installable. No
network, no signing — those are separate problems with separate risks, and
saying so is better than half-building them.

### D6. Fix `spawn(argv)` rather than blessing the file convention

`execve` already carries `argv`. Extending `SYS_SPAWN` to take an argument
vector is a contained change to one syscall and one libc wrapper, and it
removes a workaround that would otherwise become SDK documentation. A
convention that exists because a syscall is incomplete should not be taught to
third parties.

### D7. Signing, dependencies and networking are explicitly out of scope

Named here so their absence is a decision rather than an oversight:

- **No signatures.** A checksum detects corruption, not tampering. Real
  signing needs a key story this OS does not have.
- **No dependency resolution.** Every application is statically linked;
  there is nothing to depend on.
- **No remote repository.** `apm`'s "fetching from upstream" messages are
  already theatre; this plan does not make them real, and phase S5 removes
  the pretence.

---

## 3. Phases

### Phase S0 — Static libraries ✅ DONE

**Objective:** link an application with `-laurac` instead of 26 object files.

#### Tasks

- [x] `build/lib/libaurac.a` (25 objects), `libauragui.a`, `libaGL.a`, via a
      new `make libs` target.
- [x] All 43 programs relinked against the archives and still working.
- [x] One list: `LIBAURAC_OBJS` feeds the archive rule, and the link line
      names only the archive.

#### Two things that had to be right

**`crt0.o` is kept out of the archive.** It defines only `_start`, which
nothing references — it is reached through the ELF entry point, not a
relocation — so as an archive member it would never be pulled in and every
program would link with no entry code.

**`--whole-archive` preserves the previous semantics.** Naming 26 objects
linked them all unconditionally. Measured, not assumed: without the flag,
`q10_stubs.o` is dropped entirely — `closelog` disappears and `calc` shrinks
from 96936 to 58472 bytes. Those stubs exist so a program calling an
unimplemented POSIX function links at all.

#### An unplanned improvement

Non-GUI programs shrank ~14.5 KB: `auragui.o` was previously forced into
every binary. Verified by diffing `calc.elf`'s symbol table before and after
— **only `ag_*` symbols and their file-local statics disappeared**, no libc
symbol was lost, and nothing new appeared.

#### Test gate

- `make iso` produces programs that are **functionally identical** to the
  object-linked ones. Byte-identical is not claimed: archive member ordering
  and `--gc-sections` behaviour can legitimately differ. The check is that the
  full integration suite passes unchanged.
- A program that uses only libc links without `libauragui.a` present.

**Result:** `tests/unit/test_userlibs.sh` 18/18 (wired into `make test-unit`),
`make test-unit` 50 suites green, `test_opengl` 86/86, `test_gui_usb` 5/5,
`test_runtime_layout` 11/11, `test_boot_to_shell` 17/17, and four other cases.
Removing `--whole-archive` fails the test, which is how it is known to test
anything.

Fixed along the way: `test_gui_usb.sh` still named `/gusb` (an F5 leftover),
and `test_shell_all.sh` — absent from `run_all.sh`, so never run — had four
independent faults including sending `exit` to a `calc` that leaves on `quit`.

Found and recorded in `TODO.md` rather than fixed here: **tmpfs implements no
`mkdir`**, so `/tmp` is flat and the old test asserted a success that could
never happen; and **`.init_array` is never executed**, so `gusb`'s constructor
is linked in and silently never runs.

#### Deliverable

`patches/SDK_S0_static_libs.patch` ✅

---

### Phase S1 — `make sdk` ✅ DONE

**Objective:** one command produces everything an outside developer needs.

```
build/sdk/
├── include/          libc, auragui, GL headers
├── lib/              libaurac.a libauragui.a libaGL.a crt0.o
├── user.ld
├── auralite.mk       the flags, as a makefile fragment to include
└── README.md
```

#### Tasks

- [x] `make sdk` assembles `build/sdk/` (89 files) from the real sources.
- [x] `auralite.mk` is generated by `tools/mksdk.sh`, and `sdk_check.sh`
      compares its flags against the OS's own so the two cannot drift.
- [x] The ABI is recorded in `auralite.mk`, and the documented load address is
      checked against the `user.ld` actually shipped.
- [x] `make sdk-check` builds the examples in a temporary directory with no
      path back into the source tree.

#### What it found

**Ten libc headers were not usable outside the tree.** They included each
other as `#include "libc/include/sys/types.h"`, which resolves only because
the OS builds with `-I .`. Every out-of-tree compile failed on the first
`#include <unistd.h>` — a real portability defect that no in-tree build could
have exposed. Now relative.

**A stale SDK passed the drift test.** `make sdk` rebuilds from scratch, so a
failed regeneration leaves the old tree in place; the first version of the
check deleted a header, watched make fail, and passed against the leftover.
The check now compares staged headers against their sources both ways.

#### Test gate

- `make sdk` from a clean tree produces the layout above.
- `make sdk-check` passes.
- Removing a header from `libc/include` breaks `make sdk-check` — proving the
  SDK is assembled, not copied.

**Result:** 31/31. The header-removal case was performed and does fail the
check.

#### Deliverable

`patches/SDK_S1_S2_sdk_and_examples.patch` ✅

---

### Phase S2 — A worked example ✅ DONE

**Objective:** something to copy, that is built and run by CI.

#### Tasks

- [x] `examples/hello-app/` — console application, three-line Makefile.
- [x] `examples/gui-app/` — an AuraGUI window with a working event loop.
- [x] Both live outside `userspace/`, build only from the staged SDK, and are
      packed into the image by `make iso`.

#### A third way a check can lie

`sdk_check.sh` copied each example with `cp -r`, which brought along `.o` and
`.elf` files from a previous local build. Make then said "nothing to be done"
and the check inspected an artefact the staged SDK had never produced. It
copies sources only.

And `set -o pipefail` with `grep -q` produced false failures: `grep -q` exits
on the first match, closing the pipe and killing `nm` with SIGPIPE, so the
pipeline reported failure *for a successful match*. Every example "had no
`_start`" while plainly having one.

#### Test gate

- Both examples build from the staged SDK.
- Both run in QEMU and print an identifiable marker.
- An integration case asserts the markers, so a broken SDK fails CI rather
  than being discovered by a user.

**Result:** `test_sdk_examples.sh` 5/5. A binary built only from `build/sdk`
boots and prints its markers.

Shipped together with S1: the examples are how the SDK is tested, and
separating them would have meant a phase whose deliverable nothing exercised.

#### Deliverable

Included in `patches/SDK_S1_S2_sdk_and_examples.patch` ✅

---

### Phase S3 — `spawn()` takes arguments ✅ DONE

**Objective:** remove the file-passing convention before it becomes SDK
documentation.

#### Tasks

- [x] `SYS_SPAWN` takes argv in `a2`; `0` is the old behaviour.
- [x] Reused `exec_args_capture()`, as the plan required.
- [x] `spawn(path)` unchanged; `spawnv(path, argv)` added.
- [x] `run prog a b c` forwards — and so does a bare `prog a b c`.
- [x] `/tmp/apm.args` retired. `glcube`/`glgears` read a *frame count* from a
      file, which is configuration rather than an argv workaround, so they
      were left alone; changing them would have been scope creep.

#### A small behaviour change worth naming

`run <prog>` alone now yields `argc=1` with `argv[0]` set, where it used to
yield `argc=0`. That is what `execve` has always produced and what a POSIX
program expects; previously a program could not learn what it had been
invoked as.

#### Test gate

- `run argv_echo alpha "beta gamma" 42` prints all four arguments, matching
  what `execve` already produces for the same input.
- A hostile `argv` (unterminated, bad pointer, over-long) is refused without
  a kernel fault — the same cases `execve` is already tested against.
- `spawn(path)` with no argv still works: every existing caller is a test.

**Result:** `test_spawn_argv` 11/11 and `test_spawn_argv_hostile` 10/10.
An argv — or a string inside it — pointing into kernel space is refused with
`-1`; an unterminated vector is bounded at `EXEC_MAX_ARGS`; a NULL argv is
identical to `spawn()`; the shell survives all of it with no panic and no
kernel-mode fault. `test_execve_args` is unchanged at 16/16, which is the
evidence the shared capture path was not disturbed.

One honest note recorded in the probe itself: the unterminated-vector array
lives in BSS, so the walk finds a zero just past its end and the spawn
succeeds. That case proves the walk is *bounded*, not that the input was
rejected.

#### Deliverable

`patches/SDK_S3_spawn_argv.patch` ✅

---

### Phase S4 — A package format ✅ DONE

**Objective:** a `.apkg` that describes itself.

```
AURAPKG1\n
name: hello-app\n
version: 1.0.0\n
description: ...\n
size: 96768\n
crc32: 0x1a2b3c4d\n
\n
<ELF payload>
```

#### Tasks

- [x] `tools/mkapkg` (a C program, not a shell script — it links the parser).
      It parses back what it wrote and refuses to leave an unreadable file.
- [x] `libc/src/apkg.c`, shared by `apm` and the tool.
- [x] **26 host checks**, nearly all malformed input.
- [x] The three packages are `.apkg` now, plus a deliberately corrupted
      fourth so the verification path is exercised by the suite.

#### What it found

**`printf` never parsed the `-` flag.** `%-12s` fell through and printed the
specifier *literally*; every column-aligned table in the OS was broken and had
been for as long as those tables existed. Nothing compared formatted output
against an expected string, so only a person reading `apm`'s listing could
have noticed. Fixed for `%s %d %u %x %X`, with a new 28-case test that
immediately caught `%-10x` slipping through the first fix.

#### Test gate

- Round-trip: build a package, read it back, byte-identical payload.
- Every malformed case is rejected with a specific message, not a crash.
- `apm info` shows real metadata read from the file.

**Result:** `test_apkg` 26/26 on the host; round-trip byte-identical;
every malformed case rejected with a specific message rather than a crash.

#### Deliverable

`patches/SDK_S4_S5_packages.patch` ✅

---

### Phase S5 — `apm` installs from the filesystem ✅ DONE

**Objective:** a package `apm` was never compiled with can be installed.

#### Tasks

- [x] `/pkg` is scanned for `*.apkg`; the compile-time array is gone.
- [x] `apm install <name>` resolves through the scanned headers.
- [x] `apm install /path/to/x.apkg` installs from anywhere.
- [x] The payload is verified **before** anything is written — verifying while
      copying would leave a half-written executable in `/opt`.
- [x] The invented "upstream" messages are gone.

#### Test gate

- A package built by `mkapkg.sh` and dropped into `/pkg` at image build time
  installs and runs, without `apm` having been recompiled.
- A corrupted package is refused and **nothing is written** — checked by
  listing `/opt` afterwards, not by trusting the return code.
- Installing over an existing package replaces it.

**Result:** `test_apm_packages` 12/12, including a corrupted package that is
detected, refused, and leaves nothing in `/opt` — checked by listing the
directory afterwards rather than by trusting a return code.

Shipped with S4: a format nothing reads is not testable, so the reader and the
format landed together.

#### Deliverable

Included in `patches/SDK_S4_S5_packages.patch` ✅

---

### Phase S6 — Getting a package into a running system ✅ DONE

**Objective:** close the last gap — an application that exists nowhere in this
repository reaching a booted machine.

This phase is last because it is the one with a real dependency on hardware
that may not be attached, and the honest answer may be "on FAT32, when there
is a disk".

#### Tasks

- [x] The route is **FAT32 on an AHCI disk**, mounted at `/fat`. It persists
      across reboots (`test_fat32_persistence` already proved that).
- [x] `apm install /fat/THING.APKG` works.
- [x] `test_external_install.sh` builds a program that is not part of the OS
      against the staged SDK, packages it, writes it to a FAT32 volume from
      the host, and installs and runs it in a booted machine.
- [x] Documented in `docs/filesystem.md`.

**The feared outcome did not happen** — there is a working route, so this
phase is code rather than an apology.

#### What it found

**The volume must start at LBA 64.** `kernel/fs/fat32.c` looks for a
signature there and formats the disk if it is absent; a plain
`mformat -i disk.img` writes its boot sector at LBA 0, so the kernel sees an
unformatted disk and **wipes it**, package and all. The first attempt did
exactly that: the volume mounted, `ls /fat` was empty, and only the
`formatting default FAT32 volume` line in the log explained why.

`mformat -i disk.img@@32768` is the fix, and the test asserts that
`formatting default FAT32` does *not* appear — so a silent reformat fails the
case instead of producing a mysteriously empty directory.

#### Test gate

- The end-to-end case passes: external package → attached medium → `apm
  install` → the program runs.
- `/opt` survives for the life of the session; its non-persistence across
  reboots is already recorded in `TODO.md` from FSLAYOUT F1.

**Result:** `test_external_install` 7/7 — a program compiled outside the OS
tree, packaged by the host tool, carried in on a FAT32 volume, verified,
installed into `/opt` and executed, with no rebuild of the OS at any point.

#### Deliverable

`patches/SDK_S6_external_install.patch` ✅

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| S0 | Nothing else is usable while linking needs 26 named objects |
| S1 | The SDK is the deliverable; it needs the archives first |
| S2 | The examples are how S1 is tested, so they follow immediately |
| S3 | Independent, but must land before the SDK docs teach a workaround |
| S4 | A format is needed before a repository can read one |
| S5 | The repository, now that packages describe themselves |
| S6 | Last: depends on hardware, and may end in a documented limitation |

**If only one phase is ever built, build S0.** Everything else is
convenience layered on it; the 26-object link is the thing that makes
third-party development effectively impossible today.

---

## 5. Risks

**The SDK drifting from the tree.** A copied header is wrong the moment
either side changes. Mitigated by D3 and by `make sdk-check` building the
examples against the staged SDK — but only if that check runs in CI. If it is
skipped, the SDK rots silently and the first person to notice is a user.

**The fixed load address.** Every program links at `0x40000000`. Nothing
today breaks — one address space per process — but it forecloses shared
libraries and makes ASLR impossible for user text. This plan does not fix it;
it documents it as the ABI, which is a commitment that will be awkward to walk
back.

**`argv` handling in `spawn`.** The dangerous part of S3 is not the plumbing,
it is copying an array of user pointers safely. `execve` already solves it;
the risk is solving it a second time slightly differently. The task list says
to reuse `exec_args_capture()` for that reason.

**A package parser is an attack surface.** S4's parser reads a
length-prefixed structure from a file the user was told to obtain from
elsewhere. Every field is attacker-controlled. The test gate lists malformed
inputs first for that reason.

**S6 may not have an answer.** If no supported configuration has a writable
medium at boot, there is no route in without rebuilding the image, and the
phase ends in documentation rather than code. That would be a real limitation
of the OS surfaced by this work — worth knowing, and not worth papering over
with a route that only exists in QEMU with specific flags.

**Scope creep toward a real package manager.** Signing, dependencies and
networking are each plausible "while we're here" additions and each is larger
than this entire plan. D7 names them as out of scope so that adding one is a
deliberate decision.

---

## 6. What this plan does not do

- No dynamic linking, no shared libraries, no `dlopen()`. `libc/include/dlfcn.h`
  stays a stub.
- No language runtimes beyond C.
- No cross-compilation story for non-Linux hosts; the SDK assumes `clang`,
  `ld.lld` and GNU make, which is what the OS already requires.
- No ABI stability guarantee across releases. The syscall numbers in
  `docs/syscall_abi.md` are documented, not frozen, and pretending otherwise
  would be a promise this project has no way to keep yet.
