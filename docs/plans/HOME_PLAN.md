# AuraLite OS — Home Directory & User Database Plan (`HOME_PLAN.md`)

## Status: H0 ✅ · H1–H5 ⬜

> **Baseline:** `bcc248f` (merge of PR #22, 2026-11-22). Written against the
> tree as it stands, in the style of `OTA_PLAN.md` / `FSFULL_PLAN.md`: each
> phase has a dependency-ordered position, an exit gate that runs, and one
> `.patch` per phase under `patches/`.
>
> **The question this plan answers**, asked by a user and verified against the
> tree before writing a line: *"в ОС нету сейчас домашнего каталога? куда
> пользователь сможет сохранять заметки и т.д"* — and yes, there was no home
> directory (`grep -rn '"/home'` over the whole tree returned nothing) and no
> user database (`getpwuid()` was a compile-time stub answering root-and-NULL).
> A user saving a note had exactly three writable places: `/tmp` (scratch, by
> FSLAYOUT F1's own design), `/opt` (the package manager's install target) and
> `/documents` (the office suite's, OF0). None of them is "the user's".

---

## 1. Where things actually stand (measured, not assumed)

| Fact | Evidence on `bcc248f` |
|---|---|
| No home directory | `grep -rn '"/home'` over `kernel/ userspace/ lib/ docs/ Makefile` → **0 hits**; `ls /` lists no `home` |
| No user database | `lib/libc/src/pwd.c` is 21 lines: a static `root_pw` answering `getpwuid(0)`/`getpwnam("root")`, NULL otherwise |
| Credentials DO exist | `tcb->uid/euid/suid/egid`, `SYS_SETUID`/`SYS_GETUID` checks in `kernel/arch/x86_64/syscall.c`; every VFS permission check consumes them |
| `pwd` was a constant | `userspace/system/init/init.c`: `cmd_pwd() { puts("/"); }` — the process's real cwd (`tcb->cwd`, joined onto relative paths by the dispatcher) was never printed |
| No `cd` builtin | the dispatch chain has no `"cd"` arm; `SYS_CHDIR` exists and is used by `execvpe`'s PATH retry, so the kernel half works and the shell simply never exposed it |
| `/tmp`, `/opt`, `/dev/shm`, `/documents` are tmpfs volumes | `kernel/fs/tmpfs.c` — the mount-point construction `/home` reuses |
| `/documents` carries the lesson `/home` needed | `kernel/fs/documents.c` header: the symlink-into-/tmp design resolved on lookup but not on **create**, and `open(O_CREAT)` returned EROFS |
| Mount points are invisible to `ls` | `vfs_readdir()` returned the backing filesystem's listing only; hence `ls /home` and `ls /tmp` were reachable but `ls /` never said they existed |
| The in-guest build pins its source set | `tools/selfhost/gen_kernel_build.sh` refuses to emit a script when the C count differs from its pin (152 before H0) |

---

## 2. Phase map

| Phase | What lands | Gate | Status |
|---|---|---|---|
| **H0** | `/home` tmpfs volume; `/etc/passwd` parsed at boot; `SYS_GETPWENT`/`SYS_GETHOME`; libc `getpwuid`/`getpwnam`/`getpwent`/`getpwuid_r`; shell `cd`, real `pwd`, `~`, `$HOME`/`$USER`; `ls /` shows mounts | `test_home.sh` 13/13 + `test_home_passwd` (26 checks + mutation lane) in `make test-unit` | ✅ **landed** |
| **H1** | `/root` as a home: the initrd root becomes writable for root's own directory (or `/root` becomes a symlink into `/home/root`), so `cd /root` means what it says | root's `pw_dir` returns to `/root`; `test_home` pins both spellings | ⬜ |
| **H2** | `adduser`/`passwd`-shaped tooling: edit `/etc/passwd` (and a future `/etc/shadow`) from the shell, create the home with the right owner | a user added at the prompt can save a note without a rebuild; hostile names refused | ⬜ |
| **H3** | Ownership: new homes get the record's `uid:gid`, and `vfs_mkdir`'s blanket `umask` stops being the only authority; `~` refuses another user's home | two uids in one boot, each sees only its own home; `stat` shows the record's uid | ⬜ |
| **H4** | Durability: *"сделать свою файловую систему очень быструю… и сделать её фишкой ОС"* — the user's own idea, recorded here as the plan's north star. `/home`, `/etc/passwd` and `/opt` move onto a real writable filesystem (the FS modernization plan owns the format) | a note survives a reboot; the receipt drops `storage=tmpfs` | ⬜ |
| **H5** | Login: `getty`-shaped session over the serial console, `passwd`-like verification, per-user cwd/shell env at start | a wrong password is refused; a right one lands in the right home | ⬜ |

**Dependency notes.**
- H1 must precede H2: a tool that creates homes must not be able to create a
  home on a read-only filesystem.
- H3 depends on H2 only for a convenient way to *make* a second user; the
  permission machinery itself (`vfs_check_perm`) already exists.
- H4 depends on the filesystem modernization plan (`FSMOD_PLAN.md`), which is
  the larger series the user proposed after this phase: H0–H3 are deliberately
  **format-agnostic** (they use the VFS, not a disk layout), so H4 replaces the
  backing store without redoing any of them.
- H5 is last because a login that cannot persist a password (H4) or separate
  owners (H3) is theatre.

---

## 3. H0 as landed — the decisions and why

### 3.1 `/home` is a tmpfs MOUNT POINT, not a directory

Three constructions were available and two are wrong:

1. **A directory inside `/tmp`** — fails the point: /tmp is scratch that
   FSLAYOUT F1 says may be wiped, and its 256-file table would be shared.
2. **A symlink `/home -> /tmp/home`** — reads correctly (`ls /home` lists
   the target) and cannot be written through: this VFS follows a symlink when
   resolving a path and not when *creating* one, so `open("/home/x",
   O_CREAT)` falls through to the read-only initrd at `/` and returns EROFS.
   This is exactly the defect `documents.c`'s header records discovering the
   expensive way.
3. **Its own tmpfs volume at `/home`** (what landed) — resolved by the mount
   table in BOTH directions, owns its own file table, and tells itself apart
   from /tmp, /opt, /dev/shm and /documents by a distinct `vfs_ops` address
   (the same reason those three exist).

### 3.2 The user database is DATA

`/etc/passwd` lives in the initrd and is parsed once at boot. Adding a user is
editing a text file, not editing a table in `kernel/fs/home.c`. The record
grammar is the file's own grammar; the two syscalls hand it out in that same
format so libc parses one thing.

**The parser refuses rather than truncates.** A field longer than its
destination, an 8-field line, a non-numeric or `> 2^32-1` id, a nameless
record — each is a malformed line, counted and reported
(`"(N malformed line(s) refused)"`). Truncating would hand out a WRONG home
directory, which is worse than no record at all.

**6-field records default the shell.** `/etc/passwd` proper has no comment
syntax and no optional field, but this tree's file is hand-maintained; the
parser accepts `#`/blank lines as skips (not malformed) and a 6-field record
as "shell = /bin/sh", because a record with a home but no shell is a user, not
a corruption. Five fields, however, is malformed: a record with no home is the
shape that walks users into `/` by accident.

### 3.3 `root`'s home is `/home/root`, and the difference is load-bearing

Found by running the gate, not by reading the code: with root's `pw_dir` set
to `/root` (the natural spelling, and what the pre-H0 stub answered), the
first boot printed

```
[home] FAIL: /root lies on a read-only filesystem (EROFS): /etc/passwd names
a home the initrd owns; on this tree the durable-looking place for a user who
must save notes is /home/<name> (tmpfs, writable until power-off)
```

`/root` lives **on the initrd**, which is read-only, and the shell runs as
uid 0 — so `cd ~` would have landed a user in a directory that cannot hold a
note, which is the exact question this plan exists to answer. H0 therefore
ships `root:x:0:0:root:/home/root:/bin/sh`, and **H1** is the phase that makes
`/root` real (a symlink into `/home/root`, or a writable initrd root).

The receipt distinguishes the two failure shapes on purpose: EROFS is a
configuration mistake in `/etc/passwd` and gets a sentence naming the path;
everything else is a filesystem failure and gets the errno.

### 3.4 `ls /` shows the mount table

Without this, everything above is invisible: `vfs_readdir()` returned the
backing filesystem's listing, so `/tmp`, `/dev`, `/proc`, `/documents` — and
the new `/home` — never appeared in `ls /`. The VFS now unions in the mount
points that live **directly** under the listed path. Properties that matter:

- **Direct children only.** With `path = "/"`, a mount at `/dev/shm`
  contributes nothing; `ls /dev` adds `shm` on its own call.
- **No duplicates.** A mount the backing filesystem already lists is skipped,
  not listed twice.
- **The filesystems are untouched.** An initrd entry is still listed exactly
  once; this is a union at the VFS edge, which is what every Unix's `ls /`
  shows.

`ls /home` additionally needed `vol_of_vnode()` to learn `home_vol`: without
it the volume root fell through to the per-file branch with NULL `fs_data` and
`readdir` returned -1. That is byte-for-byte the omission OF2's closure fixed
for `doc_vol` — the same defect, found again by the same test shape.

### 3.5 The shell stops pretending

- `pwd` prints `getcwd()`, not `"/"`. `getcwd()` failing still prints `/`,
  which is the kernel's own convention for an empty cwd.
- `cd [dir]` is new; bare `cd` goes home. It is a thin wrapper over the
  existing `SYS_CHDIR` — the kernel always could, the shell never exposed it.
- `~` expands at the start of an argument (and only there), decided by the
  **raw token**: `echo "~"` stays a literal tilde, `echo ~/x` expands. `~user`
  is deliberately NOT expanded — it needs a lookup, and silently treating
  `~alice` as `~` is the class of wrongness this project names.
- `$HOME`/`$USER` are exported at startup from ONE `getpwuid(getuid())`. The
  first build of this export called the function three times inside a single
  ternary; because `getpwuid()` answers a pointer into a static record the
  next call overwrites, `USER` came out **empty** — caught by the integration
  case, not by review. The comment at the function records it.

---

## 4. What H0 deliberately does NOT do (and where each debt lives)

| Not done | Why, and who owns it |
|---|---|
| Durability across reboot | `/home` is tmpfs. **H4.** The boot receipt says `storage=tmpfs (volatile: …)` rather than letting a user discover it. |
| Per-user ownership of homes | Homes are created 0755 root-owned by the boot bring-up. **H3** (the permission machinery exists; the *creation* owner does not). |
| `~user` expansion | Needs a name→record lookup in the expander; **H2** (the same tooling that adds users). |
| `/root` as a real directory | It is on the read-only initrd; root's home is `/home/root` today. **H1.** |
| Passwords | `passwd` field is parsed and reported as `"x"`; no hashing, no verification, nothing reads it. **H5.** |
| Login sessions | One shell, uid 0, no `getty`. **H5.** |
| Moving `/documents` under a home | OF0's volume is shared by convention; a per-user documents location is a UI question (which home is "the" home for the office suite?) that H3 must answer first. |
| An on-disk `/etc/passwd` | Today it is baked into the initrd at build time (the Makefile recipe). Editing it at runtime needs a writable `/etc`, which is H4's dependency. |

---

## 5. Test ledger for H0

| Gate | Where | Result |
|---|---|---|
| Parser grammar: 26 checks (canonical, 6-field default shell, 5-field refusal, non-numeric/empty/overflowing uid, 8-field refusal, blank/comment skip, CRLF, last-line-no-newline, 2^32-1 boundary, whole-file path) | `tests/unit/test_home_passwd.c` in `make test-unit` | **26/26** |
| Mutation lane: the default-shell rule removed → the gate MUST fail | same file, `-DTEST_MUTATE_PARSE=1`, built and required to exit 1 by the Makefile rule | **red as required** |
| Boot receipts: records parsed, homes created, first writable | `test_home.sh` | ✔ |
| `ls /` shows `home/`; `ls /home` shows the records' directories | `test_home.sh` | ✔ |
| `cd ~` → the record's directory; `pwd` says so; `cd /` → `/` | `test_home.sh` | ✔ |
| A note saved at `~/note.txt` reads back from the absolute path | `test_home.sh` | ✔ |
| `$HOME`/`$USER` come from the record; `"~"` stays literal | `test_home.sh` | ✔ |
| The receipt states `storage=tmpfs` | `test_home.sh` | ✔ |
| Width sweep: the new file adds **zero** `(uint64_t)` casts (449 → 449) | `test_width_sweep.sh` | ✔ |
| Parity: `kernel/fs` compiles at rv64/a64/i386 (incl. `-Wshorten-64-to-32`) | `check_parity_claims.py` (`FS_FILE_COUNT` 26 → 27) | ✔ |
| SH8: the in-guest build's source pin 152 → 153 in all four places | `check_build_hygiene.py` check 4 | ✔ |
| Integration registry: the new case is in `ALL_CASES` and matches exactly one shard (`fs`) | `check_test_registry.py`, `run_all.sh --check-groups` | ✔ |

`test_home.sh` result on the final tree: **13/13**, run five times during
development (two runs caught real defects: the EROFS home and the empty
`$USER`).

---

## 6. Review notes for whoever lands the next phase

1. **The parser is the security boundary of this feature.** It decides which
   directory a uid gets. Every new accepted form needs a check in
   `test_home_passwd.c`, not a manual boot.
2. **The mutation lane is part of the gate.** A Makefile rule that builds the
   doctored binary and *requires* it to fail is the only thing standing
   between "the tests pass" and "the tests can pass".
3. **Ratchets move in the change that trips them.** H0 tripped three
   (`FS_FILE_COUNT`, SH8's pin, and — if the code had been written the lazy
   way — the `(uint64_t)` counter). Moving them in a follow-up commit is how
   a ratchet stops being one.
4. **`vol_of_vnode()` has bitten twice.** Any new tmpfs volume must be added
   there or its root cannot be listed. A unit test that walks every mounted
   tmpfs volume and readdirs its root would have caught both occurrences;
   that test does not exist yet and is the first candidate for H1's
   test work.
