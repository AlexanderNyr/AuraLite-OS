# MATURITY_PLAN.md — audit

Checked against the tree, not against the document. Every claim below was
verified by running something or reading the code it refers to.

Date of audit: the tree at `U8 update` + the USB U9 work.

---

## 1. Verdict in one table

| Phase | Plan says | Actually | Verdict |
|---|---|---|---|
| M1 — FPU/SSE context switch | ✅ complete | `fxsave`/`fxrstor` in `context.asm`, `fpu_valid` + 16-byte-aligned `fpu_area[512]` in the TCB | ✅ **correct** |
| M2 — IOAPIC | ✅ core complete | `ioapic_init()` called from `kernel.c:252`; MSI and virtio-RX correctly left unchecked | ✅ **correct, honestly scoped** |
| M3 — uaccess | ❌ "pending" | **Already landed**: `usercopy.h`, `tests/unit/test_uaccess.c` (in the Makefile), `tests/integration/cases/test_uaccess.sh` all exist; the patch no longer applies | ⚠️ **status wrong** |
| M4 — demand-paged/shared VMAs | pending | `shmem.c` exists for `MAP_SHARED\|MAP_ANONYMOUS`; no demand-paging fault path | ✅ correct |
| M5 — POSIX process model | ✅ complete | `do_setsid`/`do_setpgid`/`do_getpgid` wired in `syscall.c` | ✅ **correct** |
| M6 — Production TCP | pending | Partially done already: `cwnd`, `ssthresh`, `rto_ms`, SRTT are in `tcp.c`. **No** fast retransmit, dup-ACK counting, SACK, Nagle or delayed ACK | ⚠️ **understates progress** |
| M7–M9, M11–M14 | pending | Consistent with the tree | ✅ correct |
| M10 — USB transfer engine | pending | **Overtaken by `USB_PLAN.md`**: xHCI rings/slots/route strings, EHCI periodic + TT fields, Isoch TRB type 5 all landed in U0–U9 | ⚠️ **stale** |

---

## 2. The findings that matter

### 2.1 Two integration gates are dead code — they cannot pass

`test_uaccess.sh` (M3) and `test_mmap_shared.sh` (M4) both call an API that
**does not exist**:

```
tests/integration/cases/test_uaccess.sh: line 14: il_run: command not found
```

They use `il_run`, `il_assert`, `il_assert_shell_alive` and `$IL_SERIAL`.
The library (`tests/integration/lib/lib.sh`) provides `il_run_qemu`,
`il_assert_grep`, `il_pass`/`il_fail`, `il_summary` — and no `IL_SERIAL` at
all. These gates have never run. M3's deliverable claims a test battery
that has never executed once.

This is the same failure mode `USB_PLAN.md` U0 was written to attack: a
guarantee that exists on paper only. It is worse here, because the phase in
question is the **security** predicate — "a hostile user pointer returns an
errno instead of panicking".

### 2.2 27 test cases are never run by CI

`run_all.sh` hardcodes a 97-entry `ALL_CASES` list. There are **124** case
files on disk. The 27 unregistered ones:

```
test_3d_render          test_procfs             test_usb_hub_full
test_devfs              test_shell_all          test_usb_isoc
test_diskfs             test_sysmon_data        test_usb_printer
test_fat32_mkdir        test_tmpfs              test_usb_string
test_mmap_shared        test_uaccess            test_userspace_apps
test_process_spawn_many test_usb_audio_full     test_virgl_gpu
test_usb_cdc_acm        test_usb_driver_registry
test_usb_full_stack     test_usb_hid_input      test_usb_hub_depth
test_xhci_address       test_xhci_bulk          test_xhci_control
test_xhci_interrupt
```

**Five of those are mine** — the U3–U9 gates (`test_xhci_address`,
`test_xhci_bulk`, `test_xhci_control`, `test_xhci_interrupt`,
`test_usb_hid_input`, `test_usb_hub_depth`). I wrote them, verified them by
hand, and never registered them. They would not have protected anything on
a CI run. That is my omission and it belongs in this list, not in a
footnote.

The plan's own M-phase gates land in the same hole: writing
`patches/MAT_Mn_*.patch` with a new case file does nothing unless
`ALL_CASES` is edited too. Nothing in the repository enforces that.

### 2.3 Sampling the unregistered cases: 5 of 10 are red

| Case | Result |
|---|---|
| `test_devfs` | ✅ 2/2 |
| `test_procfs` | ✅ 3/3 |
| `test_tmpfs` | ✅ 9/9 |
| `test_usb_string` | ✅ 3/3 |
| `test_uaccess` | 💥 broken API |
| `test_mmap_shared` | 💥 broken API |
| `test_diskfs` | ❌ 1 of 3 failed |
| `test_fat32_mkdir` | ❌ 2 of 4 failed |
| `test_usb_isoc` | ❌ 2 of 6 failed |
| `test_usb_cdc_acm` | ❌ 1 of 6 failed |

So the "118/118 green" figure that `USB_PLAN.md` U9 and the project's own
status reporting lean on describes **97 registered cases**, not the suite.
Roughly a fifth of the tests in the tree are outside it, and half of that
sample is failing.

### 2.4 A stale claim I left behind in U0–U9

`test_usb_isoc` fails on:

```
[xhci] self-test: halted=0 CNR=0 (bring-up only)
```

`xhci_self_test()` still says **"bring-up only"** after U3–U9 made slots,
control, bulk, interrupt and nested hubs real. U0's whole point was that
the log must not overstate — this is the same sin in the other direction,
understating, and it makes a test disagree with reality. My omission.

### 2.5 M3's status line is simply wrong

The header says *"M3, M4, M6–M14 pending"* and the table row says
`pending | —`, but `patches/MAT_M3_uaccess.patch` exists, its files are in
the tree, and it no longer applies (already applied). The task checkboxes
under M3 are also all `- [ ]`. Whoever landed M3 did not update the plan.

### 2.6 M10 has been overtaken and should be retired

M10 asks for xHCI rings, slot addressing, endpoint contexts, route-string
addressing, EHCI splits and isoc. `USB_PLAN.md` U0–U9 delivered all of it
except what is documented as absent there. Leaving M10 as "pending" will
cause someone to redo finished work. It should be rewritten as a pointer to
`USB_PLAN.md` with only its genuinely-unclosed items kept:

- USB Audio actually moving samples (isoc end to end)
- writable `/usb` FAT32 automount + ext2 hotplug
- `test_usb_msc.sh` per controller

---

## 3. What the plan gets right

Worth saying, because most of it is sound:

- **D1/D2** (correctness before completeness; finish the deepest items) are
  the right priorities, and M1/M2/M5 were done in that order.
- The 🧪 inventory in §1.1 is accurate where I checked it.
- M2's checkboxes are **honestly** split: IOAPIC ticked, MSI and virtio-RX
  explicitly deferred rather than quietly bundled.
- M11's framing of the GLSL→TGSI work as "a compiler phase, not a rendering
  tweak" is exactly the kind of scope honesty that stops a phase from
  silently overrunning.
- Every phase has a deliverable patch name and a test gate, which is the
  structure that made `USB_PLAN.md` executable.

---

## 4. Recommended fixes, in order

1. **Repair the two dead gates** (`test_uaccess.sh`, `test_mmap_shared.sh`)
   against the real `lib.sh` API. Until then M3 and M4 have no evidence.
2. **Register all 27 orphan cases** in `run_all.sh`, and add a check that
   fails when a file in `cases/` is missing from `ALL_CASES` — the same
   drift guard `tools/check_usb_claims.py` applies to documentation.
   Otherwise this recurs.
3. **Triage the 4 failing unregistered cases** — decide per case whether the
   test or the code is wrong, exactly as U0 did.
4. **Correct M3's status** to complete (or list what is genuinely missing
   from its audit task).
5. **Fix `xhci_self_test()`'s "bring-up only"** string and re-run
   `test_usb_isoc`.
6. **Rewrite M10** as a reference to `USB_PLAN.md` plus its three unclosed
   items.
7. **Restate M6** to credit what exists (cwnd/ssthresh/RTO) and scope it to
   what does not (fast retransmit, SACK, Nagle, delayed ACK, backlog,
   TIME_WAIT).

Items 1–3 are the load-bearing ones: they are the difference between a test
suite that reports the truth and one that reports 118/118 while a fifth of
it never runs.
