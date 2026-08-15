# AuraLite OS — Test-Integrity and Maturity-Plan Repair

## Status: IN PROGRESS 🚧 — A0 next; A1–A6 planned

| Phase | Subject | State | Deliverable |
|---|---|---|---|
| A0 | Make the invisible failures visible | 📋 next | `patches/AUDIT_A0_registry_guard.patch` |
| A1 | Repair the two dead gates (M3, M4) | 📋 planned | `patches/AUDIT_A1_dead_gates.patch` |
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

### Phase A1 — Repair the two dead gates

**Objective:** M3 and M4 have gates that can actually run.

#### Tasks

- [ ] Rewrite `test_uaccess.sh` against the real `lib.sh` API
      (`il_init`/`il_run_qemu`/`il_assert_grep`/`il_summary`).
- [ ] Same for `test_mmap_shared.sh`.
- [ ] Record what each now proves — and what it does not.

#### Test gate

- Both cases run and produce a verdict. If the underlying feature is
  incomplete, the verdict is **red**, and that is the honest result.
- Negative control on whichever passes.

#### Deliverable

`patches/AUDIT_A1_dead_gates.patch`

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
