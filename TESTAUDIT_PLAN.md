# AuraLite OS — Test-Integrity and Maturity-Plan Repair

## Status: IN PROGRESS 🚧 — A0, A1 complete; A2–A6 planned

| Phase | Subject | State | Deliverable |
|---|---|---|---|
| A0 | Make the invisible failures visible | ✅ **done** | `patches/AUDIT_A0_registry_guard.patch` |
| A1 | Repair the two dead gates (M3, M4) | ✅ **done** | `patches/AUDIT_A1_dead_gates.patch` |
| A2 | Triage the failing orphan cases | 📋 planned | `patches/AUDIT_A2_orphan_triage.patch` |
| A3 | Correct `MATURITY_PLAN.md` (M3, M6, M10) | 📋 planned | `patches/AUDIT_A3_plan_corrections.patch` |
| A4 | Retire the stale `bring-up only` claim | 📋 planned | `patches/AUDIT_A4_xhci_selftest.patch` |
| A5 | M3's real audit task: hostile-pointer sweep | 📋 planned | `patches/AUDIT_A5_uaccess_audit.patch` |
| A6 | M4: demand paging and shared VMAs | 📋 planned | `patches/AUDIT_A6_vma.patch` |

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

### Phase A2 — Triage the failing orphans

**Objective:** every registered case is green, or documented as expected-red
with a phase that closes it.

Known red at audit time: `test_diskfs` (1/3), `test_fat32_mkdir` (2/4),
`test_usb_isoc` (2/6), `test_usb_cdc_acm` (1/6). Per case, decide whether
the test or the code is wrong — the U0 discipline.

#### Deliverable

`patches/AUDIT_A2_orphan_triage.patch`

---

### Phase A3 — Correct `MATURITY_PLAN.md`

- [ ] M3 → complete (or list what its audit task genuinely leaves open).
- [ ] M6 → credit `cwnd`/`ssthresh`/`rto_ms`/SRTT; scope to fast retransmit,
      SACK, Nagle, delayed ACK, backlog, `TIME_WAIT`.
- [ ] M10 → retire in favour of `USB_PLAN.md`, keeping only USB Audio isoc
      end-to-end, writable `/usb` automount, and per-controller MSC.

#### Deliverable

`patches/AUDIT_A3_plan_corrections.patch`

---

### Phase A4 — Retire the stale `bring-up only` claim

`xhci_self_test()` still prints `(bring-up only)` after U3–U9 made slots,
control, bulk, interrupt and nested hubs real. `test_usb_isoc` fails on it.
Understating is the same defect as overstating: the log disagrees with the
driver.

#### Deliverable

`patches/AUDIT_A4_xhci_selftest.patch`

---

### Phase A5 — M3's real audit task

The uaccess primitives exist; the *sweep* the phase asked for does not.
Grep-audit every user-pointer dereference in `syscall.c`, `socket.c`,
`gui_syscalls.c`, `gpu_syscalls.c`; route each through the safe primitives;
add the hostile-pointer battery (unmapped, wrap-around, kernel-space,
unmapped-mid-copy).

#### Deliverable

`patches/AUDIT_A5_uaccess_audit.patch`

---

### Phase A6 — M4: demand paging and shared VMAs

Only after A0–A2 give it a gate that runs.

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
