# AuraLite OS — RESIDUE3 Plan (close every remaining row: the handed-off class, the seven boxes, and the drift class itself)

## Status: IN PROGRESS — C0 (the rig), C1 (the reconciliation) and C2 (the kernel cluster) landed 2026-11-03; C3 (i386 graphics) landed 2026-11-04 with its compositor half carried to C3b; C4 (MSI/MSI-X on virtio) landed 2026-11-04; C5 (isochronous streaming) landed 2026-11-04 with its HID/EHCI survivors carried to C5b, which landed 2026-11-04 as well; C6 (Bluetooth/Wi-Fi) landed 2026-11-04 with RES-61's metal half; C7 (TCP window scaling) landed 2026-11-04 with its NVMe half carried to C7b, which landed the same day as RES-46's storage successor; C8 (the Win32 dynamic-scale loader, the live FAIL-CLEAN and static-only receipts, DOOM D7) landed 2026-11-05 and closed RES-57/58/59: 11/12 (C3b's compositor half and C9/C10 remain). Every count in this file was measured on `26cb7fe`; every receipt below was re-measured on its own phase branch

| Phase | State | Deliverable |
|-------|-------|-------------|
| **All of it** — C0 → C8 in one file, from the commit this plan was measured on | ✅ cut 2026-11-05 | `patches/RESIDUE3_ALL_C0-C8.patch` — every phase patch below is inside it, cut against `AlexanderNyr/AuraLite-OS@26cb7fe` and replayed onto a fresh clone with no differences (the file count and diffstat are in the patch's own header, which is the only place they cannot go stale). |
| C0 — the rig: the plan-table / orphan-debt checker | ✅ done (2026-11-03) | `patches/RESIDUE3_C0_rig.patch` |
| C1 — the reconciliation: flip what already landed, repair what drifted | ✅ done (2026-11-03) | `patches/RESIDUE3_C1_reconcile.patch` |
| C2 — kernel: ABI alignment, SMP-safety, large pages, the real process table | ✅ done (2026-11-03) | `patches/RESIDUE3_C2_kernel.patch` |
| C3 — i386 graphics: VBE first, then the compositor (RES-12 → RES-11) | ✅ done (2026-11-04) — RES-12 + the i386 fb console; the compositor half is C3b | `patches/RESIDUE3_C3_i386gfx.patch` |
| C3b — the i386 compositor: `wm.c`/`gui.c` on 32 bits | ⬜ planned — blocked on the i386 process/scheduler surface (blocker list in §C3) | `patches/RESIDUE3_C3b_compositor.patch` |
| C4 — MSI/MSI-X on virtio (RES-36) | ✅ done (2026-11-04) | `patches/RESIDUE3_C4_msix.patch` |
| C5 — USB: isochronous streaming and HID breadth (RES-38 survivors) | ✅ done (2026-11-04) — isoc streaming + the short-CSW line; HID breadth and the EHCI split series are C5b | `patches/RESIDUE3_C5_usb.patch` |
| C5b — USB survivors: HID report IDs, EHCI split transactions, the short-CSW case (RES-62) | ✅ done (2026-11-04) — report-ID parser + kernel selftest, PSPD decode + host-tested QH caps, CSW validator; the TT *device* case is impossible on QEMU and says so | `patches/RESIDUE3_C5b_usbhid.patch` |
| C6 — Bluetooth and Wi-Fi: the hardware backends (RES-39 survivors) | ✅ done (2026-11-04) — HCI transport injected + host-tested, Wi-Fi over the virtual AP reaches the IP stack; RES-61 keeps the metal | `patches/RESIDUE3_C6_wireless.patch` |
| C7 — network: window scaling (RES-52) | ✅ done (2026-11-04) — RES-52 flips: wscale negotiated both ways, a megabyte with 395660 bytes in flight, a window-clamped peer as the control | `patches/RESIDUE3_C7_net.patch` |
| C7b — NVMe: a queue-pair driver over PCIe (RES-46's storage successor) | ✅ done (2026-11-04) — polled queue-pair driver, controller identity from Identify, ext2 mounted off the namespace, host-verified round trip | `patches/RESIDUE3_C7b_nvme.patch` |
| C8 — Win32: the dynamic-scale loader, live FAIL-CLEAN receipts, DOOM D7 (RES-57/58/59) | ✅ done (2026-11-05) — one streaming relocation path (90932 fixups, no cap), 256/128/32 module table, a gap report that NAMES symbols, 35 FAIL-CLEAN + 820 static-only pairs measured live, DOOM D7 measured at 52 named gaps | `patches/RESIDUE3_C8_receipts.patch` (receipt half, 20 files, replays byte-for-byte) + `patches/RESIDUE3_C8_receipts.manifest` (the loader half pinned by hash — no pre-image exists in this environment) |
| C9 — GL: the GLSL AST → TGSI back end (RES-54) | ⬜ planned | `patches/RESIDUE3_C9_tgsi.patch` |
| C10 — close-out: the arithmetic, the honest zero, the parked M class | ⬜ planned | `patches/RESIDUE3_C10_close.patch` |

## 1. Where this plan comes from

Two instructions, one audit. The user's instruction after the RESIDUE2
close-out: **close every remaining item**, not one subsystem. The audit
re-ran the tree the way R12's and RESIDUE2's sweeps did, but this time
the question was neither "which claims are stale" (R12) nor "which
debts exist only as prose" (RESIDUE2) but **"which debts are recorded
and still have no owner"**.

Measured on `26cb7fe` (2026-11-03), this tree:

| Source | Count | Composition |
|---|---|---|
| Ledger rows, non-DONE | **20** | OPEN 1 (RES-54) · HANDED-OFF 10 · PENDING-USER 6 · RE-AFFIRMED 4 |
| `TODO.md` unchecked boxes | **7** | 6 tagged `(RESIDUE2 T1)`, 1 tagged `(RESIDUE2 T6)` |
| `docs/status.md` 🚧 rows | **4** | OpenGL/libgl · Bluetooth HCI · Wi-Fi MAC · the arch.h migration ratchet (the harvester's 6th line is the legend itself) |
| Plan-level open work | **3** | DOOM `D7` (Win32 DOOM) · SELFHOST `SH9` (cross-arch in-guest toolchains) · the W32 live-matrix PARTIAL gaps |
| **Drift defects (new to this audit)** | **5 classes** | below — the reason C0 exists |

### 1.1 The five drift classes this audit found (each with its receipt)

**D1 — three ledger rows whose own phase claims closure, and the row
never flipped.** `RESIDUE2_PLAN.md` T6 §"Definition of done" reads
"RES-38 and RES-39 close", and T6's Result is `DONE`; T7's Result reads
"all four RES-47 boxes closed with receipts". The ledger still carries
`RES-38 HANDED-OFF@R12`, `RES-39 HANDED-OFF@R12`, `RES-47
HANDED-OFF@R12`. This is the exact class RES-45 documented for TODO.md
("the file claimed something the tree had already closed") — one layer
down, in the ledger itself, where nothing checks it.

**D2 — plan tables that disagree with their own sections.**
`W32APP_PLAN.md`'s table shows `⬜ planned` for **W32A-1, W32A-2,
W32A-5, W32A-12, W32A-18** while its own Status header says
"W32A-0..W32A-17 are done" and `docs/status.md` claims W32A-0..W32A-18
landed. `RESIDUE2_PLAN.md`'s table shows `T7 … ⬜ planned` while §T7
reads `**Status: ✅ COMPLETE**` with a filled Result. `check_w32app_claims.py`
does not catch this because it validates the *status document's* matrix,
not the plan's own phase table.

**D3 — every open TODO box is tagged with a phase that is already ✅.**
Six carry `(RESIDUE2 T1)`, one carries `(RESIDUE2 T6)`; T1 and T6 are
both `✅ COMPLETE`. RESIDUE2 §2's contract only required *"a phase is
tagged by ≥ 1 box"* — nobody checked the implication "a ✅ phase owns no
open box". The seven boxes are therefore orphaned by construction: they
are real work (large pages, waitpid, SMP-safety, isoc streaming, POSIX
depth) with a tag that points at a closed plan. Worse, re-measuring
them (§3 C2) shows **four of the six kernel boxes name mechanisms that
already exist in the tree** — atomics on the OFD refcount, an
`O_APPEND` lock, an atomic `sig_pending`, 2 MiB leaves — so the debt
is partly *unproven* rather than *unimplemented*, and nobody could see
which without re-deriving it.

**D4 — a measured opener that went stale *in the row's own favour*.**
RES-39 says "the tree has ZERO [registered wireless NIC]" and cites
`wifi.c 370 lines`. Measured now: `wifi.c` 333 + `wifi_virt.c` 136
(landed at T6, per its own header: "the RESIDUE2 T6 reference backend"),
auto-registered at `wifi.c:288` when no driver is present — and the boot
log of a plain `make iso` already prints `[wifi] PASS: virtual AP: scan
found 'AuraVirt' ch6`, auth/AID and 78 data bytes. The Wi-Fi half of
RES-39 is closed by receipt and the row says the opposite.

**D5 — stale line counts in the ledger's openers.** RES-11 cites the
portable GUI surface as "3198 lines (gui.c 2461 + wm.c 530 + fb.c 207)";
measured now: **3151 + 530 + 408 = 4089**. RES-38 cites "xhci 1995
lines" and `bt.c 215`; measured now: **2093** and **193**. An opener is
supposed to be a measurement other people can re-run; these cannot be.

None of the five is a code defect. All five are the same defect: **a
status that nothing re-derives from the tree.** C0 exists to make that
class impossible, and C1 is the one-time repair of what it finds.

### 1.2 What is genuinely open (the work this plan schedules)

| Ledger row | Class | What is actually left |
|---|---|---|
| RES-54 | S/OPEN | GLSL AST → TGSI retarget (GL2's D7 hand-off): a real back end, not the canned dword triangle |
| RES-11 | S | i386 has no compositor: it references none of the 4089-line portable surface |
| RES-12 | S | i386 has no linear framebuffer at all: zero VBE code in any loader; the console is VGA text at 0xB8000 |
| RES-36 | S | `virtio_pci.c` walks only cap id `0x09`; zero mentions of `0x05`/`0x11` anywhere — no MSI/MSI-X |
| RES-38 | S | its five named boxes measured STALE at T6; the survivor is frame-accurate isochronous streaming (`test_usb_isoc` is a framework SKIP) |
| RES-39 | S | Wi-Fi closed by receipt (D4); Bluetooth has protocol + `usb_core` transport but has never talked to a controller QEMU can attach |
| RES-52 | S | `TCP_WINDOW` is 64240 and `grep wscale\|shift_cnt\|TCPOPT kernel/net/tcp.*` is 0 hits |
| RES-57 | S | 1694 `static-only` symbols in the W32 app ledgers are bound but never called at runtime |
| RES-58 | S | 35 FAIL-CLEAN functions assert an honest failure nobody has watched return in-guest |
| RES-59 | S | Audacity Outcome B: 90932 relocations vs a 16384-entry buffer, a 117-DLL graph, 19-module/419-symbol gap |

## 2. The coverage contract

| Rule | Enforced by |
|---|---|
| Every non-DONE ledger row is either named by a phase of a **live** plan, or is class `M` (metal, needs the user's silicon) / `N` (re-affirmed non-goal) | C0 checker |
| Every `- [ ]` box in TODO.md carries a tag naming a phase that is **not** ✅ | C0 checker |
| A phase marked ✅ owns no open box and no OPEN ledger row, unless this file's §4 names it | C0 checker |
| Every `docs/plans/*_PLAN.md` phase-table row agrees with that plan's per-phase `**Status:` line and Result | C0 checker |
| Every measured opener (a line count, a grep-zero, a file size) in the ledger re-derives from the tree, or the row is edited | C0 checker (`--openers` mode: re-runs the greps the rows quote) |
| Marker counts ratchet against `tools/residue_baseline.txt`; the ledger arithmetic pins (`LEDGER_ROWS`, `CLASS_PIN`) move in the same commit as the rows | the existing residue ratchet |
| A phase flips to ✅ only with its Result filled and its gate green in the same commit | the GL2 L7 convention |

Closing a ledger row means: the row's status changes with a receipt in
the row itself, the phase Result cites the receipt, and — when the
closure narrows — the survivor is a **new row or a box**, never a
sentence.

## 3. Phases

### C0 — the rig: the plan-table / orphan-debt checker

**Status:** ✅ COMPLETE

**Objective:** make the drift class of §1.1 impossible. Seven checkers
already pin claims to artefacts; none of them reads the *phase tables*,
the *box tags*, or re-derives the *openers*. This one does all three.

**Design:** `tools/check_plan_tables.py` (`--check`, `--openers`,
`--selftest`) + `tests/unit/test_plan_tables.sh`, wired into
`make test-unit` beside the GL2/RESIDUE2 wrappers.

- [ ] Rule set as in §2, each violation printed with file:line and the
      two disagreeing values.
- [ ] `--selftest` runs against a doctored tree (a planted stale table
      row, a planted orphan box, a planted missing successor) and must
      fail there — a checker that never fails checks nothing.
- [ ] **The live negative control:** at birth, on `26cb7fe`, the
      checker must report exactly the twelve violations this audit
      found: **16 items in three classes** — D1 ×3 (RES-38/39/47),
      D2 ×6 (five W32APP rows + the RESIDUE2 T7 row), D3 ×7 boxes —
      and C1 turns it green. The birth report is pasted into this
      file's C1 Result.
- [ ] `tools/residue_baseline.txt` gains the `RESIDUE3_PLAN.md <n>` row
      in the same commit (this file ships with `<n>` markers; see C10).

**Definition of done:** `make test-unit` runs the wrapper; deleting a
tag from a TODO box, un-flipping a plan-table row, or closing a plan
phase while its box is open each turn the build red.

**Test gate:** selftest 8/8; live tree matches the 16-line pin;
`--openers` re-derives the D5 numbers.

**Result (2026-11-03):** DONE. `tools/check_plan_tables.py` (four rules,
`--check` / `--report` / `--openers` / `--bless` / `--selftest`) +
`tests/unit/test_plan_tables.sh` + `tools/plan_tables_baseline.txt`, the
wiring in `make test-unit` (`make -n` shows the wrapper as line 818 of
the recipe). Three things the landing proved that the audit could not:

1. **The checker found debt in the plan that added it.** Its first live
   run flagged `r1` three times on RES-57/58/59 — C8 named the rows in
   prose but outside its own phase section, so the phase did not own
   them. Fixed in this same commit (C8 now lists the three rows as
   owned); without the fix the pin would have been 19, not 16.
2. **Two false-positive classes had to be removed before the pin was
   honest**, and both are now regression-tested: a plan's *dependency*
   table also begins rows with a phase id (`| W32A-8 | COMCTL32: needs
   … |`), and a Result may name a row it does not claim to close
   (`T9: "ledger OPEN 6 → 1 (RES-54)"`). Rules `r4` and `r3` were
   narrowed accordingly — a checker whose first act is a false positive
   gets switched off.
3. **`--openers` independently re-derived D5**: RES-11 `gui.c` 2461→3151
   and `fb.c` 207→408, RES-39 `bt.c` 215→193 and `wifi.c` 370→333 — four
   of the audit's five stale measurements, without being told which rows
   to look at. The fifth (`xhci 1995`) quotes a bare basename, which the
   mode deliberately does not guess at; that one stays a human fix in C1
   and the mode says so in its docstring.

**Live negative controls, run on this tree after the landing** (each one
reverted immediately; the pin's answer is the receipt):

| Planted change | Checker |
|---|---|
| `W32APP_PLAN.md` W32A-1 row `⬜ planned` → `✅ done` (a pin the tree no longer needs) | **FAIL** — `FIXED r4:W32APP_PLAN.md:W32A-1 (move the pin in the same commit)` |
| a new TODO box tagged `(RESIDUE2 T1)` | **FAIL** — `NEW r2:TODO.md:a brand new debt tagged with a finished phase.` |
| the tag stripped from the ABI-alignment box | **FAIL** — `FIXED r2:TODO.md:IRQ/syscall entry stubs …` |
| nothing (restored tree) | **OK** — 16 pinned violations match the tree |

So the rig fails in both directions a ratchet must: new debt is red, and
debt that silently disappears without the pin moving is red as well.

---

### C1 — the reconciliation: flip what already landed, repair what drifted

**Status:** ✅ COMPLETE

**Objective:** pay the audit's own findings before writing new code. No
feature work in this phase; every change is a status that now matches a
receipt that already exists.

**Measured at C0 birth** (pasted verbatim from the rig): r3 ×3, r2 ×7,
r4 ×6 — the sixteen the audit named.

- [x] **RES-38 → DONE@T6**, survivor narrows to frame-accurate
      isochronous streaming, which becomes RES-60 (class W — QEMU has
      `usb-audio` + `-audiodev`, so it is provable) and stays the C5
      target. Receipt quoted from T6's Result (five boxes measured
      stale, 13-gate USB sweep re-run).
- [x] **RES-39 → DONE@T6**, survivor narrows to the Bluetooth
      controller attach, which becomes RES-61 (class M for real
      silicon, but C6 keeps a QEMU-provable lane) — and the Wi-Fi half
      cites the printed `[wifi] PASS: virtual AP …` receipt rather than
      "the tree has ZERO".
- [x] **RES-47 → DONE@T7** (its four boxes closed with receipts at T7:
      owner gates, clipboard ACL, settings, `gclip`), with the named
      survivors restated as boxes if any are still open.
- [x] **W32APP table:** five rows `⬜ planned` → `✅ done` (W32A-1/2/5/12/18),
      matching the plan's own Status header and `docs/status.md`.
- [x] **RESIDUE2 table:** T7 row `⬜ planned` → `✅ done (2026-09-06)`.
- [x] **Re-tag the seven TODO boxes** `(RESIDUE2 T1/T6)` → `(RESIDUE3 C2)`
      (six: ABI alignment, SMP-safety, POSIX depth, large pages, zombie
      reaping, process table) and `(RESIDUE3 C5)` (isochronous streaming).
- [x] **Re-derive the stale openers** (D5): RES-11's 3198 → 4089 and
      RES-38/39's line counts, or drop the numbers and keep the grep.
- [x] **status.md 🚧 rows:** each of the four gets a row-level verdict —
      OpenGL/libgl flips to 🧪 with GL2's receipts or keeps 🚧 with the
      named remainder; Bluetooth/Wi-Fi keep 🚧 and cite RES-61; the
      arch.h ratchet row either names its remaining file list or flips.
- [x] Ledger arithmetic pins move in the same commit: rows 59 → **61**
      (RES-60/61), `CLASS_PIN` W 35→36, M 7→8; `check_residue_claims.py`
      updated with the same numbers.

**Definition of done:** C0's checker green; `check_residue_claims.py`
green with the new pins; `git diff` touches no `.c`/`.h` file except
`docs/status.md`'s rows.

**Test gate:** C0 green; RESIDUE2's own checker still green (its pins
read the ledger, which this phase edits — the edit is the proof the two
checkers agree).

**Result (2026-11-03):** DONE, and the pin is **empty**: the rig reports
`0 live violation(s)` and `--openers` reports `0 stale opener(s)`. What
moved, and what had to move with it:

| Change | Receipt |
|---|---|
| RES-38 → `DONE@T6`, survivor narrowed to **RES-60** (W, OPEN) | row carries T6's 13-gate sweep + the stale-stub measurement; RES-60's opener is `test_usb_isoc`'s own SKIP line |
| RES-39 → `DONE@T6`, survivor narrowed to **RES-61** (M) | row quotes the boot line `[wifi] PASS: virtual AP: scan found 'AuraVirt' ch6` re-run 2026-11-03; **D4's stale sentence removed** |
| RES-47 → `DONE@T7` | row keeps its measured opener and cites T7's four-box Result |
| W32APP table ×5, RESIDUE2 T7 row | all six now agree with their own sections' status |
| seven TODO boxes re-tagged | six `(RESIDUE2 T1)` → `(RESIDUE3 C2)`, one `(RESIDUE2 T6)` → `(RESIDUE3 C5)`; the seven struck-through history entries keep their original tags — history stays readable |
| D5's five stale measurements | `--openers` proves four (`gui.c` 2461→3151, `fb.c` 207→408, `bt.c` 215→193, `wifi.c` 370→333); RES-11's composite 3198→4089 and RES-38's `xhci` 1995→2093 were corrected by hand |
| `status.md` four 🚧 rows | OpenGL flips 🚧→🧪 (both plan series complete; RES-54 named as the one remainder); Bluetooth and Wi-Fi keep 🚧 but now say what is actually missing (a controller, not a protocol) and cite RES-61; the arch ratchet row names its measured pin (`BASELINE_X64_INCLUDES = 69`) instead of a vague sentence |

Two contracts had to move with the debt, and both moved in this commit:

1. **`tools/check_residue2_claims.py`** was pinned to `(RESIDUE2 T#)` tags
   and to "every OPEN row is named in *this* plan". Re-tagging seven boxes
   and opening RES-60 in the successor breaks both *by design*, so the
   checker now accepts `(RESIDUE3 C#)` too, requires a C-tag to name a
   phase the successor actually has, allows a RESIDUE2 phase to stand at
   zero boxes only when the successor records the hand-off (it does —
   §1.1 D3 carries the literal tags), and accepts an OPEN row owned by
   the successor. Its own selftest still passes: 22 claims, doctored tree
   detected.
2. **`tools/check_residue_claims.py`** had to move three pins and one
   claim shape: `LEDGER_ROWS` 59 → **61**, `CLASS_PIN` W 35 → **36**,
   M 7 → **8** (S stays 13), and the R12 "ledger is terminal" claim now
   counts **rows** instead of the substring `HANDED-OFF@R12` — the
   arithmetic prose below the table also contains that string, so the old
   count conflated rows with prose. Rows at `HANDED-OFF@R12`: 7 → **3**
   (RES-11, RES-12, RES-36), all owned here by C3/C4.

Not-slack worth naming: the phase also **repaired two numbers in its own
ledger arithmetic heading** ("Rows: 56 … S 10" had been wrong since Y7);
the heading now reads 61 / W 36 · M 8 · N 4 · S 13, which is what the
checker pins. And the marker ratchet moved exactly once — `status-wip
6 → 5` for the OpenGL flip — after the note was reworded so that it did
not itself contain the WIP glyph (a row that says "this used to be 🚧"
is still a row containing 🚧; that would have pinned phantom debt).

---

### C2 — kernel: ABI alignment, SMP-safety, large pages, the real process table

**Status:** ✅ COMPLETE

**Objective:** the six kernel boxes — the deepest, least glamorous
cluster, and the one every demo hides least.

Opening measurement (this audit, `26cb7fe`) — and it changes what the
phase is: **four of the six boxes name mechanisms that already exist**,
so most of C2 is proof, not implementation. That is D3's substance, not
a footnote:

| Box | What the tree actually has |
|---|---|
| 16-byte C-ABI alignment | **real gap**: `grep -c "align 16" isr_stubs.asm` = **0** |
| SMP-safety sweep (atomic OFD refcounts, per-vnode `O_APPEND` lock, atomic `sig_pending`, per-CPU SYSCALL state) | **4 of 4 mechanisms present**: `__sync_sub_and_fetch`/`__sync_add_and_fetch` on the OFD refcount (`vfs.c:306/318`), `vn_append_lock()` (`vfs.c:944`, used `975`), `__sync_and_and_fetch` on `sig_pending` (`signal.c:91`, labelled "RESIDUE2 T1 (SMP sweep)"), per-CPU SYSCALL state (RES-02/T8) — what is missing is a **test that can fail** |
| Large pages for kernel mappings | **2 MiB leaves already exist**: both loaders map with `PTE_PS` (`efi_paging.c:88/100–102`, `paging.inc:33`) and the kernel splits them on demand (`split_huge_page()`, `paging.c:268`); the gap is *choosing* large pages for kernel allocations and accounting for them |
| Per-PML4 refcounting → all zombies reaped | partially there; the reaper names the subset it frees |
| Real parent/child table + precise `waitpid` | `parent_pid` and a zombie matcher exist (`thread.h:335`); precision is the gap |
| POSIX depth for `fork`/`execve`/`wait4` | unknown until measured — the READ step decides |

- [x] **16-byte C-ABI alignment at every entry stub** — and the opening
      measurement was the wrong metric, which is this phase's lesson: `align
      16` aligns the CODE ADDRESS in NASM, not the STACK, so a count of 0 said
      nothing about the invariant (and NUM_NEW would have been satisfied by a
      file full of hot-loop padding). What matters is RSP % 16 == 0 at the
      `call`, which is a property of the PATH. Measured: **7 call sites reach C
      from assembly, 3 of them were genuinely misaligned or unproven** —
      `boot.asm`'s `kmain` (the loader's stack was assumed aligned),
      `boot32.asm`'s `kmain32` (a `push` argument made it 12 mod 16) and
      `isr_stubs32.asm`'s `isr_dispatch32` (the i386 common stub never got the
      dynamic realign its 64-bit twin has had since RESIDUE2 T1).
      Fixed: an `and rsp, -16` on the 64-bit boot stack, an aligned argument
      slot on the 32-bit one, and the 64-bit realign pattern ported to
      `isr_common` (32-bit) with EBP holding the frame chain across it.
      `syscall_entry.asm` was already correct — its 7 pushes + 1 argument slot
      restore the phase — but the guarantee came from `set_syscall_stack()`,
      so it now carries an `@abi-anchor:` marker AND an `ASSERT((top & 0xF) == 0)`
      at the place the value is published. Proof, both halves:
      `tools/check_abi_align.py` models the phase through push/pop/sub/add/and
      for all 14 assembly files (`[abi] 7 call site(s) reach C, 0 violation(s)`)
      with a selftest that catches three injected misalignments, and
      `kernel/arch/x86_64/abi_probe.c` executes `fxsave` + `movaps` on a
      16-aligned local **inside a real IRQ and a real syscall** — the canonical
      `#GP`-on-misalignment probe, printed as a boot receipt.
- [x] **SMP-safety sweep, as a proof** — all four mechanisms were already
      present at C1's close (atomic OFD refcount, `vn_append_lock()`, atomic
      `sig_pending`, per-CPU SYSCALL state); no mechanism turned out
      non-atomic on inspection, and that is the finding. The receipt that can
      fail already existed and was re-run on this branch:
      `/tests/smpstress` under `-smp 4` — six appenders × 200 fixed records
      through six independent OFDs onto one file (the append lock),
      100 fork+exit cycles through the parent/child table (the table), and a
      SIGUSR1 storm against `sig_pending` (the atomic) — `5/5` assertions.
- [x] **Large pages** — 2 MiB leaves were already chosen by
      `kheap_expand()` (4 KiB head, whole 2 MiB bulk, 4 KiB tail, with the
      fallback to 4 KiB when the PMM cannot give a contiguous aligned run).
      What C2 added is the ACCOUNTING, because "the mm accounting must not lie
      about order": `kheap_2m_leaves` / `kheap_4k_pages` /
      `kheap_order_refusals` are incremented only where a leaf was actually
      installed, and the boot prints
      `[heap] large-page audit: 3 x 2MiB + 0 x 4KiB = 6144 KiB (100% of the
      heap rides 2MiB leaves); 1GiB leaves refused: 0 request(s) …` — the 1 GiB
      refusal as a number and a reason (the heap VA window is 256 MiB and the
      PMM cannot promise a contiguous 1 GiB run at the 512 MiB
      configuration). `split_huge_page()` re-exercised by the COW lane
      (`test_fork_cow` 7/7).
- [x] **Per-PML4 refcounting → wire every zombie** — the one box that was
      NOT already done, and the phase's real defect. `kernel/mm/aspace.{c,h}`
      is a refcount table keyed by the PML4 root (512 slots, 8-probe window,
      32 tombstones against double release, atomic via `__sync`), wired at
      every creator and dropper: clone(CLONE_VM) `aspace_get`, fork and spawn
      `aspace_get`, execve `aspace_get(new)` + `aspace_put(old)` instead of an
      unconditional free, reap `aspace_put`. Two defects fell out of it:
      (a) `thread_reap_zombies()` used to free an address space when the
      NON-pthread TCB was reaped, whichever thread actually died last;
      (b) **pthread zombies were never reclaimed at all** — clone() threads are
      not in the parent/child table and `pthread_join()` synchronises on the
      CLONE_CHILD_CLEARTID futex rather than `wait()`, so a pthread zombie had
      no waiter by construction and leaked a TCB + a 32 KiB kernel stack each
      (measured on this branch: running `/tests/fpustress` left four TCBs
      queued and unreaped). Same rule as an orphan adopted by PID 1 (which
      only ever waits for children IT spawned): auto-collected at exit. The
      boot log now carries one line per reaped thread with its frame count and
      the space's remaining owners.
- [x] **Real parent/child table + precise `waitpid`** — present at C1's close
      (children list, `waitpid(-1)`, `WNOHANG`, `ECHILD`, `WUNTRACED`,
      `WCONTINUED`, `wait4` rusage, orphan adoption), so the box was stale in
      the same way D1's ledger rows were. What C2 adds is the ordering proof
      the old code could not produce: `test_aspace_refs` pins
      `reaped 'pthread' … aspace refs 4` → `… refs 1` → the leader
      `reaped '/tests/fpustress' (… 1046 frames, aspace refs 0)`, i.e. the
      LAST owner frees the page tables. Existing lanes re-run green:
      `test_smp_procstress` (100-cycle fork/waitpid churn) 5/5,
      `test_fork_cow` 7/7, `test_posix_p10` 27/27.
- [x] **POSIX depth for `fork`/`execve`/`wait4`** — the floor was already
      above what the box assumed: `vfork`-class requests are refused with
      `-ENOSYS` at the syscall boundary (`syscall.c`, "not on the ladder"),
      `execve` honours `FD_CLOEXEC` (fdtest + selftest), and `wait4` fills a
      rusage from the child's `cpu_ticks` captured before the reaper runs
      (selftest asserts `ru_utime > 0` for a tick-burning child). Verified on
      this branch by `test_selftest`; nothing was re-implemented.

**Definition of done:** six boxes closed with receipts; host tests for
the process table, the refcount and the stub alignment; `posix` and
`core` shards green.

**Test gate (as written):** `make test-unit` (new `test_proctable`,
`test_ofd_refcount`); integration `test_waitpid_precise.sh` (new),
`test_stack_alignment.sh` (new), existing `posix` shard unmodified.

**Result (2026-11-03):** DONE. Five of the six boxes were stale — the
mechanisms existed, the receipts did not — and the sixth (per-PML4
refcounting) was real, so the phase landed as **one mechanism, two
defects, five proofs**. The gate the phase promised and the gate it
delivered differ in exactly one place, named below: `test_proctable` and
`test_ofd_refcount` were folded into `test_aspace` + the existing
`test_smp_procstress`, and `test_waitpid_precise.sh` was replaced by
`test_aspace_refs.sh`, because the waitpid semantics it would have
asserted are already pinned by `selftest`/`stoptest`/`smpstress` and
duplicating them would have been a second, weaker copy.

| Box | Verdict | Receipt |
|---|---|---|
| 16-byte C-ABI alignment | **3 of 7 call sites fixed**, 4 proven | `check_abi_align.py` 7/0 + selftest ×3; `test_abi_align.sh` 3 lanes; in-guest `[abi] PASS` in IRQ **and** syscall context; `test_stack_alignment.sh` 8/8 |
| SMP-safety sweep | already done, re-proved | `test_smp_procstress` 5/5 under `-smp 4` (O_APPEND ×6×200, 100 fork/waitpid cycles, SIGUSR1 storm) |
| Large pages | already chosen, now **accounted** | `[heap] large-page audit: 3 x 2MiB + 0 x 4KiB = 6144 KiB (100% …)`; 1 GiB refusal with a reason; `test_fork_cow` 7/7 re-exercises `split_huge_page` |
| Per-PML4 refcount → every zombie | **built** (`kernel/mm/aspace.{c,h}`) | `test_aspace` 7/7; `test_aspace_refs` 8/8: `refs 4→3→2→1` then the leader `1046 frames, aspace refs 0` |
| Process table + precise `waitpid` | already done, now **order-proved** | `test_aspace_refs` (last-owner free); `test_smp_procstress` 5/5; `test_posix_p10` 27/27 |
| POSIX depth `fork`/`execve`/`wait4` | already done | `vfork` → `-ENOSYS`; `FD_CLOEXEC` honoured; `wait4` rusage from the pre-reap `cpu_ticks`; `test_selftest` green |

Two defects the phase **found by building the receipt**, both invisible
before it existed:

1. **pthread zombies were never reclaimed.** `clone()` threads are not
   linked into the parent/child table and `pthread_join()` synchronises on
   the CLONE_CHILD_CLEARTID futex, not on `wait()` — so a pthread zombie had
   no waiter by construction. The old `!z->is_pthread` guard in the reaper
   hid the consequence by skipping the free instead of doing it, which is why
   nobody had noticed. Measured on the branch before the fix: `/tests/fpustress`
   printed four `'pthread' (tid N) exited` lines and **zero** reaped lines;
   after it, four `reaped 'pthread'` lines plus the leader's `1046 frames`.
   Cost per leaked thread: one TCB + one 32 KiB kernel stack, per boot.
2. **`execve` freed the old address space unconditionally.** With CLONE_VM
   siblings still running, the exec'ing thread is not necessarily the last
   owner, so the free tore the page tables out from under its own thread
   group. It is now `aspace_put(old)` and the free happens at zero.

Both are the same shape as D1: *the tree claimed a mechanism and nothing
asked it to show the mechanism doing work*. That is the phase's thesis
proved on its own subject matter.

| Landing set | Lines |
|---|---|
| `kernel/mm/aspace.c` (new), `kernel/mm/aspace.h` (new) | the refcount table |
| `kernel/proc/thread.c` | aspace drop at reap, orphan + pthread auto-collect, the per-zombie line |
| `kernel/proc/{clone,process,user}.c` | `aspace_get` at every creator; execve's put |
| `kernel/arch/x86_64/{boot.asm,isr.c,syscall.c,abi_probe.c}` | `and rsp,-16`, the probe, the `ASSERT` |
| `kernel/arch/i386/{boot32.asm,isr_stubs32.asm}` | the two i386 misalignments |
| `kernel/arch/x86_64/syscall_entry.asm` | the `@abi-anchor` |
| `kernel/mm/kheap.{c,h}`, `kernel/kernel.c` | the large-page census, the aspace census |
| `kernel/lib/abi_probe.h` | the prototype, kept OUT of `arch/` to hold the width-sweep include ratchet at 69 |
| `tools/check_abi_align.py`, `tests/unit/{test_aspace.c,test_abi_align.sh}`, `tests/integration/cases/{test_aspace_refs,test_stack_alignment}.sh` | the gate |
| `tests/integration/cases/test_memory_reaping.sh` | moved in the same commit: the reap line gained an `aspace refs` field and that case parses it |

One interface moved with the code, not after it: `[thread] reaped '<name>'
(tid N, M frames)` gained `, aspace refs R`, and `test_memory_reaping.sh`
is the one case that PARSES that line (it seds the frame count out). Its two
patterns and its sed moved in the same commit; the case's first run against
the new kernel was red (`1 of 9`), which is how the coupling was found rather
than remembered. Every other consumer of the line matched by prefix and was
unaffected.

Not-slack worth naming: the width sweep caught the C2 branch twice —
one `(uint64_t)` cast (403 → 404) and one `#include
"kernel/arch/x86_64/…"` from portable `kernel.c` (69 → 70). Both were
fixed by moving the code, not by moving the ratchet: the cast became
`(unsigned)`, the prototype became `kernel/lib/abi_probe.h`. A ratchet
that is raised in the same commit as the code that trips it is not a
ratchet.

---

### C3 — i386 graphics: VBE first, then the compositor

**Status:** ✅ done 2026-11-04 for RES-12 + the framebuffer console; the
compositor half is carried to **C3b** with its blocker list measured, not
guessed.

**Objective:** close RES-12 and then RES-11 in that order — the ledger's
own opener says the series starts at the fb seam, not the compositor.

Measured: `grep -rn "4F02\|vbe" boot/` matches exactly one comment in
`stage2_start.asm` ("there is no VBE"); the i386 kernel references none
of `gui.c`/`wm.c`/`fb.c` (zero hits under `kernel/arch/i386/`).

- [x] **RES-12:** VBE in Stage 2 — `INT 10h AX=4F01` to enumerate modes,
      `4F02` to set one with `LIN_FA` (1024×768×32 preferred, 800×600×32
      fallback, text mode as the last resort), fill a 32-bit
      `boot_info_t` framebuffer block through the same offset-checked
      header the 64-bit path uses, and print the chosen mode on both
      consoles. `tests/unit/test_boot_info_width.c` grows the fb fields.
- [x] **RES-11 (console half):** the framebuffer console for i386 — the
      SHARED `drivers/framebuffer/fb.c` + `psf.c` + `graphics.c`, admitted as
      `KERNEL32_SHARED` rows, behind ONE arch seam: paging flags.  The seam is
      `PAGE32_FLAG_MMIO` (PCD|PWT) in `paging32_map` plus
      `kernel/arch/i386/fbmap32.c`, which owns the device-memory window and
      the only VA arithmetic involved.  `kprintf32` fans out to `fb_putchar`
      first and to the VGA text fallback second — mutually exclusive by
      construction once Stage 2 sets a mode.
- [ ] **RES-11 (compositor half) → C3b:** the portable `wm.c` + `gui.c`
      surface on 32 bits.  MEASURED, not estimated: both files compile at
      `--target=i686-elf` with the strict i386 flags and **0 errors** (the
      width sweep already did its work); what is missing is the LINK.  `nm
      -u` on the i386 object lists 45 undefined symbols; 37 already exist on
      this side (graphics/psf/kprintf/kmalloc/kfree/string/spinlock/
      perfstat), and the 8 that do not are the blocker: `sched_current`,
      `timer_get_ticks`, `timer_get_frequency`, `keyboard_get_event`,
      `keyboard_get_mods`, `mouse_get_event`, `mouse_get_position`,
      `process_spawn` — the x86_64 process/scheduler/mouse surface, which
      belongs with the i386 process work (I386_PLAN I4+), not with a
      graphics phase.
- [ ] A VNC screenshot lane for i386 in `tests/integration/lib/gui_lane.sh`
      (`-vga std`, `qemu-system-i386`), reusing the 64-bit assertions.

**Definition of done:** `qemu-system-i386 -vga std` boots to a graphical
console and a compositor window; the screenshot gate passes; the width
ratchet records the new i386 adoptions.  *Met for the console half; the
compositor window is C3b's DoD.*

**What landed (receipts, 2026-11-04):**

| Claim | Receipt |
|-------|---------|
| Stage 2 sets a VBE mode | `[vbe] selecting 1024x768x32 (linear framebuffer)` on BOTH consoles, `[vbe] framebuffer live; boot_info.fb filled` on the UART |
| the 32-bit struct is the carrier | `[boot] framebuffer console online (1024x768x32, pitch 4096, 128x48 cells at fe000000)` — offsets from `build/boot_offsets.inc`, pinned by `test_boot_info_width.c` |
| pixels, not assertions | monitor screendump: 1024x768 P6 with 146 840 bg (16,16,28) and 10 447 fg (220,220,220) samples |
| the 64-bit path improved too | `fb: WC via PAT4 (768 pages` on a SeaBIOS boot — it used to print `fb: none present; WC remap skipped` |
| the fallback still works | `qemu-system-i386 -vga none` → `[vbe] no 32-bpp linear-framebuffer mode` + `[boot] VGA text console online` |
| mini-asm parity held | `stage2_start.asm` still byte-identical to nasm (8 192 bytes); it cost one encoding fix — a zero displacement must be written bare, or mini-asm emits a `+0x0` byte nasm does not |

**Two defects found while landing it (both fixed here, both recorded):**

1. **The silent MMIO alias.** `fbmap32.c`'s first window choice was
   `0xFD000000` — the address `net32.c` had already claimed for the e1000
   BAR0.  `paging32_map` refuses an already-mapped PTE and net32 threw the
   refusal away, so the NIC read its registers out of framebuffer pixels:
   the MAC printed as `1c:10:10:00:dc:dc` and DHCP never answered, with no
   line anywhere saying why, while the screen looked perfect.  Fixed by
   moving the framebuffer window to `0xFE000000` (every window is now listed
   in `fbmap32.h`) and by making net32 check the return value and name the
   conflict.
2. **A console that could not be built twice.** `fsglue32.c` defined
   `fb_putchar` as a shim onto the VGA text console; linking the real `fb.c`
   made it a duplicate symbol.  The shim is gone and the shared `kprintf`
   now links the real glyph renderer.

**Finding recorded rather than papered over:** the i386 console is
*shadowless* — `[fb] shadowless console (i386: no contiguous allocator, no
PAT); scrolls are in-place`.  `pmm32.h` has no contiguous multi-frame
allocator; the box's "no allocator assumptions" clause is honoured by
refusing the shadow out loud instead of growing one.

**Test gate:** `tests/integration/test_i386_vbe.sh` (new, 14/14: x86_64
lane, i386 lane, screenshot, and the `-vga none` negative control),
`tests/integration/cases/test_i386_gui.sh` (new, registered in `ALL_CASES`,
10/10: the BIOS-boot compositor with real pixel deltas plus the i386 console
photograph), `i386_shell_smoke.sh` extended to assert the framebuffer sink
AND the VGA fallback, `i386_boot32/cpu/proc_smoke.sh` still green, the
64-bit GUI shard untouched.

---

### C4 — MSI/MSI-X on virtio (RES-36)

**Status:** ✅ done (2026-11-04) — RES-36 flips `DONE@C4`

**Objective:** give virtio-blk and virtio-gpu real message-signalled
interrupts, with INTx kept as the fallback.

Measured before: `virtio_pci.c` parses capability id `0x09` only; `0x05`/`0x11`
appear zero times in `virtio_pci.c` and `pci.c`; the ISR register is
read-to-clear INTx.

**Result.** Three pieces, split along the seam the width sweep enforces:

- `drivers/pci/msi_caps.{h,c}` — the portable half: a capability walk to
  `0x05` (MSI) and `0x11` (MSI-X), the multiple-message capable/enable
  fields, table + PBA BAR offsets, and every write read back.  It reaches
  the device only through an accessor table (`msi_pci_io_t`), which is what
  makes it host-testable: `tests/unit/test_msi_caps.c` drives it against a
  synthetic 2 KiB config space (**12/12**, including a dropped-enable
  negative control, a writer-less transport, and malformed-list
  termination).
- `kernel/arch/x86_64/msi.{h,c}` — the other half: vectors `0x60`–`0x6F`
  (one monotone block, never reused — nothing here teardown-probes), a
  handler table, `msi_dispatch()` called from `isr.c` **before** the
  spurious branch, and the EOI written after the handler so the existing
  early-EOI path for IOAPIC lines is untouched.  `fwcfg_msi_probe()` reads
  `opt/auralite.msi`; the boot prints
  `[msi] message-signalled interrupts: ENABLED (build default on)`.
- the drivers: `virtio_blk.c` and `virtio_gpu.c` attach through
  `kernel/arch/arch.h` (the forwarding seam — see defect 3 below), program
  the MSI-X table entry, tell the device which entry to use, and print a
  receipt naming the vector.

**Receipts (QEMU, measured 2026-11-04, `tests/integration/cases/test_msix.sh`):**

| Claim | Receipt |
|-------|---------|
| the switch, and its cause | `[msi] message-signalled interrupts: ENABLED (build default on)` / `… DISABLED (fw_cfg opt/auralite.msi=off)` |
| the capability walk finds and programs MSI-X | `[msi] virtio-blk: MSI-X vector 96 (1 msg, 3 entries)` and the same line for `virtio-gpu` |
| the DEVICE accepts the entry | `[msi] virtio-blk: queue 0 -> table entry 0 (readback 0), x86 vector 96` |
| a message is really delivered | `[msi] virtio-blk: 1 interrupt(s) delivered on vector 96` — printed from the request path, after the first completed request, not at probe time |
| the request path itself works | `[virtio-blk] self-test: LBA 0 read OK (interrupt delivered)` — the driver had **no in-tree caller** on x86_64, so C4 added a read-only sector-0 self-test; without it "interrupts are configured" would have been the whole claim |
| the GPU too | `[msi] virtio-gpu: MSI-X vector 96 (1 msg, 3 entries)`, `queue 0 -> table entry 0 (readback 0)`, `1 interrupt(s) delivered on vector 96`, over the control queue that carries `GET_DISPLAY_INFO` |
| the fallback is not a hope | `opt/auralite.msi=off` → both devices still initialise, `[virtio-blk] self-test: LBA 0 read OK (INTx/polling)`, and **no** `MSI-X vector` / `delivered on vector` line appears (asserted with `il_assert_no_grep`) |
| nothing else was disturbed | no `spurious interrupt`, no panic, the shell is reached in all four lanes |

**Three defects found while landing it (all fixed here):**

1. **The vector/table-index conflation.**  The first run printed
   `[msi] virtio-blk: MSI-X vector 96 …` and then
   `device refused queue 0 vector 96 (NO_VECTOR); keeping INTx`.  The device
   register `queue_msix_vector` takes the **table entry index**, not the x86
   vector; the vector belongs in the entry's DATA field.  QEMU's table has 3
   entries, so 96 was simply out of range.  Both drivers now write
   `MSI_TABLE_ENTRY` (0) and read it back; `msi.h` records the mistake next
   to the constant, because the two numbers look interchangeable and are not.
2. **virtio-blk never negotiated features at all.**  `device_status` went
   ACK→DRIVER→FEATURES_OK→DRIVER_OK without ever writing `driver_feature`,
   and without `VIRTIO_F_VERSION_1` (bit 32) a transitional device stays in
   legacy I/O-BAR mode: every write to the modern common-config window is
   accepted and means nothing, so the first request never completes.  This
   was invisible until C4's self-test issued one.  The negotiation is now
   real, `FEATURES_OK` is checked instead of assumed, and MSI-X (bit 27) is
   accepted when offered.  The same commit fixed `capacity`, which was being
   read out of PCI BAR0 (`18356408198371524609 sectors` on an 8 MiB disk) —
   it now comes from the device-config capability (cfg_type 4), and the
   receipt reads `capacity 16384 sectors`.
3. **The width ratchet caught the first draft.**  Both drivers included
   `kernel/arch/x86_64/msi.h` directly, which is exactly what
   `check_width_sweep.py` forbids (72 > ratchet 69).  The MSI entry points
   are now forwarded through `kernel/arch/arch.h`, with the non-x86_64
   arches declaring them `unavailable` rather than stubbing them — a device
   that silently never interrupts is the bug shape this phase exists to
   catch.  Sweep back to `403/403, 69/69, 0, 27/27`.

**Two measurements that corrected the plan's own text:**

- The plan's `-device virtio-blk-pci,msix=on` / `virtio-gpu-pci,msix=on`
  arguments **do not exist** in this QEMU: passing them is a hard error
  (`Property 'virtio-blk-pci.msix' not found`) and killed every lane of the
  first gate run before the firmware started.  Both devices offer MSI-X
  unconditionally; the lanes pass neither.
- The DoD's expected `vector 42` is written down wrong: the block starts at
  `0x60`, so the first device gets **96**.  The pinned line names 96 and its
  table size.

**Recorded, not caused here:** `test_virgl_gpu.sh` carries a
KNOWN DEFECT saying the virtio-gpu driver hangs during initialisation as soon
as a device is attached (predating phase G11d).  It **does not reproduce on
this tree** — measured twice, with a GPU attached and with C4's GPU MSI code
compiled out: both boots reach the shell and print
`[virtio-gpu] 2D resource ready`.  The case's comment is updated to say so
and to keep the hang's reproducer list; the attribution is left open rather
than credited to this phase.

**Two gates had to move, and both moves are recorded here rather than made
quietly:**

1. `tools/residue_baseline.txt`: `RESIDUE3_PLAN.md 47 → 51` (the C4 section and the
gate-change note below it).  This section
   names residue (the untested MSI path, the polling request path) on one
   line the harvester counts; the ratchet's own rule is that the baseline
   moves in the same commit that grows it.
2. `tools/check_residue_claims.py`: the R12 clause asserted
   *"at least one `HANDED-OFF@R12` row is still tracked"* (`r12_rows >= 1`),
   which was correct while one was.  C4 closed the last one, so the clause
   now asserts the terminal state — `r12_rows == 0` **and** five successor
   rows (`DONE@T6/T7/C3/C4`) proving the rows were closed by succession, not
   deleted.  A new `HANDED-OFF@R12` row is now a failure, which is the
   property the clause was always reaching for.

**Test gate:** `tests/integration/cases/test_msix.sh` (new, registered in
`run_all.sh` ALL_CASES, **28/28** across four lanes: virtio-blk MSI-X on,
virtio-blk MSI off, virtio-gpu MSI-X on, virtio-gpu MSI off),
`test_virgl_gpu.sh` **13/13** unmodified in behaviour, `make test-unit`
**EXIT=0** with `test_msi_caps` 12/12, `check_width_sweep` OK, and
`test_e1000_irq.sh` (the nearest INTx neighbour) unaffected.

**Residue this phase names rather than hides:** the walker's MSI (non-X)
path is host-tested only — no virtio device in this QEMU offers cap `0x05`,
so its table programming has never been run against silicon; and the request
paths still POLL the used ring (the interrupt is delivered and counted, but
I/O does not sleep on it — that is a scheduler change, not a transport one).

### C5 — USB: isochronous streaming and HID breadth (RES-38's survivors)

**Status:** ✅ done (2026-11-04) — RES-60 flips `DONE@C5`.  The HID-breadth and
EHCI-split bullets are **C5b** (RES-62), with the openers measured below.

**Objective:** close the two survivors the T6 measurement left standing:
frame-accurate isochronous transfers, and HID beyond the boot protocol.

Measured before: `test_usb_isoc` printed `[isoc] SKIP: no isochronous endpoint
exercised`; `xhci.c` 2093 lines carried rings and an ISOCH TRB type (fixed at
U9) that **nothing called** — `usb_isochronous_transfer()` routed xHCI to
`xhci_bulk_transfer()`, a bulk transfer on an isochronous endpoint.

**Result — what actually had to be fixed, in the order it was measured:**

1. **The routing.** `usb_isochronous_transfer()`'s xHCI case called
   `xhci_bulk_transfer()`.  `xhci_isochronous_transfer()` existed, was correct
   as far as it went, and had no callers.  Now it is the xHCI case.
2. **The endpoint type.** `xhci_isochronous_transfer()` configured the endpoint
   as ISOCH… in its own words, and then passed `is_in ? 7 : 3` to
   `xhci_configure_ep()` — **interrupt IN / interrupt OUT**.  A type-5 TRB on an
   interrupt endpoint is not an isochronous transfer; QEMU rejects it.  The
   types are now 5 (isoc IN) and 1 (isoc OUT), and a re-configure is issued
   when an endpoint's cached type disagrees (which is what an alt-setting
   switch does).
3. **The clock.** MFINDEX is the **first register of the RUNTIME space**
   (offset 0x00), not an operational register; reading operational `0x1C`
   returns the high dword of CRCR and always read as frame 0.  Reading it
   correctly showed the frame counter advancing.
4. **The scheduling policy — measured, not assumed.** Pinning packet *i* to
   frame `base+i` (what the specification describes) delivered **1 packet out
   of 24**: QEMU's isoclock runs far ahead of the guest (measured: ~45 frames
   during one 8 ms wait), so every pinned frame after the first was already in
   the past when the controller walked the ring.  The identical burst with
   **SIA (Start-Isoch-ASAP)** on every packet delivered **24/24**.  The burst is
   therefore issued ASAP and the accounting reports what the isoclock says.
   Both numbers are in the code comment next to the decision.
5. **A device that consumes it.** `usb_audio_stream_selftest()` generates a
   440 Hz sine in the guest (Q15 quarter-wave table, no libm), packetises it at
   the endpoint's own max packet size (192 B = 48 stereo frames at 44.1 kHz =
   one full-speed frame), selects alt setting 1, streams 24 packets through the
   isoc path and counts what came back.  No host audio backend is needed:
   `-audiodev none` is enough, because the claim is about frames, not sound.
6. **The short CSW line** (RES-38's BOT survivor): a CSW is exactly 13 bytes
   (BOT 6.2.3); a short one used to surface as `bad CSW signature 0x0`. It is
   now `[msc] short CSW: N byte(s), want 13`, and the signature check is not
   reached on a truncated wrapper.

**Receipts (QEMU, measured 2026-11-04):**

| Claim | Receipt |
|-------|---------|
| packets move through a real isoc endpoint | `[isoc] PASS: 24 packet(s) streamed on xHCI ep 0x01 (192 B/packet, 440 Hz sine, 1152 frames of audio)` |
| the accounting is next to the count | `[isoc]   frames: scheduled 24, completed 24, lost(overrun) 0, empty bursts 0, timeouts 0, isoclock frame(s) elapsed 4` |
| the isoclock is live (frame figures mean something) | no `MFINDEX FROZEN` suffix, and the case asserts its absence |
| the framework agrees with the producer | `[isoc] PASS: isochronous endpoint exercised (24 packet(s) scheduled on frames; see the [isoc] PASS line above)` |
| the device is the real USB audio class driver | `[audio] attached addr=1 VID=0x46f4 PID=0x0002 as audio0 ctrl IF 0 streams 2`, `[audio] start stream 1 IF 1 -> OK`, `endpoint 0x01: isoc OUT, maxpkt=192, interval=1, sync=0xc` |
| nothing timed out or faulted | no `[xhci] isoc timeout`, no `bandwidth exceeded`, no panics |

**Test gate:** `tests/integration/cases/test_usb_isoc.sh` **11/11** — the
assertion that used to require the SKIP line now requires the PASS line, and
the SKIP's history (U0 deleted a false PASS; U9 made the TRB real and the case
honestly asserted a SKIP; C5 makes frames move) is written in the case itself
so the flip is auditable.  `test_virgl_gpu`, the GUI cases and the rest of the
USB sweep are untouched by this phase and re-run green.

**Carried to C5b (RES-62), with the openers measured here:**

- **HID breadth.** `hid.c`'s parser keeps **one** `report_id` global
  (`rp->report_id`) and tags no field, so a descriptor with report IDs 1 and 2
  collapses every field onto the last ID and the decoders compare against that
  single value.  The fix is per-field report IDs plus a host test with a
  synthetic multi-report-ID fixture — the same shape C4 used for `msi_caps`
  (extract, host-test, then drive it from the driver).
- **EHCI split transactions.** `ehci.c` carries the TT fields (hub address,
  S-mask, C-mask) and the `test_usb_hub*` cases attach their hub on **xHCI**
  (`usb-hub,bus=xhci.0`).  No case puts a full/low-speed device behind a
  high-speed hub on EHCI, so that code is unexercised rather than absent.
- **A short-CSW case.** The line landed here; nothing asserts it yet (a real
  device will not emit a truncated CSW, so it wants a host-level fixture).

---

### C5b — USB survivors: HID report IDs, EHCI split transactions, the short-CSW case (RES-62)

**Status:** ✅ done (2026-11-04) — RES-62 flips `DONE@C5b`.  One bullet's
device case was replaced by measurement: QEMU has no multi-report-ID HID device
and no high-speed hub, so both were rebuilt as host tests plus a kernel
selftest, and the gaps are asserted rather than papered over.

**Objective:** take the three measured survivors of C5, with the openers C5
wrote down.

**Result — what actually had to be fixed, in the order it was measured:**

1. **The parser moved out of `hid.c`** into `drivers/usb/hid_report.{h,c}`:
   a per-report bit cursor, a `report_id` on every field, `report_ids[]` /
   `report_bits[]` / `report_bits_max` per descriptor, and `hid_report_body()`
   to split the ID prefix off an incoming report.  Before this, `hid.c` kept
   **one** `report_id` global: a descriptor with IDs 1 and 2 parsed into a
   single blob, so report 1 was dropped or decoded with report 2's layout.
2. **A bug the extraction found on its own.** `hid_report_wire_len()` counted
   output-report bits as input bits.  A Report ID is shared by the input,
   output and feature reports of that ID (HID 1.11 §6.2.2.7) but the lengths
   are separate, so QEMU's keyboard (8 input bytes + an 8-bit LED report under
   no ID at all) computed a **10-byte** wire length for a **9-byte** endpoint
   in the first cut.  Input bits only, now — and `test_usb_hid_report.c` pins
   it with a QEMU-shaped descriptor.
3. **The driver polls what the descriptor asked for**, and logs both numbers:
   `poll_len` (from the descriptor) and `ep_maxpkt` (from the endpoint).  A
   device may over-allocate a packet (QEMU's tablet asks 8, sends 6); the
   failing direction — a descriptor that claims more than one packet can
   carry — gets its own `[hid] note:` line.
4. **EHCI port speed was a stub.** `ehci_port_speed()` returned
   `"high-speed"` for every port and `usb_enumerate_all()` passed
   `USB_SPEED_HIGH` unconditionally, so a **full-speed** device on a
   high-speed root port was enumerated as high-speed and its queue head said
   `eps=high`.  `PORTSC.PSPD` is decoded now (0=full, 1=low, 2=high), the
   speed travels into enumeration and into the QH: QEMU's keyboard on EHCI
   reports `[ehci] port 0: full-speed device (PSPD=0)` and the QH says
   `eps=0, split=no`.
5. **The QH capability words are built by a host-tested module.**
   `drivers/usb/ehci_qh.{h,c}` owns the bit positions from EHCI 1.0 §3.6 and
   §4.12 (address, endpoint, EPS, DTC, MPL, S-mask, C-mask, TT address/port,
   Mult) and `ehci.c` only allocates, links and polls.  Writing the test found
   a real defect in the old inline code: a start-split in micro-frame 7
   produced an **empty C-mask**, so that endpoint could never complete.  The
   mask now wraps past bit 7 and the test sweeps all eight start phases.
6. **The CSW check left `msc.c`** for `drivers/usb/msc_csw.{h,c}`: length
   first (13 bytes exactly, short and over-long are separate results), then
   little-endian decode of the signature, the tag, the remaining-byte count and the status byte — explicitly decoded
   rather than memcpy'd into a packed struct, because this kernel also builds
   for big-endian-capable targets.  `msc.c` prints the result names instead of
   re-deriving them.

**Receipts (QEMU 10.0.13, measured 2026-11-04):**

| Claim | Receipt |
|-------|---------|
| a two-report-ID descriptor keeps both IDs and both body lengths | `[hid] selftest: two-ID descriptor -> ids=2 (1,2) input bits: r1=64 r2=4 wire=9` |
| output bits do not lengthen the input report | `[hid] selftest: QEMU-shaped keyboard -> report_id=none input bits=64 wire=8` |
| the split, the per-report decode and the unknown-ID rejection all hold in the kernel build | `[hid] selftest: PASS report-ID split and per-report decoding` |
| the parser agrees with the three devices QEMU does send | `[hid] parsed generic keyboard report: fields=14 input_bits=64 report_id=none`, `pointer report: fields=8 input_bits=32`, `pointer report: fields=6 input_bits=48` |
| poll length vs endpoint capacity, both named | `keyboard ready: ... poll_len=8 ep_maxpkt=8`, `mouse ready: ... poll_len=4 ep_maxpkt=4`, `generic-mouse ready: ... poll_len=6 ep_maxpkt=8` |
| EHCI decodes the port speed instead of assuming it | `[ehci] port 0: full-speed device (PSPD=0)`, `[usb] device at addr 1: ... speed=full (12 Mbps)` |
| the QH carries the right speed and no phantom split | `[ehci] interrupt endpoint 0x81 dev=1 on periodic schedule (interval=8 frames, maxpkt=8, eps=0, split=no, S-mask=0x01 C-mask=0x00)` |
| the test discriminates (fails on the pre-C5b parser) | `tests/unit/test_usb_hid_report_negcontrol.sh` → `negcontrol: PASS — the pre-C5b parser fails 5 check(s) in the same test` |

**Test gates:** `test_usb_hid_report_ids` **13/13**, `test_usb_ehci_split`
**11/11**, `test_usb_hid_input` **9/9** (unchanged after the rewiring),
`test_usb_msc_hotplug` **8/8**; host unit `test_usb_hid_report` **8/8**,
`test_usb_ehci_qh` **9/9**, `test_usb_msc_csw` **10/10**, all three registered
in `make test-unit`.  Registry: 220 integration cases, all registered.

**Two bullets changed shape after measurement — recorded, not hidden:**

- *"An integration case with a real multi-report-ID device (`usb-tablet` +
  `usb-kbd`)"* — **no QEMU HID device declares a Report ID item at all.**
  Dumping the descriptors usb-kbd, usb-tablet and usb-mouse actually send
  (C5b measurement) shows zero `0x85` items.  The case that exists therefore
  asserts the two things that are real: a kernel-side selftest over a two-ID
  descriptor carried in-tree, and, per device, the parsed input length against
  the endpoint packet size.
- *"EHCI split transactions … asserting the TT fields actually schedule the
  periodic transfer"* — **QEMU cannot build the topology.**  Its only hub is
  `usb-hub`, a full-speed device, and QEMU refuses it on a high-speed bus:
  `Warning: speed mismatch trying to attach usb device "QEMU USB Hub" (full
  speed) to bus "e.0", port "1" (high speed)`.  `test_usb_ehci_split.sh`
  asserts that refusal (rc≠0, message, verbatim command line), so the missing
  device case is a measured property of the emulator; the TT encoding itself
  is pinned bit by bit in `test_usb_ehci_qh.c` and the split decision is
  printed by `ehci.c` on every periodic endpoint.  On real hardware with a TT
  the log shows `split=yes via TT` plus a `TT: hub address N port M` line;
  **that path has not been seen on silicon** — it is the one part of C5b whose
  receipt is an encoding plus a log, not a transfer.

---

### C6 — Bluetooth and Wi-Fi: the hardware backends (RES-39's survivors)

**Status:** ✅ done (2026-11-04) — RES-61 keeps its metal half
(`PENDING-USER@C6`); the protocol half is now proven on both stacks, and the
Bluetooth bullet was rewritten because the emulator cannot host it.

**Objective:** the last mile of both protocol stacks: a controller the
emulator can actually attach.

**What the measurement said before any code moved.** QEMU **removed its
Bluetooth subsystem in 9.0**.  On the emulator this project tests against
(10.0.13):

```
$ qemu-system-x86_64 -bt help
qemu-system-x86_64: -bt: invalid option
$ qemu-system-x86_64 -device help | grep -i -E 'bluetooth|bt-dongle'
(nothing)
```

So the bullet *"QEMU's `-device usb-bt-dongle` … a real HCI round trip with the
controller's own bytes"* is **not buildable**, and `test_bt_hci_transport.sh`
case 1 asserts that rather than leaving the reader to wonder why no dongle case
exists.  The Wi-Fi side needed no emulator at all: `wifi_virt` is a backend in
the tree, so the data path there is real traffic.

**Result — Bluetooth.** The command/event round trip left `bt.c` for
`drivers/bluetooth/bt_transport.{h,c}` with the transport injected
(`bt_io_t.cmd_out` / `evt_in`).  `bt.c` binds those to USB — commands as class
requests on endpoint 0 (`bmRequestType 0x20`), events as `0xA0` with a fallback
to the enumerated interrupt/bulk IN endpoint — and the endpoints now come from
enumeration instead of the hardcoded `0x82`/`0x02` the file used to carry.
Against a scripted CSR-class controller (`bt_selftest.c`, answers per HCI Core
5.x) the kernel build produces:

| Claim | Receipt |
|-------|---------|
| Reset + Read Local Version + Read BD_ADDR round trip | `[bt] selftest: bring-up -> 3 command(s), HCI version code 0x0c (5.3) rev 0x0f23, manufacturer 0x001d (CSR)` |
| the BD_ADDR is the controller's answer, wire order | `[bt] selftest: BD_ADDR bc:9a:12:34:56:78 read from a Command Complete` |
| inquiry sends the GIAC LAP and parses results | `[bt] selftest: inquiry -> 2 device(s), GIAC LAP 9e8b33, first 66:55:44:33:22:11` |
| pairing is a named refusal | `[bt] selftest: pairing -> refused (pairing is out of scope at this phase: …)` |
| a completion for the WRONG opcode is refused | `[bt] selftest: wrong-opcode control -> refused (-1)` |
| no controller is claimed | `[bt] no Bluetooth controller found`, then `[bt] selftest: PASS HCI transport round trip (no controller attached; QEMU has no Bluetooth device)` |

Host side: `tests/unit/test_bt_transport.c`, **10/10** — including two
controllers that a happy-path test would never build: one that answers
*nothing* (the driver must report a transport failure, not a success with
zeroed fields) and one whose BD_ADDR is `00:00:00:00:00:00` (a real class of
broken clone; the driver keeps "no address" and fails bring-up).

**Result — Wi-Fi, the data path C6 asked for.** `wifi_virt` grew an
Ethernet-level AP: an ARP request for its address is answered with a real ARP
reply, an ICMP echo request addressed to it with a real echo reply, both built
from the received bytes with recomputed checksums, and both handed back through
a queue.  `drivers/wifi/wifi_netdev.c` exposes that queue as a `struct netdev`
(send → `wifi_send_data`, i.e. Ethernet → 802.11 + LLC/SNAP; recv → the queue),
and `kernel/net/net.c` attaches it as the **last** fallback in its NIC chain —
so a machine with a wired NIC never changes behaviour.  Booted with `-nic none`
the receipt is the IP stack's own ping:

```
[netdev] active NIC: wifi-virt
[net] wifi: virtual AP has no DHCP server; static 10.0.2.15/24, AP 10.0.2.2
[net] ARP request sent for 10.0.2.2
[net] ARP reply: 10.0.2.2 is 02:11:22:33:44:55
[net] ICMP echo request sent to 10.0.2.2
[net] ICMP echo reply received from 10.0.2.2 (seq 1)
[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)
[wifi] PASS: virtual AP answered 1 ARP request(s) and 1 ICMP echo request(s) with real replies (2 Ethernet frame(s), 116 bytes)
```

`test_wifi_virtual_ap` **18/18** (was 8): case 1 keeps the T6 management
receipts on a normal boot, case 2 is the `-nic none` run above.
`test_bt_hci_transport` **12/12**.

**Two defects found by measuring, both fixed here:**

1. `wifi_init()` reset the connection state unconditionally, so the boot
   self-test's second call tore down the association `net_init()` had just
   brought up and the receipts vanished (the log said only "wireless driver
   registered").  Init is idempotent now and says which state it found.
2. The T6 case's shell interaction was a blind 12 s sleep; the C6 boot is
   longer (it brings a link up and pings it), and the command was typed before
   the shell existed — the polling UART drops it, so `gate-end` never appeared.
   The case now gates on the banner (`il_send_wait`), which is what the library
   asks for: a content gate, not a guessed sleep.

**Status rows:** `docs/status.md`'s two wireless rows no longer say 🚧 alone.
Both now read *protocol/data path ✅ / silicon 🚧* and name the measurement
(QEMU 9.0 removed hw/bt; no 802.11 radio) instead of implying nothing works.

**Still open, and named:** RES-61 (M, `PENDING-USER@C6`) — a real dongle and a
real 802.11 chip are the user's to attach; nothing in this phase claims them.
The Wi-Fi AP answers exactly two protocols (ARP, ICMP) and says so; TCP over
the virtual link would need a real peer, not another self-answer.

**Test gate:** `test_wifi_virtual_ap` 18/18, `test_bt_hci_transport` 12/12,
`test_networking` 8/8 and `test_tcp_x5` 12/12 unchanged (the wired path is
untouched), `make test-unit` green with `test_bt_transport` 10/10 and
`test_wifi_proto`/`test_bt_hci` unchanged; registry 221 cases, all registered.
The standing `w32_import_ledger: CHECK FAILED (8 problems)` (no
`tools/w32_import_ledger.py` in the tree) is still there under an EXIT=0
runner — pre-existing, unrelated, and named at C5/C5b as well.

---

### C7 — network: window scaling (RES-52) — DONE; NVMe carried to C7b

**Status:** ✅ window scaling done (2026-11-04) — RES-52 flips; the NVMe half
of RES-46's successor is **carried to C7b** with a measured opener, because
the phase's own definition of done (a 1 MiB lane) and a queue-pair driver are
not the same afternoon's work and this plan does not pretend otherwise.

**Objective (as planned):** close the TCP window box and the storage/NIC half
RES-46 left standing.

**What was measured before the work:** `TCP_WINDOW` was 64240 and the window
field was a constant; `wscale`/`shift_cnt` had zero hits in tcp.c+tcp.h. A
16-bit window caps a connection at 65535 bytes in flight. Two more ceilings
turned up while working, neither of them in the plan:

1. **`ssthresh` WAS the ceiling.** `ssthresh = TCP_WINDOW` carried the comment
   "arbitrarily high" — true while the window itself was capped, and false the
   moment a peer could grant 16 MiB. Measured: with a 16 MiB peer window, peak
   in-flight stopped at *exactly* 64240 on a 1 MiB transfer.
2. **The connection silently forgot its shift.** The established-path code
   reset `snd_shift = 0` right after the SYN-ACK parser had learned it, so a
   correctly negotiated connection fell back to a 16-bit window the instant
   the handshake finished. Measured: `peer options: … wscale=8` followed by
   `ESTABLISHED, peer shift 0`.

**Result.** `kernel/net/tcp_wscale.h` holds the arithmetic (parse / emit /
apply / encode / shift-for-buffer / engaged), pure and host-pinned by
`tests/unit/test_tcp_wscale.c` — **56/56**, including the cases a happy path
never writes: shift 255 clamped to 14, a WSCALE with the wrong length ignored,
an option block that ends mid-option, a zero-length option (the classic way to
spin a parser), and a buffer one byte short of the next shift.

`tcp.c` emits a WSCALE in its SYN (option block 8 → 12 bytes, header 28 → 32),
reads the peer's answer in the SYN-ACK, applies `snd_wnd = field << shift`, and
encodes its own advertised window with `rcv_wnd >> shift`. **Our shift is 0 and
that is a decision, not an omission:** the receive path streams data straight
to the caller and holds only the single out-of-order stash, so the honest
promise is one unscaled window (`TCP_RCV_BUF_BYTES = TCP_WINDOW`). Advertising
a shift we cannot back would invite the peer to put megabytes in flight that we
would drop. The receipt says so out loud: `our shift 0 (64240-byte receive
buffer)`.

**The 1 MiB lane.** `kernel/net/tcp_ws_lane.c`, opt-in through fw_cfg
(`opt/auralite.tcp_ws=<big>:<clamped>`), so every other boot in the suite is
untouched. It moves a megabyte to a real Linux peer twice and prints what the
connection did:

| Claim | Receipt |
|-------|---------|
| the peer's 16-bit field means megabytes through the shift | `[tcp] wscale: peer shift=8 -> snd_wnd=16776960 (field 65535); our shift 0 (64240-byte receive buffer)` |
| a megabyte crossed, and more than 65535 bytes were in flight | `[tcp] wscale lane (big): sent 1048576 bytes in 460 ms, peak in-flight 395660 bytes, peer window 1502208 (shift 8)` |
| the ceiling was passed | `[tcp] wscale lane (big): PASS 395660 bytes were in flight at once — above the 65535-byte unscaled ceiling` |
| a clamped peer holds the SAME transfer inside 16 bits | `[tcp] wscale lane (clamped): PASS peak in-flight 14600 stayed inside the 65535-byte window the peer actually granted` |
| a peer that omits the option is told apart | `[tcp] wscale: peer did not offer a window scale; snd_wnd=65535 stays 16-bit` |

The control lane is what makes the first lane mean anything: without it, a
guest that simply believed any number would pass.

**Two substitutions, both measured, neither hidden:**

- **The 200 ms RTT is not there.** QEMU has no netem; a userspace peer cannot
  delay ACKs without also shrinking the window it grants (delaying reads does
  nothing until the receive buffer fills, and filling it is the opposite of
  granting a large window). "200 ms" and "window above 64 KiB" cannot both hold
  through such a peer, so the lane measures **bytes in flight** and reports the
  elapsed time. The plan's intent — prove the window grew past 64240 — survives
  intact; its clock does not.
- **The lane needs a TAP link, and the case says so.** SLIRP terminates TCP
  itself and never offers a window scale, so a scaled peer cannot exist behind
  it. `test_tcp_wscale.sh` therefore creates a TAP (`10.0.2.2/24`), which needs
  `sudo` + `/dev/net/tun`; without them it prints an asserted SKIP and claims
  nothing. Its SLIRP half is a case in its own right: SLIRP *is* the peer that
  omits the option.
- **`il_run_qemu` cannot be used for the TAP lane** — the library always
  appends `-netdev user` + its own NIC, and with two NICs the SLIRP one wins
  registration. Measured (and fixed): the lane ran over SLIRP while the TAP
  peer still received its megabyte — a green-looking half-truth. The case
  drives `$IL_QEMU` directly.

**Test gate:** `test_tcp_wscale` **19/19**; `test_tcp_options` 5/5 (updated:
the SYN's option block is 12 bytes now, and the assertion is still derived from
the header built); `test_networking` 8/8, `test_tcp_x5` 12/12,
`test_tcp_ordering` 10/10 unchanged; `test_e1000e` 14/14 and `test_vmxnet3`
14/14 re-derived (the plan's fourth bullet: T6's receipts still hold, so that
half is closed explicitly rather than rebuilt); unit `test_tcp_wscale` 56/56 in
`make test-unit`; registry 222 cases.

**NVMe: carried to C7b, with the opener measured now.**

- `qemu-system-x86_64 -device nvme,help` lists a full device (queues, AER,
  `num_queues`, `nsid`, …) — the emulator half exists.
- `grep -rli nvme drivers/ kernel/` finds **one** file, and it is a comment in
  `drivers/vm/virtual_drivers.c`: there is no NVMe code in this tree at all.
- The seam it must land on exists: `kernel/fs/blkdev.h` (`blkdev_register`,
  `blkdev_read/write`, `blkdev_sector_count`), which is how AHCI and virtio-blk
  already become mountable filesystems.
- There is no `kernel/arch/x86_64/pci_ecam.c` in the tree (the plan assumed
  one); PCI access is `drivers/pci/pci.c` with `msi_caps.c` beside it.

So C7b is queue pairs + PRP lists + identify + a blkdev registration, then a
mount — a phase, not a bullet.

---

### C7b — NVMe: a queue-pair driver over PCIe (RES-46's storage successor)

**Status:** ✅ done (2026-11-04) — the successor RES-46 named now carries a
mounted filesystem, not a probe.

**Objective:** an NVMe namespace that becomes a block device and mounts ext2.

**Opener (measured before any code, recorded in C7):** `qemu-system-x86_64
-device nvme,help` lists a full device; `grep -rli nvme drivers/ kernel/`
found **one** file and it was a comment; the seam (`kernel/fs/blkdev.h`) exists;
there is no `pci_ecam.c` in this tree — PCI access is `drivers/pci/pci.c`.

**What landed.**

| Piece | Where |
|-------|-------|
| Encoding: register packing, the 64-byte SQ entry, the 16-byte CQ entry, doorbell geometry, Identify decoding, LBA-format resolution | `drivers/nvme/nvme_proto.h` (pure, host-pinned) |
| Hardware: PCI probe, BAR map, reset, admin/I-O queue pairs, bounce-buffer transfers, blkdev registration, self-test | `drivers/nvme/nvme.c` |
| Host pin | `tests/unit/test_nvme_proto.c` — **78/78** |
| Mounted-filesystem gate | `tests/integration/cases/test_nvme.sh` — **23/23** |

**Receipts (QEMU 10.0.13, `-device nvme,serial=AURA0001`):**

```
[nvme] VS=0x00010400 CAP=0x4008200f0107ff (MQES=2047, DSTRD=0, MPSMIN=0)
[nvme] reset: CC.EN=0 -> CSTS.RDY=0
[nvme] admin queues: AQA=0x00070007 ASQ=0x4021000 ACQ=0x4022000 (8 entries)
[nvme] reset: CC.EN=1 -> CSTS.RDY=1 (controller ready)
[nvme] identify controller: model="QEMU NVMe Ctrl" serial="AURA0001" firmware="10.0.13"
[nvme] namespace 1: 32768 blocks x 512 bytes = 16 MiB
[nvme] I/O queue pair ready (qid 1, 8 entries, physically contiguous, no interrupts — completion is polled)
[blkdev] registered 'nvme0' as disk 1 (32768 sectors)
[nvme] read LBA 0: first bytes 00 00 00 00, checksum 0 (driver) vs 0 (seam)
[nvme] PASS: 32768 blocks reachable, both paths agree on LBA 0
[ext2] blkdev 1 mounted at /ext2
[ext2] PASS: ext2 read/write/dir/indirect works
```

…and the shell reads the `NVME.TXT` that `debugfs` put on that image, writes
`from_aura.txt`, and the **host** reads those bytes back off the image
afterwards.  The negative control is the same boot's sibling: with no
`-device nvme` the driver prints **nothing at all** (`il_assert_no_grep
"\[nvme\]"`), which is what makes the lines above evidence rather than noise.

**Three defects found by measuring, all in this phase:**

1. **The SQ tail doorbell carried the OLD tail.** Ringing with the slot just
   filled instead of the next free one tells the controller there is nothing
   new to fetch: the command times out with every register perfectly healthy
   (`CSTS.RDY=1`, `AQA` correct).  Measured on the first Identify.
2. **The completion status was read out of the wrong half of DW3.** DW3's low
   16 bits are the *command id*; the status (phase, SC, SCT, DNR) is the high
   half.  Reading the low half made a perfectly good completion look like a
   phase-0 error — and would have made some real errors look like successes,
   because the CID's low bit reads as a phase tag.  Both halves are now named
   in `nvme_proto.h` with the trap written down, and the host test pins the
   swapped-halves case.
3. **A completion is now checked against the command id it answers**, not just
   its phase bit: unwritten queue memory is all ones, whose phase is 1 — the
   same value a driver waiting for phase 1 expects.  `nvme_cqe_plausible()`
   takes the expected CID.

**Declared scope (in the header, not implied):** polled completion (no
interrupts), one command in flight, PRP1 only via a 4 KiB bounce buffer (so
the caller's buffer need not be physically contiguous — at one memcpy per
4 KiB), one namespace, no PRP lists/SGLs/ZNS, and 512-byte LBAs only, because
the blkdev seam below it is 512-only; a 4096-byte namespace is refused **by
name**.

**Test gate:** `test_nvme` 23/23; unit `test_nvme_proto` 78/78 in
`make test-unit`; registry 223 cases.  `test_ext2`, `test_fsfull_*` and the
block-device cases are unchanged — NVMe registers only when a controller is
present, so their disk ordering cannot move.

**Patch:** the row's path; replayed onto `26cb7fe` + C0…C7 it yields **14/14
byte-identical** files (the exec bit on `test_nvme.sh` included), and every
claim checker is green in the replayed tree as well as in this one.  Size and
digest are recorded in the study log rather than here, because this file is
inside its own patch and any number written here would change the artefact
that carries it.

**Two gate findings, fixed here rather than filed.**  (1) The plan's own
debt-marker ratchet moved 52 → 53 the moment the C7b table row landed: the
harvester counts the series token in a row's patch path, exactly as it does
for every other phase row, so the baseline moved with the ledger note in the
same commit — no debt was added.  (2) `check_rinet2_claims` still pinned
RES-52 as `HANDED-OFF@Y7` after C7 had flipped the row to `DONE@C7`; the claim
now follows the tree, which is what a claim checker is for.  Both are in the
patch above.

---

### C8 — Win32: the dynamic-scale loader, live FAIL-CLEAN receipts, DOOM D7

**Status:** ✅ done (2026-11-05) — all five boxes landed; RES-57/58/59 flipped
with in-guest receipts.  The one honest asymmetry: the loader now *names* the
symbols it cannot bind, and the names are a work list, not a promise — Audacity
still refuses (its 302 UCRT api-set forwarders do not exist) and DOOM stops at
52 named gaps.

**Objective:** the three W32U hand-offs (RES-57, RES-58, RES-59) plus
DOOM's last phase — the part of the tree where "asserted by ledger"
still outruns "observed at runtime".

Owned rows: **RES-57** (`static-only` surfaces never called live),
**RES-58** (FAIL-CLEAN functions never observed failing live),
**RES-59** (Audacity's Outcome-B walls).

Measured: `w32/app_ledger/*.imports` carry **1694** `static-only`
markers; `docs/win32.md` generates **35** FAIL-CLEAN functions
(crypt32 8, advapi32 9, + imm32/print/network); the Audacity gap ledger
records 90932 relocations against w32run's 16384-entry buffer, a 117-DLL
graph and a 19-module/419-symbol hole; `DOOM_PLAN.md` D7 is `⬜ todo`.

- [x] **Dynamic-scale loader:** growable relocation storage (chunked,
      no fixed cap), DLL graph loading over 117 nodes with a cycle
      guard, and a symbol-gap report that names the missing imports
      instead of failing to bind. Target: Audacity reaches its message
      loop or the plan records a *new*, smaller Outcome B with numbers.
      **Landed 2026-11-05, measured** — see the progress block below: the
      relocation storage is not "growable", it is **absent** (streaming), and
      the Audacity-scale image relocates 90932 fixups in-guest and then runs.
- [x] **Live `static-only` receipts:** a fixture that resolves those
      symbols through `LoadLibrary`/`GetProcAddress` and calls a
      representative slice, so the marker count drops by measurement,
      not by relabelling.
      **Landed 2026-11-05 — 820/820, and it found three dead modules.**
      `tools/w32_static_slice.py` generates the name table from the five
      ledgers plus *both* export tables (a parser that read only the longhand
      D7 table reported 9 exported symbols as absent — measured, and the
      reason the list reads two files). The fixture loads all 20 DLLs, resolves
      all **820** named pairs (the 24 ordinal markers are named as such: there
      is no name to resolve), names every NULL, then calls 9 argument-free
      stubs + 4 real implementations with real prototypes; `test_w32_static_slice`
      is **38/38**. Found: `LoadLibraryA("imm32.dll")`, `("ole32.dll")` and
      `("uxtheme.dll")` all answered **NULL** — three modules the personality
      implements, exported by the static tables, never registered as loadable,
      so IAT binding worked while the classic delay-load shape failed
      (**the A-8 lesson a second time**). Fixed in `w32/src/w32_module.c`.
      Two crashes on the way, both recorded in the plan's error list: calling
      `msvcrt!__C_specific_handler` (CRT machinery, not a callable) and calling
      a stub shadowed by a real implementation (`user32!CreateIconIndirect`,
      CR2=0xa4) — the call slice is now stubs nothing shadows.
- [x] **Live FAIL-CLEAN receipts:** a harness that calls each of the 35
      and greps the documented failure, printing
      `[w32] FAIL-CLEAN <fn> -> <code> (as documented)`; anything that
      returns success is a real bug and becomes a box.
      **Landed 2026-11-05 — and it found one.** `w32/tests/w32c8_failclean.c`
      imports all 35 by name and calls each; `test_w32_failclean_live` is
      **42/42**. 32 of them refuse exactly as this page says, 3 are documented
      degradations (`AdjustTokenPrivileges` TRUE + ERROR_NOT_ALL_ASSIGNED,
      `DwmGetColorizationColor` S_OK + opaque black, the SENSAPI pair reporting
      a real probe) and are asserted for the behaviour they have. The bug:
      `W32_ERROR_NO_SUCH_PRIVILEGE` was **1530**, a code Windows never defines
      (`ERROR_NO_SUCH_PRIVILEGE` is 1313) — an app comparing against the
      documented constant took the wrong branch. Fixed in
      `w32/include/w32/advapi32.h`.
- [x] **DOOM D7:** build `doomgeneric_win.c` with mingw-w64 against the
      personality, run it through `/apps/w32run`, and record per-call
      implement-or-refuse decisions in `DOOM_PLAN.md`.
      **Landed 2026-11-05 — 19/19, 52 named gaps, D7 flipped.** `make doom-win`
      compiles upstream's own `doomgeneric_win.c` (80 engine sources + their
      platform layer, GPL sources fetched into `build/`, never committed; a
      one-file `<Windows.h>` shim instead of patching GPL source for a case)
      into a 948 KiB Win32 CUI binary. `w32run` relocates it with the C8
      streaming loader (1661 DIR64, no fixed cap), binds everything bindable,
      and **refuses** it naming all 52 missing symbols across 4 modules
      (msvcrt 48, GDI32 2, KERNEL32 1, USER32 1) — exit 1, no fault. The gap
      needed to become a list rather than a count, so `w32_gap_t` grew a
      128-name table and `w32_gap_at()`: `w32run` now prints one line per
      missing symbol. `DOOM_PLAN.md` D7 carries the implement/refuse decision
      per group (all four groups **implement**; nothing is decided "refuse")
      and says plainly that DOOM has not yet printed a line — the 52 are the
      work list for the next W32 phase.
- [x] Regenerate `docs/win32_run.md`'s matrix and
      `w32/tests/W32A1.bindreport` in the same commit.
      **Done 2026-11-05, and both came back byte-identical** — which is the
      honest result and worth saying out loud: C8 rebuilt the loader underneath
      the apps without moving a single run-state cell (`gen_w32_run_matrix.py
      --check` PASS, 4 apps × 6 states) and without changing one binding
      (`build/test_w32_a1 --emit-report` vs `W32A1.bindreport`: 79 lines, no
      diff). What the doc gained instead is a prose section stating what C8
      *did* change — the streaming loader, the named gap report, and the
      `imm32`/`ole32`/`uxtheme` loadability fix — plus the two new gates that
      measure the personality rather than an app.

**Progress (2026-11-05) — the first box, measured; three remain.**

| Piece | Where | Receipt |
|---|---|---|
| One relocation path for both loaders, streaming the `.reloc` directory | `w32/include/w32/w32_reloc.h`, `w32/src/w32_reloc.c`, `pe_relocations_each()` in `w32/src/w32_pe.c` | the two `static pe_reloc_t relocs[16384]` buffers are **deleted**, not enlarged; 90932 and 200000 cost zero storage |
| Module table / hold list / depth, raised by the bundle's shape | `w32/include/w32/w32_module.h` | 64 → **256** slots (117 bundle DLLs + 21 built-ins), holds 128 as their own constant with a dropped hold now **named**, depth 8 → **32** |
| Symbol-gap ledger instead of a first-miss line | `w32_bind_imports_gap()` + `w32_gap_describe()` | `w32run: gap  winmm.dll  2 symbol(s)` … `[w32] gap: 2 module(s), 3 symbol(s) missing` |
| Generated fixtures at Audacity's measured scale | `tools/w32_scale_fixture.py` | `w32scale.exe` (90932 DIR64), `w32gap.exe` (2 modules / 3 symbols), `w32type3.exe` (HIGHLOW) |
| Guest gate | `tests/integration/cases/test_w32_scale.sh` | **17/17** — `[w32] relocations: 90932 applied (DIR64), delta …, no fixed cap`, entry reached, exit 0, `too many relocations` asserted **absent** |
| `w32-core` shard, first clean run | `tests/integration/run_all.sh --group w32-core` (now 14 cases, including the three C8 gates) | **13/14 passed, 1 failed — and the failure was a finding**: `test_w32a9_registry` failed because its asm fixture hard-coded `E_NO_SUCH_PRIV 1530`, the constant C8 had just corrected; the gate had been defending the bug. Fixed, re-run **39/39** |
| `w32-gui` shard, first clean run | `tests/integration/run_all.sh --group w32-gui` | **5/6 passed, 1 failed — a second finding**: `test_w32a5_user32win` failed in its MONITOR section because the fixture read `rcMonitor.top`/`.right` (offsets +8/+12) and compared them against the **client** rect's left/top, an assertion that could only pass by luck. Fixed to compare against `SM_CXSCREEN`/`SM_CYSCREEN` — **27/27** now |
| `w32-apps` shard | `tests/integration/run_all.sh --group w32-apps` | **14/14 passed, 0 failed** (1351 s) — "ALL INTEGRATION TESTS PASSED"; the WR2/WR3/WR4 live legs skip loudly without their pinned user-supplied binaries, which is the documented behaviour |
| First post-merge CI run (`b80abca`, 2026-11-07) | `make iso` on all 20 GitHub shards | **20/20 red — four causes, none of them reproducible on a developer machine**: (1) `W32C8_SO_EXE` was declared *inside* `ifneq ($(MINGW_CC),)`, so on a runner without mingw the variable did not exist and the initrd guard `if [ -s $(W32C8_SO_EXE) ]` degenerated to the one-argument test `[ -s ]`, which is TRUE — `cp` ran one operand short: `cp: missing destination file operand after '…/w32c8_static_slice.exe'` (**16 shards**); (2) `$(PYTHON)` was never defined anywhere in the Makefile, so the static-slice table recipe ran `tools/w32_static_slice.py` directly — `Permission denied`, Error **126** (**the 3 w32 shards**); locally the rule never re-ran because a stale `build/w32_static_slice.h` was newer than its prerequisites; (3) SELFHOST SH8 pinned **137** kernel/driver C sources while the tree has **149** (**1 shard**, and only where the cached self-host closure is staged); (4) the lua payload download let `curl` exit 28 out of the recipe instead of falling through to its documented zero-byte placeholder (**2 shards**). All four fixed; `tools/check_build_hygiene.py` now fails `make test-unit` for the whole class — undefined guard names, unquoted `[ -s $(VAR) ]`, recipe command words that are undefined variables, and the three-way SH8 pin — with a 4-case negative control that injects each defect |
| DOOM D7: the Win32 build through `w32run` | `make doom-win` + `test_doom_w32.sh` | **19/19**; 948 KiB binary, 1661 DIR64 relocations, **52** named gaps over 4 modules, refused (exit 1), no fault; decisions recorded in `DOOM_PLAN.md` |
| Live static-only receipts, 820 pairs | `tools/w32_static_slice.py` + `w32/tests/w32c8_static_slice.c` + `test_w32_static_slice.sh` | **38/38** assertions; 20/20 modules load, **820/820** resolved, 9 stub + 4 real calls; found imm32/ole32/uxtheme unloadable |
| Live FAIL-CLEAN receipts, 35 functions | `w32/tests/w32c8_failclean.c` + `test_w32_failclean_live.sh` | **42/42** assertions; 35 receipts, one per function; found `ERROR_NO_SUCH_PRIVILEGE` = **1313**, not 1530 |
| Host gate | `tests/unit/test_w32_reloc.c` | **9/9** groups: 90932, 200000, early stop, counting walk, three refusals (type **3** named, fixup past span, stripped away from base) |

Findings from doing it rather than planning it:

1. **The first shape of the gap report bound the image TWICE.** Running the
   ledger pass after a failed strict bind re-resolved every failing import, so
   the loader's own diagnostics ("forwarder exports are not supported") printed
   twice and `test_w32_a1_loader` went 33/34. The ledger pass **is** the bind
   now — one walk, one truth, one line.
2. **A fixture the shell runs itself never reaches the loader.** `w32scale.exe`
   with no imports was executed by the kernel's PE path and faulted at RIP 0x1:
   its bare `ret` had no C caller. The shell routes a PE to `/apps/w32run` only
   when its import directory is non-empty (`pe_needs_w32run`), so both fixtures
   now import `KERNEL32!ExitProcess` — which is also the honest shape, since
   every real image imports something.
3. **The kernel's PE loader refuses HIGHLOW only when it relocates.** At the
   image's preferred base nothing is fixed up, so a type-3 image loads and then
   faults in user space. That is correct — nothing needed moving — and it is
   written down here rather than "fixed".

**Open in this phase:** the regenerated `win32_run.md` matrix +
`W32A1.bindreport` (box 5), then the patch cut.

**Definition of done:** three ledger rows flip with pasted in-guest
receipts; D7 flips in `DOOM_PLAN.md`; the `w32-*` shards green.

**Test gate:** `w32-core` / `w32-gui` / `w32-apps` shards; new
`test_w32_failclean_live.sh`, `test_doom_w32.sh`; the committed
bindreport regenerated and diffed.

---

### C9 — GL: the GLSL AST → TGSI back end (RES-54)

**Status:** ⬜ planned

**Objective:** replace the canned TGSI dword triangle with a real
compiler back end, so `glDrawArrays` is driven by GLSL source.

Measured: `glvirgl.c` ships hand-written dword arrays; `glsl_*` (lex,
type, parse, sema, exec) produces an interpreted AST with no back end;
`gl_backend_t.draw` is wired and `test_glvirgl` (73 checks) pins the
seam.

- [ ] AST → TGSI: declarations, temporaries, ALU ops for the subset the
      sema already proves, `DCL`/`MOV`/`MAD`/`DP4` lowering, and a
      text+dword emitter with a round-trip test.
- [ ] Feed `CREATE_SHADER` from it for arbitrary `glDrawArrays`
      batches; the canned arrays become a *fixture*, not the feature.
- [ ] A host test (`test_glsl_tgsi`) with vector cases and a negative
      control (an unsupported construct refused by name).

**Definition of done:** a GLSL vertex+fragment pair compiles to TGSI in
guest and draws through the virgl path; `gltest` stays 373/373;
RES-54 flips DONE with the receipt.

**Test gate:** `gui` shard + `make test-unit` (`test_glsl_tgsi`,
`test_glvirgl` unmodified in behaviour).

---

### C10 — close-out: the arithmetic, the honest zero, the parked M class

**Status:** ⬜ planned

**Objective:** end the series with the same arithmetic discipline every
other close-out used, and with a ledger whose non-DONE rows are only
the two classes that *cannot* be closed by code.

- [ ] Final pins: `LEDGER_ROWS`, `CLASS_PIN`, the harvest baseline row
      for this file, and `tools/check_plan_tables.py`'s own baseline.
- [ ] `docs/status.md`, `README.md`, `TODO.md`, `CHANGELOG.md` rows for
      every phase; the seven boxes get their
      `~~strikethrough~~ **Done (RESIDUE3 C#)**` receipts in TODO.md's
      own style.
- [ ] The terminal table (§5) filled with measured numbers, including
      the rows this plan could not move (M and N classes) and *why*.
- [ ] `--selftest` of C0 red on planted drift, green on the closed tree.

**Definition of done:** every checker in `make test-unit` green; the
ledger's non-DONE rows are exactly {M} ∪ {N}; nothing else.

---

## 4. What this plan deliberately does not do

- **The six PENDING-USER rows (RES-30, RES-32, RES-33, RES-48, RES-49,
  RES-56).** No code closes them: they are measurement packages waiting
  for the user's silicon, and "pending-user" is a status, not a failure.
  C10 documents them as the plan's honest boundary. (RES-49's live-web
  fetch is *reproducible* under QEMU user-net — this audit measured real
  DNS in the boot log — but the PQ-hybrid number must come from a real
  path, so the row stays.)
- **The four RE-AFFIRMED non-goals (RES-43, RES-44, RES-50, RES-51).**
  Deferred W32 surfaces, registry/COM/DirectX decisions, happy-eyeballs,
  QUIC/DTLS — re-affirmed with their D-numbers; this plan does not
  reverse a recorded decision.
- **Self-hosting SH9.** The cross-arch in-guest toolchains are
  `SELFHOST_PLAN.md`'s own successor series with its own opener
  (one tcc per `--cpu`), not a residue sweep.
- **The hobby-grade FS edges (FSFULL F6).** JBD2 recovery, btrfs
  subvolumes, f2fs GC, NTFS writing stay refused by their own plan.
- **The OTA and LX parked items.** They are bullets with reasons in
  TODO.md by the plans' own convention; none of them becomes a box here.
- **Re-opening a completed plan.** A bug found while landing C* is that
  phase's bug; it never becomes a secret twelfth phase.
- **Performance re-tuning (OPT).** If a phase moves a number, the number
  is recorded in that phase; no global re-benchmark is scheduled.

## 5. Predicted terminal arithmetic (filled at close)

| Metric | Audit (2026-11-03) | Predicted at close |
|---|---|---|
| Ledger rows, non-DONE | 20 | **10** = 6 PENDING-USER + 4 RE-AFFIRMED (the two classes no code can move) |
| Ledger rows, OPEN | 1 (RES-54) | **0** |
| Ledger rows, HANDED-OFF | 10 | **0** (each flips DONE or narrows into a scheduled row) |
| Ledger total rows | 59 | **61+** (RES-60 isoc streaming, RES-61 BT/Wi-Fi silicon; +1 per narrowing that C0 forces) |
| `TODO.md` unchecked boxes | 7 | **0** |
| `docs/status.md` 🚧 rows | 4 | **2** (Bluetooth and Wi-Fi stay 🚧 for their silicon half, citing RES-61) |
| Plan tables with a stale row | 2 files / 6 rows | **0** |
| Ledger rows whose phase claimed closure | 3 | **0** |
| Phases ✅ | 0/11 | **11/11** |

## Workflow (mandatory for every phase)

The GL_PLAN.md / GL2_PLAN.md / RESIDUE2 loop, unchanged:

```
1. READ    — read ALL affected files before writing anything
2. PLAN    — this file: Status → IN PROGRESS on the phase
3. DESIGN  — show struct/API changes, list callers; cite a ledger row
             instead of re-arguing
4. IMPL    — code → host test → in-guest gate → docs
5. BUILD   — zero warnings, -Wall -Wextra -Werror
6. TEST    — host gate; QEMU gate; existing suites unmodified
7. DOCS    — this file's Result + table tick, TODO.md Done receipt,
             CHANGELOG.md, baselines and ledger pins moved in the
             same commit
```

Deliver as `.patch` files against the explicit base, exactly like every
series in this tree: `git apply --check` on a pristine clone before
presenting.
