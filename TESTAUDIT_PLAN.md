# AuraLite OS — Test-Integrity and Maturity-Plan Repair

## Status: ✅ COMPLETE — A0–A6 delivered; A7 added (plan-drift sweep)

| Phase | Subject | State | Deliverable |
|---|---|---|---|
| A0 | Make the invisible failures visible | ✅ **done** | `patches/AUDIT_A0_registry_guard.patch` |
| A1 | Repair the two dead gates (M3, M4) | ✅ **done** | `patches/AUDIT_A1_dead_gates.patch` |
| A2 | Triage the failing orphan cases | ✅ **done** | `patches/AUDIT_A2_orphan_triage.patch` |
| A3 | Correct `MATURITY_PLAN.md` (M3, M6, M10) | ✅ **done** | `patches/AUDIT_A3_plan_corrections.patch` |
| A4 | Retire the stale `bring-up only` claim | ✅ **done** (folded into A2) | — |
| A5 | M3's real audit task: hostile-pointer sweep | ✅ **done** | `patches/AUDIT_A5_uaccess_audit.patch` |
| A6 | M4: file-backed `MAP_SHARED` + writeback | ✅ **done** | `patches/AUDIT_A6_vma.patch` |
| A7 | `FIXES_PLAN.md` said PLANNED while fully done | ✅ **done** | `patches/AUDIT_A7_fixes_plan.patch` |

Findings this plan acts on are recorded in [`MATURITY_AUDIT.md`](MATURITY_AUDIT.md).

---

## 1. Why this plan exists

`MATURITY_AUDIT.md` measured `MATURITY_PLAN.md` against the tree and found
that the problem is not mainly *unfinished work* — it is *unmeasured work*:

- Two integration gates (`test_uaccess.sh` for M3, `test_mmap_shared.sh` for
  M4) call `il_run`/`il_assert`/`$IL_SERIAL`, none of which exist. They have
  never executed.
- `run_all.sh` hardcodes 97 cases; there are 124 case files. **27 never run.**
  Five of those are the USB gates I wrote in U3–U9 and failed to register.
- A sample of 10 orphans found 4 genuinely failing and 2 broken.

So "118/118 green" describes the registered subset, not the suite. Every
maturity phase from here on would inherit that blind spot: a phase can ship
a gate, have it pass by hand, and protect nothing.

## 2. Decisions

### D1. Visibility before repair
A0 lands first and fixes no test. It makes the gap countable and makes it
impossible to reopen, exactly as `USB_PLAN.md` U0 did for the boot log. A
red CI at A0 is the correct outcome, not a regression.

### D2. Register everything, then triage
Registering an orphan that fails turns a hidden failure into a visible one.
That is progress, and the plan records the expected-red band (A0 → A2)
rather than hiding it by registering only the green cases.

### D3. A test that cannot fail is not a test
Every gate added here gets a negative control: revert the fix, watch it
redden. The `test_usb_isoc` case in the audit — which fails because the
*driver log* is stale, not the driver — is the reminder that a gate can be
red for the wrong reason too.

### D4. Fix the plan document, don't rewrite its judgement
`MATURITY_PLAN.md`'s priorities (D1/D2 there) are sound. A3 corrects
statuses that are factually wrong (M3 landed, M6 understated, M10 overtaken
by `USB_PLAN.md`) and leaves the ordering alone.

### D5. Scope: this plan does not finish M6–M14
It repairs the measurement apparatus and closes M3 and M4, the two phases
whose gates were fictional. M6–M14 stay in `MATURITY_PLAN.md`.

---

## 3. Phases

### Phase A0 — Make the invisible failures visible **(no behaviour change)**

**Objective:** the test suite reports its own coverage honestly.

#### Tasks

- [ ] Register all 27 orphan cases in `run_all.sh`.
- [ ] Add a drift guard: a case file present in `cases/` but absent from
      `ALL_CASES` is a hard failure, in the manner of
      `tools/check_usb_claims.py`. The list cannot silently fall behind again.
- [ ] Guard the reverse too: a name in `ALL_CASES` with no file.
- [ ] Fix the comment that claims the list is "discovered" when it is
      hardcoded.

#### Test gate

- The guard fails on the tree as it stands today (27 unregistered), and
  passes once they are registered — both states demonstrated.
- Deleting a name from `ALL_CASES` reddens it; adding a stray name reddens it.
- `make test-unit` still green; no kernel source touched.

#### Deliverable

`patches/AUDIT_A0_registry_guard.patch`

---

### Phase A1 — Repair the two dead gates ✅ DONE

**Objective:** M3 and M4 have gates that can actually run.

**Landed. Both phases have evidence for the first time.**

```
test_uaccess      4/4   (usertest 30/30)
test_mmap_shared  7/7   (mmapshare 4/4)
```

**M3 was not actually complete.** With the gate finally running, the
hostile-pointer battery reported **29/30**. The failing case was
`wait4(-1, 0xDEAD, WNOHANG)`.

The interesting part is *who* was wrong. The test asserted
`r == -3 /* ECHILD */`, but `ECHILD` is **10** in both `kernel/lib/errno.h`
and `lib/libc/include/errno.h`. The kernel had answered `-ECHILD`
correctly all along; the test scored a correct answer as a failure. Fixing
the constant gives 30/30 with **no kernel change**.

**A change I made and then removed.** My first reading blamed the kernel:
`wait4` only validated the status pointer when `ret > 0`, so a bad pointer
with `WNOHANG` and no ready child was never inspected. I added an up-front
`validate_user_range()`. Reverting it did **not** redden the test — the
`ECHILD` path returns before the validation could matter, so the code was
unreachable. An unreachable "hardening" that no test can distinguish is
dead weight, so it is not in this patch. The real defect was the test.

**M4 had a gate with nothing to measure.** `test_mmap_shared.sh` only ever
asked the shell to run the generic `selftest`, which does not touch
`MAP_SHARED` at all. Even had it executed, it would have proved nothing.
`/tests/mmapshare` was written for it: parent writes through a
`MAP_SHARED|MAP_ANONYMOUS` page, child reads it and writes back, parent
sees the reply — and a **`MAP_PRIVATE` control** in the same program, which
must stay copy-on-write. A mapping layer that shared everything would pass
the first half while being badly broken.

#### Test gate — met

- ✅ `test_uaccess` 4/4; `usertest` 30/30, no kernel fault, shell alive.
- ✅ `test_mmap_shared` 7/7; `mmapshare` 4/4 including the private control.
- ✅ Negative control 1: restoring the wrong `-3` constant → 29/30, red.
- ✅ Negative control 2: changing the shared mapping to `MAP_PRIVATE` →
  "child observed the parent's write" fails, 2 of 7 red.
- ✅ Registry guard still green (124 cases); `make test-unit` green.

**Both gates also had a second, quieter bug:** they sent their first shell
command after a 3-second delay, but the prompt appears several seconds into
boot — the other cases in this suite wait 6–7s. The command was swallowed,
which looks exactly like the program failing. Both now wait 8s.

#### Deliverable

`patches/AUDIT_A1_dead_gates.patch`

#### Tasks

- [ ] Rewrite `test_uaccess.sh` against the real `lib.sh` API
      (`il_init`/`il_run_qemu`/`il_assert_grep`/`il_summary`).
- [ ] Same for `test_mmap_shared.sh`.
- [ ] Record what each now proves — and what it does not.

---

### Phase A2 — Triage the failing orphans ✅ DONE

**Objective:** every registered case is green, or documented as expected-red
with a phase that closes it.

**All four were the test's fault, in two distinct ways — and neither was a
kernel bug.**

**Group 1 — asserting on banners U0 deliberately deleted.**
`test_usb_isoc` and `test_usb_cdc_acm` waited for
`[isoc] PASS: isoc full support ready` and
`[cdc-acm] PASS: CDC ACM full support ready`. Both lines were printed
unconditionally — with no isochronous transfer ever issued and with zero
CDC devices attached — which is precisely why `USB_PLAN.md` U0 replaced
them with an honest `SKIP` or a device count. The tests were asserting the
lie. They now assert the honest output.

**A4 folded in here.** `test_usb_isoc` also failed on
`[xhci] self-test: ... (bring-up only)` — stale since U3–U9 made slots,
control, bulk, interrupt and nested hubs real. U0's rule cuts both ways: a
log that *understates* disagrees with the driver just as badly as one that
overstates. The line now reads
`control/bulk/interrupt real; isoc TRBs issued, not stream-verified`.

**Group 2 — asserting against a filesystem that was never mounted.**
`test_diskfs` and `test_fat32_mkdir` exercised `/disk` and `/fat` **while
attaching no disk to QEMU at all**. The guest printed
`no AHCI disk available; /disk not mounted`, every command failed, and the
cases were red for a reason unrelated to their subject. Both now attach a
real FAT32 volume over AHCI, the way `test_ahci_large_read.sh` does. The
underlying code was fine: with a disk present, mkdir, write and read-back
all work.

**Worse than red: two of those assertions were passing for the wrong
reason.** `test_diskfs` matched `"$MARK"` and `"persist.txt"` anywhere in
the log — and both appear in the *echo of the write command itself*. They
passed while nothing was written. The mark is now required to appear
**twice** (input plus a genuine read-back), and the filename must appear
inside an actual `ls /disk` listing. `test_fat32_mkdir` had the same defect
with `testdir`.

**A library bug found on the way.** `il_assert_count()` calls
`il_pass`/`il_fail` but never incremented `IL_ASSERT_COUNT`, so every case
using it under-reported its own total — the first fixed run printed
`4/2 assertions passed`. Seven cases are affected.

#### A2-R1 — a real kernel bug the fix uncovered ✅ **FIXED**

> **Resolved in `patches/FIX_A2R1_ahci_lock.patch`.** It was never a FAT32
> bug: `drivers/ahci/ahci.c` had **no lock at all**. Every transfer goes
> through one command slot and one DMA bounce buffer *per port*, and eight
> subsystems call in (fat32, diskfs, ext2, ext4, btrfs, f2fs, buffer_cache,
> `/disk`). `fat32_lock` made FAT32 safe against FAT32, but nothing stopped
> the BSP reading `/disk` through diskfs while an AP flushed the kernel log
> to `/fat/AURALOG.TXT` — both on the **same physical port**. The second
> request overwrote the first's command header and PRDT while the first was
> still polling `PxCI`, so one request's sector landed in the other's
> buffer. That is precisely why the garbage read `AURALOG TXT`: a FAT32
> *directory* sector delivered into a diskfs read. The victim named the
> culprit and it was misread as the culprit naming itself.
>
> Fixed with a **per-port** spinlock (not global — independent disks still
> overlap) covering both the transfer and the `memcpy` out of the shared
> bounce buffer. `test_diskfs` now 4/4 across 10 consecutive runs, and
> `tests/unit/test_ahci_serialisation.c` models the race with a control
> that must corrupt.

Historical description of the symptom follows.

`test_diskfs` was **intermittently red, correctly**. About one run in three,
`cat /disk/persist.txt` returns raw FAT directory-entry bytes instead of
the file:

```
cat /disk/persist.txt
AURALOG TXT ^@^@^M^@M-X\M-X\^@^@^U^@M-X
```

The write always lands — the file grows 24 → 26 bytes and appears in
`ls` — so the defect is on the read-back or cache-flush side of the FAT32
path. The old assertion could never have caught it: it matched `$MARK`
anywhere in the log, and the echo of the write command satisfied that on
every run, pass or fail.

Left asserting the correct behaviour on purpose throughout. A retry loop
would have re-hidden precisely what this phase existed to surface — and it
would have buried a genuine SMP data-corruption bug in the block layer
under a "flaky test" label.

#### Test gate — met

- ✅ `test_fat32_mkdir` 5/5, `test_usb_isoc` 6/6, `test_usb_cdc_acm` 6/6.
- ✅ `test_diskfs` 4/4 — was an honest intermittent (3/4 when A2-R1 fired),
  now green on 10 consecutive runs since the AHCI lock landed.
- ✅ Negative control 1: removing the AHCI drive from `test_diskfs` →
  4 of 4 red, led by "/disk is actually mounted".
- ✅ Negative control 2: restoring `(bring-up only)` → `test_usb_isoc` red.
- ✅ Registry guard green (124 cases); `make test-unit` green.

#### Deliverable

`patches/AUDIT_A2_orphan_triage.patch`

---

### Phase A3 — Correct `MATURITY_PLAN.md` ✅ DONE

- [x] **M3 → complete.** The patch was applied and its files were in the
      tree while the plan still read `pending | —` with every task box
      unticked. Its gate had never run; A0 registered it and A1 repaired
      it, which is how the 29/30 → 30/30 `ECHILD` defect surfaced.
- [x] **M4 → complete (anonymous).** Also silently landed. File-backed
      `MAP_SHARED` still returns `-ENOSYS` and there is no demand-paging
      fault path — both stated rather than implied.
- [x] **M6 → credited, then scoped.** Measured: `cwnd` (14 uses),
      `ssthresh` (5), `rto_ms`, SRTT, a retransmit queue, `FIN_WAIT_1/2`
      and `tcp_listen()` all exist; `TCP_MAX_CONNS` is **16**, not the 8
      the task list assumed. Genuinely absent: dup-ACK counting, fast
      retransmit, SACK, Nagle, delayed ACK, `TIME_WAIT`/`CLOSE_WAIT`/
      `LAST_ACK`, backlog, `SO_REUSEADDR`, keepalive.
- [x] **M10 → superseded** by `USB_PLAN.md`, with the three genuinely open
      items carried forward and the untestable EHCI split case recorded in
      `docs/usb.md` instead of left as a task nobody can close.

**The corrections are not the point; the guard is.** A status line is
prose, and prose does not fail a build — which is exactly how three of them
drifted. `tools/check_maturity_claims.py` now ties **14** claims to the
source, checking the absences in reverse as well, and runs in CI beside the
USB and registry guards.

#### Test gate — met

- ✅ 14 claims verified against the tree.
- ✅ Negative control 1: putting M3 back to `pending` → "M3's status row
  says 'complete'" fails.
- ✅ Negative control 2: introducing `TCP_TIME_WAIT` → "M6 is still
  pending: no TIME_WAIT state yet" fails. The guard catches drift in
  **both** directions, not just staleness.
- ✅ Registry guard 124/124, USB claim guard 12/12, build green.

#### Deliverable

`patches/AUDIT_A3_plan_corrections.patch`

---

### Phase A4 — Retire the stale `bring-up only` claim ✅ DONE (folded into A2)

`xhci_self_test()` printed `(bring-up only)` long after U3–U9 made the
driver real, and `test_usb_isoc` was failing on that line rather than on
anything isochronous. Fixing the test without fixing the log would have
been fixing the symptom, so both landed together in A2.

---

### Phase A5 — M3's real audit task ✅ DONE

**The sweep found nothing to fix — and that is the reportable result.**
All 3,257 lines across `syscall.c`, `gui_syscalls.c`, `gpu_syscalls.c` and
`socket.c` route user pointers through `copy_from_user`, `copy_to_user`,
`copy_string_from_user` or `validate_user_range`. The uaccess path was in
better shape than the plan assumed.

**So the deliverable is the sweep itself, not a fix.** A grep-audit is the
kind of thing done once, declared finished, and never repeated;
`tools/audit_user_pointers.py` does it on every CI run. A new syscall that
dereferences an argument without a guard now fails the build.

**Four defects in my own checker, each found by a negative control.** This
is worth recording, because a checker that reports "clean" is worthless
unless it is known to be capable of reporting dirty:

| # | Defect | Consequence |
|---|---|---|
| 1 | Flagged the declaration line itself | 13 findings, all false — the guard is on the *next* line |
| 2 | `*ptr = x` skipped as a comment continuation | Blind to the most obvious unsafe shape there is |
| 3 | Guards matched by bare name, file-wide | A guard in `syscall_vfs_write()` excused a dereference 800 lines away |
| 4 | `memcpy` matched only its first argument | `memcpy(tmp, user_buf, 8)` reported the *kernel* buffer |

Defect 3 is the instructive one: `user_buf` is a parameter near the top of
the file **and** a local in a case arm much later. Name-based tracking made
the checker structurally unable to see the bug its own control had planted.
Symbols are now scoped by brace depth.

#### Test gate — met

- ✅ Clean tree: no unchecked dereferences across 4 files.
- ✅ Negative control 1: `*ut = now` unguarded → caught at `syscall.c:1757`.
- ✅ Negative control 2: `memcpy(tmp, user_buf, 8)` → caught at `:1181`.
- ✅ Negative control 3: `((char*)user_arg)[0]` behind a cast → caught at
  `:1484`.
- ✅ `test_uaccess` still 4/4 (usertest 30/30); all four guards green;
  build green. No kernel source touched.

**Not attempted:** the "page unmapped between the length check and the
copy" case from M3's task list. `validate_user_range()` is documented in
`usercopy.h` as *optimistic* — the real protection is the #PF fixup in
`copy_*_user`, and provoking that race deterministically needs a second
thread unmapping mid-copy. Stated rather than quietly dropped.

#### Deliverable

`patches/AUDIT_A5_uaccess_audit.patch`

---

### Phase A6 — M4: file-backed `MAP_SHARED` ✅ DONE

**Demand paging already existed.** `handle_user_page_fault()` in
`kernel/mm/vma.c` resolves lazy VMAs — anonymous, file-backed and shmem —
and `syscall_mmap()` allocates nothing up front. Half of what this phase
was scoped to build was in the tree. Only file-backed `MAP_SHARED` was
missing, and it returned `-ENOSYS` "pending page cache writeback from M9".

**The blocker was one line that was never written.** The page cache had a
`dirty` field, and `page_cache_flush()` honoured it — but nothing in the
entire tree ever set it. `dirty` was assigned `0` in two places and tested
in one. Writeback was a no-op *by construction*, so M9 was not actually a
prerequisite: the missing piece was the dirty bit, not the cache.

Four changes, all small:

| Change | File |
|---|---|
| `page_cache_mark_dirty()` + `page_cache_flush_range()` | `kernel/mm/page_cache.c/.h` |
| Mark dirty on a write fault; map read-only on a *read* fault so the first store still traps | `kernel/mm/vma.c` |
| Drop the `-ENOSYS`; add `msync(2)`; flush on `munmap()` | `kernel/arch/x86_64/syscall.c` |
| `msync()` + `MS_*` flags | `lib/libc` |

**The read-only-on-read-fault detail is the load-bearing one.** The VMA is
`PROT_WRITE`, so the fault handler used to map the page writable on the
very first (read) fault — after which the first store never trapped and the
kernel never learned the page had changed. Mapping read-only until a write
actually faults costs one extra fault per page, once.

**`munmap()` no longer frees page-cache frames.** They are shared with the
cache and with every other mapping of that file; returning them to the PMM
would hand out live memory.

#### Test gate — met

- ✅ `/tests/mmapfile` **6/6**, `test_mmap_file` **8/8** (case 125).
- ✅ **Negative control:** removing the `page_cache_mark_dirty()` call →
  **4 of 8 assertions red**, including the `MAP_PRIVATE` control.
- ✅ No regression: `test_mmap_shared` 7/7, `test_uaccess` 4/4,
  `test_diskfs`, `make test-unit`, all four guards, `make iso`.

**One assertion was deliberately withdrawn.** The first draft checked that
a plain `read()` saw the store *before* `msync()`, and it failed —
correctly. `vfs_read()` calls the filesystem read op directly and does not
consult the page cache, so a dirty page is invisible to `read()` until it
is written back. That is ordinary write-back behaviour, and POSIX does not
promise coherence until `msync()`. Asserting it would have been asserting a
guarantee the system never made.

#### Deliverable

`patches/AUDIT_A6_vma.patch`

---

## 4. Order and rationale

A0 first, and alone, because every later measurement depends on the suite
telling the truth about what it ran. A1 next because M3 and M4 currently
have *no* evidence at all. A2 clears the resulting red band. A3 and A4 are
documentation and log honesty — cheap, and they stop the next reader
repeating finished work. A5 and A6 are the actual engineering, deliberately
last, because doing them first would mean writing code against a test suite
that cannot report on it.


---

### Phase A7 — `FIXES_PLAN.md` claimed to be unstarted ✅ DONE

**The same drift A3 found, one document over — and worse.**
`FIXES_PLAN.md` led with `Status: PLANNED 📋 (phases R0–R8)` while:

- all **33** of its own task checkboxes were ticked,
- every repair was in the source — `idt_set_ist(8, 1)` in `tss.c`, the
  seeded `__stack_chk_guard`, `__errno_location`, `.init_array`,
  `SIGSTOP`/`SIGCONT`, specific socket errno, a second keymap,
- and all **seven** of its test gates existed **and were registered** in
  `run_all.sh`.

A reader taking the header at face value would conclude the kernel still
had two *critical* unfixed defects — an unarmed IST and an undiagnosable
SMP stack-protector trip — both of which were repaired long ago.

**No `patches/FIX_R*.patch` exists**, so the work was merged without the
per-phase patches the plan specified. That is why the header was never
revisited: nothing in the process forced it.

**The guard is the deliverable.** `tools/check_fixes_claims.py` ties all
nine phases to the source, and additionally requires each promised gate to
be **registered** in `run_all.sh` — present-but-unregistered was A0's
finding and would otherwise pass here.

#### Test gate — met

- ✅ 19 claims verified against the tree.
- ✅ It **found the live defect on first run**: 18 passed, the status line
  failed.
- ✅ Negative control 1: restoring `PLANNED` → status-line check fails.
- ✅ Negative control 2: disarming `idt_set_ist(8, 1)` → R1 fails.
- ✅ Negative control 3: unregistering `test_ist_double_fault` from
  `run_all.sh` → the gate check fails.
- ✅ Five guards green; `make iso` green.

**One defect in my own guard.** The first version searched the whole
document for `PLANNED`, so the corrected header — which *explains* what it
used to say — kept failing it. A guard that cannot be satisfied by fixing
the thing it complains about is a broken guard; it now reads only the
`## Status:` line.

#### Deliverable

`patches/AUDIT_A7_fixes_plan.patch`
