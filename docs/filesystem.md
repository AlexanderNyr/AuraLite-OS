# AuraLite OS — Filesystem Layout

This document describes where files live at runtime and how the initrd is
built. It tracks `FSLAYOUT_PLAN.md`; each phase of that plan updates this
document as it lands.

**Status: complete.** Phases F0 (directories in the initrd), F1 (enforced
installation directories), F2 (a program search path), F3 (the layout move),
F4 (the source tree) and F5 (aliases removed).

---

## Mounts

| Mount | Filesystem | Writable | Notes |
|---|---|---|---|
| `/` | USTAR initrd | no | the shipped image; parsed once at boot |
| `/dev` | devfs | — | `/dev/null`, `/dev/zero`, ttys |
| `/proc` | procfs | no | process and kernel introspection |
| `/tmp` | tmpfs | yes | in-memory, lost on reboot |
| `/opt` | tmpfs (second volume) | yes | installed packages; see below |
| `/disk` | diskfs | yes | small persistent store on AHCI |
| `/fat` | FAT32 | yes | full FAT32 with LFN and subdirectories |
| `/ext2` | ext2 | yes | full ext2 with indirect blocks |

Resolution is by longest-prefix mount match, then the filesystem's own lookup.

---

## The initrd

`tools/mkinitrd.sh` packs a staging directory into a USTAR (POSIX tar)
archive; Limine hands it to the kernel as a boot module, and
`kernel/fs/initrd.c` parses the 512-byte headers into an in-memory file table
mounted read-only at `/`.

### Subdirectories

Since phase F0 the initrd can carry subdirectories. A file staged at
`<staging>/etc/motd` is packed with the USTAR name `./etc/motd` and resolves
at runtime as `/etc/motd`.

Two constraints are worth knowing:

- **USTAR names are 100 bytes.** `mkinitrd.sh` fails the build on a longer
  path rather than letting tar truncate it into a wrong or colliding name.
- **The archive is sorted** (`tar --sort=name`), so the file table order — and
  therefore any directory listing — is reproducible across builds.

### How directories work

The kernel's file table is **flat**: each entry stores its full path relative
to the mount root, for example `etc/motd`. Directories are not stored in the
image at all; they are *derived*:

- At parse time, every `/`-terminated prefix of every file path is registered
  as a directory. `tests/gl/gltest` registers `tests` and `tests/gl`. No
  entry in the archive has to declare them.
- `initrd_lookup()` checks the directory list first, then the file list, so
  `/etc` resolves to a directory vnode and `/etc/motd` to a file vnode.
- `initrd_readdir()` enumerates the *immediate* children of a directory,
  collapsing anything deeper into a single directory entry. Reading `/` shows
  `etc` as a directory, not `etc/motd` as a file.

A tree would be the right structure for a writable filesystem. For an image
that is parsed once and never mutated, derivation costs one extra pass at boot
and no invalidation logic at all.

A trailing slash asserts "this is a directory": `/etc/` resolves, and
`/etc/motd/` does not.

### Limits

| Constant | Value | Where |
|---|---|---|
| `INITRD_MAX_FILES` | 192 | `kernel/fs/initrd.c` |
| `INITRD_MAX_DIRS` | 32 | `kernel/fs/initrd.c` |
| USTAR name field | 100 bytes | format; enforced by `mkinitrd.sh` |
| `VFS_PATH_MAX` | 256 | `kernel/fs/vfs.h` |

`INITRD_MAX_FILES` was 64 with 41 entries in use. The reorganisation in phase
F3 keeps compatibility aliases for a while, which roughly doubles the entry
count, so the old ceiling would have been reached mid-plan.

Exceeding either table is a warning at boot, not a silent truncation.

---

## Where programs may be installed

Since phase F1 the kernel refuses to create an executable file outside a
small allowlist:

```
/opt   installed packages — this is where apm writes
/tmp   scratch
```

Everything else stays writable. This is not a read-only filesystem: a plain
data file can still be created anywhere. What is restricted is a file
*acquiring an execute bit*, whether at `open(O_CREAT)` with an executable mode
or later through `chmod`. A refusal returns `EPERM` and logs the reason —
a refused install that fails silently is a mystery, not a policy.

### Why in the kernel

An installer that enforces its own rules constrains only itself. The check
lives in `vfs_open()` and `vfs_chmod()`, where every path goes through it.
The predicate itself is in `kernel/fs/execpolicy.c`, separate so the host unit
test compiles the shipping code rather than a copy.

### Why /opt is a separate volume

`/opt` is a second tmpfs volume, not a directory inside `/tmp`. Separate file
tables mean scratch traffic cannot crowd out or evict an installed program,
and separate ops tables are what let `vfs_vnode_path()` tell the two mounts
apart — without that, `fchmod()` on a file in `/opt` would be judged as if it
were in `/tmp`.

### What this does not promise

`/opt` is **in-memory and does not survive a reboot.** The plan asks for a
persistent location; that needs a writable disk present on every boot, and the
persistent filesystems here mount only when their device exists. Shipping
`/opt` as tmpfs now fixes the defect that mattered — `apm` was installing into
`/tmp`, the one directory guaranteed to be wiped — and the durability is
tracked in `TODO.md` rather than claimed.

Path canonicalisation in the policy is **lexical**. It defeats
`/opt/../etc/evil`, the obvious bypass, but it does not follow symlinks: a
symlink inside an allowed directory pointing outside it would let a write
through. This is stated rather than papered over; closing it means resolving
the parent through the VFS first.

---

## Finding a program

Since phase F2 a command name is resolved through a search path rather than
being taken as a literal path:

```
/bin  →  /apps  →  /demos  →  /tests  →  /opt  →  /
```

The first existing entry wins. A name containing `/` is used as given — an
explicit path always bypasses the search.

`/` is searched **last** on purpose. When F3 ships compatibility aliases at
the root, a program in its proper directory must win over its own alias;
searching `/` first would let the aliases silently shadow the real layout.

The implementation is `libc/src/progpath.c`, not the shell, because the GUI
launcher launches programs too. One list, one lookup, both callers.

```
run calc        # searched
calc            # searched, after built-ins
run /apps/calc  # explicit, not searched
```

A failed search names the directories it looked in.

---

## Current runtime layout

```
/bin     init hello apm play sysinfo
/apps    calc editor http clock browser
         gcalc gedit gfiles gterm gsysmon gabout gtaskmgr glaunch
         gaudio gusb gbrowser
/demos   guess snake glcube glgears
/tests   selftest proctest fdtest p10test argv_echo execve_child gltest
         tcpserver elfperm udptest timestest fifolinktest stackguard insttest
/pkg     matrix.pkg life.pkg fetch.pkg
/etc     motd
/opt     (empty until something is installed)
```

**Each program has exactly one location.** `/calc` does not exist; `calc`,
`run calc` and `/apps/calc` all do.

During phase F3 every program also had a root-level alias, so that the move
could happen without a flag day. Phase F5 removed them. The initrd went from
85 entries to 43.

### Hard links (kept, though no longer used here)

The F3 aliases were USTAR type-`1` entries — a name pointing at an earlier
file, carrying no data — because duplicating 43 binaries would have taken a
5 MB image to 10 MB. `kernel/fs/initrd.c` still supports them: it is tested,
it costs nothing, and an image that wants two names for one file can have
them.

One consequence is worth knowing, because it was a real bug. When two names
are links to the same file, tar writes whichever it *reaches first* as the
real entry and the other as the link. Under a plain alphabetical sort `./apm`
comes before `./bin/apm`, so the alias became the file and the canonical path
became a link to it — harmless while both existed, and a trap for F5, where
dropping the aliases would have left every canonical path dangling.
`tools/mkinitrd.sh` therefore archives nested paths before root-level ones.

---

## Installing third-party software

An application built outside this repository reaches a running machine like
this (`SDK_PLAN.md`):

```
make sdk                       # once, in the OS tree
cc  -I $SDK/include  -c myapp.c        # against the SDK only
ld.lld -T $SDK/user.ld ... -o myapp.elf
build/mkapkg -n myapp -v 1.0 -o myapp.apkg myapp.elf
# write myapp.apkg onto a FAT32 volume, attach it, then in the OS:
apm install /fat/MYAPP.APKG            # verified, unpacked into /opt
run myapp
```

### The FAT32 volume must start at LBA 64

`kernel/fs/fat32.c` looks for a FAT32 signature at **LBA 64** and formats the
disk if it does not find one. A plain `mformat -i disk.img` writes its boot
sector at LBA 0, so the kernel sees an unformatted disk and **wipes it** —
silently, taking the package with it.

```sh
dd if=/dev/zero of=disk.img bs=1M count=32
mformat -i disk.img@@32768 -F -v AURALHCI ::   # @@32768 = 64 * 512
mcopy   -i disk.img@@32768 myapp.apkg ::MYAPP.APKG
```

This is a real trap and it cost a debugging cycle to find, so it is written
down rather than left to be rediscovered.

---

## Tests

| Test | Covers |
|---|---|
| `tests/unit/test_initrd_dirs.c` | the real parser against hand-built USTAR images: nesting, prefix collisions, trailing slashes, `readdir` bounds |
| `tests/integration/cases/test_initrd_dirs.sh` | that the packaging script ships a subdirectory and the shell can `ls` and `cat` through it |
| `tests/unit/test_execpolicy.c` | the allowlist predicate: traversal, prefix lookalikes (`/tmpfile`), the directory itself, overlong and relative paths |
| `tests/integration/cases/test_install_dirs.sh` | that the rule is wired into a running kernel, that `apm` installs into `/opt`, and that ordinary file creation still works |
| `tests/unit/test_progpath.c` | the search order and the path joining, against a stub filesystem |
| `tests/integration/cases/test_search_path.sh` | resolution by name on a running kernel — passed unmodified across the F3 move |
| `tests/integration/cases/test_runtime_layout.sh` | the directories, a program at its new path, and that the old root paths no longer resolve |
| `tests/integration/cases/test_external_install.sh` | the whole third-party route: build against the SDK, package, write to FAT32, install, run |

The unit test compiles `kernel/fs/initrd.c` itself rather than a copy, so it
cannot drift from the shipping parser.
