# AuraLite OS — RESIDUE2 Plan (close the remainder: every TODO box and every open ledger row, scheduled and machine-checked)

## Status: IN PROGRESS — T0–T2 landed; T3–T9 specified

| Phase | Result | Deliverable |
|-------|--------|-------------|
| T0 — the rig: the coverage checker | ✅ done (366c850) | `patches/RESIDUE2_T0_rig.patch` |
| T1 — kernel core (memory, reaping, processes, SMP-safety) | ✅ done | `patches/RESIDUE2_T1_kernel.patch` |
| T2 — interrupts and discovery (MADT overrides, AP wake) | ✅ done | `patches/RESIDUE2_T2_irq.patch` |
| T3 — storage (AHCI breadth, fsck tooling, block cache, btrfs CRC) | ⬜ planned | `patches/RESIDUE2_T3_storage.patch` |
| T4 — VFS and POSIX (canonicalisation, VMAs, libc/TTY gaps) | ⬜ planned | `patches/RESIDUE2_T4_posix.patch` |
| T5 — network and TLS (blocking edges, production TCP, idle RX, HTTPS) | ⬜ planned | `patches/RESIDUE2_T5_net.patch` |
| T6 — devices beyond QEMU (OHCI/EHCI/xHCI/HID, BT, Wi-Fi, modern NICs) | ⬜ planned | `patches/RESIDUE2_T6_devs.patch` |
| T7 — GUI (isolation, clipboard, settings, apps) | ⬜ planned | `patches/RESIDUE2_T7_gui.patch` |
| T8 — ports and oddities (RES-02, RES-06, RES-18) | ⬜ planned | `patches/RESIDUE2_T8_ports.patch` |
| T9 — tooling and close-out (GDB, flakiness, arithmetic) | ⬜ planned | `patches/RESIDUE2_T9_close.patch` |

## 1. Where this plan comes from

Two instructions, one audit.  The user's instruction after GL2 closed:
find everything in the project that can be written into TODO, and make
a plan that closes ALL of it.  The audit re-swept the tree the way the
R12 sweeps did, but downwards instead of upwards: not "which claims
are stale" (that was R12's question) but "which open debts exist only
as prose and can therefore hide".  Answer: **twenty of them**, now
boxes in TODO.md, alongside the twenty that were already counted.  The
audit also caught two bookkeeping defects and fixed them in place: the
TODO header still said the ledger had 48 rows (it grew to 54), and the
ledger's closing prose listed RES-26 among the OPEN rows while its own
row says DONE@Y4 — the row is the truth.

Measured at audit time (2026-09-04, this tree):

| Source | Count | Notes |
|---|---|---|
| TODO.md unchecked boxes | **40** | 20 pre-existing + 20 newly boxed from prose |
| Ledger rows OPEN | **6** | RES-02/06/07/16/18/54 |
| Ledger rows PENDING-USER | **4** | RES-30/32/33/48 — need the user's real hardware; see §4 |

Every unchecked box now ends with the `(RESIDUE2 T#)` tag of the phase
that closes it.  That tag is the contract this plan cannot dodge:
`tools/check_residue2_claims.py` (T0) fails the build when a box is
untagged, names a phase this plan does not have, or when a phase has no
boxes and no named ledger rows.

## 2. The coverage contract

| Rule | Enforced by |
|---|---|
| Every `- [ ]` box in TODO.md carries exactly one `(RESIDUE2 T#)` tag | T0 checker |
| The tagged phase exists and is one of T0–T9 | T0 checker |
| Every phase T1–T8 is tagged by ≥ 1 box or names its OPEN ledger rows in this file | T0 checker |
| Every OPEN ledger row id appears in this file | T0 checker |
| A phase may flip to ✅ only with its Result filled and its gate green in the same commit | the GL2 L7 convention, re-checked each flip |
| TODO.md marker count ratchets against `tools/residue_baseline.txt` | the existing residue ratchet |

Closing a box means: the work lands, the box gets its `~~strikethrough~~
**Done (...)**` receipt in TODO.md's own style, and the tag disappears
with the checkbox — the count only goes down through a landed patch.

## 3. Phases

### T0 — the rig: the coverage checker

**Status: ✅ COMPLETE**

**Objective:** make it impossible for this plan and TODO.md to drift,
the way GL2's L0 made the GL2 plan undodgeable.

Design: `tools/check_residue2_claims.py` + `tests/unit/test_residue2_claims.sh`
wired into `make test-unit` next to the GL2 wrapper.  The claims are the
coverage contract in §2.  `--selftest` runs against a doctored empty
tree and must fail there — a checker that never fails checks nothing.

Tasks:

- [x] The checker + wrapper + Makefile wiring, selftest green.
- [x] The plan's phase table and Status header agree with reality from
      day one (PLANNED ⇔ zero ✅ rows).

**Definition of done:** `make test-unit` runs the wrapper; a stripped
tag from any TODO box turns the build red.

**Test gate:** the checker green on this tree, selftest catches a
planted miss.

**Result:** DONE (commit 366c850). `tools/check_residue2_claims.py`
verifies 21 live claims; `tests/unit/test_residue2_claims.sh` runs in
`make test-unit` and its planted-violation selftest fails as required.

---

### T1 — kernel core: memory, reaping, processes, SMP-safety

**Status: ✅ COMPLETE**

**Objective:** close the seven kernel-side boxes that need no new
hardware and no new ports — the deepest, least glamorous cluster, and
the one the demos hide least.

Boxes (tags): large pages for selected kernel mappings (T1); wire
`paging_free_address_space` reaping for all zombies once TLB shootdown
+ per-PML4 refcounting land (T1); real parent/child process table and
precise `waitpid` semantics (T1); POSIX-completeness for
`fork`/`execve`/`wait4` beyond demo needs (T1); native errno into the
disk FS drivers and `process.c` (T1); IRQ/syscall entry stubs maintain
16-byte C-ABI alignment (T1); the SMP-safety sweep — atomic OFD
refcounts, per-vnode write lock for O_APPEND, atomic `sig_pending`,
per-CPU SYSCALL state (T1).

Design rules:

| Area | Rule |
|---|---|
| Large pages | Selected mappings only (kernel image, HHDM): a 2 MiB path with the 4 KiB fallback; measured in boot ticks or it did not happen |
| Reap wiring | Lands WITH its dependency (shootdown + refcount) in the same phase, never half-wired |
| Stub alignment | Fix the stubs, then DELETE the M5 runtime-aligned scratch workaround in the same commit — a workaround that survives its cure is a second bug |
| SMP sweep | One locking-design paragraph per structure before code; each guarded by a stress test that fails on the unpatched tree when run under `-smp 4` |

**Definition of done:** all seven boxes carry Done receipts; the SMP
stress gates run in CI.

**Test gate:** `make test-unit` EXIT 0; the SMP stress case green under
`-smp 4`; no new QEMU regressions.

**Result:** DONE. All seven boxes carry receipts in the tree: the
large-page audit prints measured cycles at boot (2051 × 2 MiB kernel-half
leaves, 328532 cycles); zombie reaping calls `paging_free_address_space`
with the precise O5 `tlb_shootdown_range(cr3, 0, 0)` wired in the same
phase; the parent/child process table backs precise `waitpid`/`wait4`
(rusage) semantics; `waitid` and POSIX wait-completeness landed with the
libc headers; the disk FS drivers and `process.c` return native errno;
`isr_common_stub` aligns RSP and the syscall entry pads its C calls —
the M5 runtime-aligned fxsave scratch is deleted; the SMP sweep shipped
atomic `sig_pending` RMWs, the per-vnode O_APPEND lock, atomic OFD
refcounts and per-CPU SYSCALL state. Gates: `make test-unit` EXIT 0
(ratchets hold, 69/69); `tests/integration/cases/test_smp_procstress.sh`
green under `-smp 4` (O_APPEND 6×200 intact, 100 precise fork/wait
cycles, 8×10 counted signal deliveries); `test_signals` and
`test_selftest` green. Two latent kernel bugs fell out of the stress
gate and are fixed here: `kernel_nanosleep` truncated sub-tick sleeps
to zero ticks (usleep < 10 ms returned instantly), and the syscall
entry's signal-check calls ran on a misaligned stack (`fxsave` #GP in
`build_handler_frame` on the first cross-process delivery).

---

### T2 — interrupts and discovery

**Status: ✅ COMPLETE**

**Objective:** the machine stops being QEMU-hardcoded where the ACPI
tables already carry the truth, and the one unproven SMP interrupt
path gets its receipt.

Boxes: Interrupt Source Overrides from the ACPI MADT (T2).  Ledger
rows: RES-16 (a device IRQ waking a hlt-ed AP — receipt line in an SMP
case).  Note MSI/MSI-X itself is RES-36's, outside this plan's scope
(§4) — the ports/NIC cluster of RES-36/RES-46 overlaps T6 only where a
box names it.

Tasks:

- [x] Walk RSDP→RSDT/XSDT→MADT ISO entries; program the I/O APIC
      redirections from them; keep the QEMU defaults as the fallback
      and print the agree/disagree line the RES-37 check established.
- [x] RES-16 receipt: an integration case that parks an AP in `hlt`,
      raises a device IRQ targeted at it, and greps the wake.

**Definition of done:** a machine whose MADT disagrees with the PC
standard boots with the MADT's routing; RES-16 closes with its receipt.

**Test gate:** existing integration cases unchanged-green; the new AP
wake case green.

**Result:** DONE. `ioapic.c` walks the MADT once for both the I/O APIC
base (the RES-37 agree line, unchanged) and the type-2 Interrupt Source
Overrides; the redirection table is programmed from the ISO list with
the table's polarity/trigger bits, the PC-standard defaults (PIT→GSI2,
identity, edge-high) remain the fallback, and every divergence prints by
name at boot (`[ioapic] overrides: ...` summary + per-override lines).
The RES-16 receipt is `tests/integration/cases/test_irq_ap_wake.sh`:
`SYS_IRQ_AP_WAKE` (604) parks a `hlt` looper directly on cpu 1's run
queue, aims the RTC periodic interrupt (ISA IRQ8 → GSI8) at that AP's
APIC ID via the new `ioapic_route_gsi()`, and the boot log carries
`[smpwake] PASS: 9 RTC(GSI8) device IRQs delivered to cpu1 (apic id 1);
hlt looper woken 8 times — RES-16 receipt` (the handler asserts the
delivery cpu; a stray delivery anywhere else fails the gate). Gates:
the new case green under `-smp 4`; boot-to-shell smoke, SMP stress
(5/5), `test_signals`/`test_selftest` unchanged-green; `make test-unit`
EXIT 0 with the RES-37 checker pin moved to the new walk in the same
commit. QEMU note: its MADT carries no ISOs, so the NULL machine takes
the fallback path and prints it — the agree/disagree line names that
too.

---

### T3 — storage

**Status:** not started

**Objective:** the storage stack stops being QEMU-shaped and stops
writing known-zero checksums.

Boxes: broaden AHCI beyond the QEMU test path (T3); fsck/recovery
tooling or defensive consistency checks for FAT32/ext2 (T3); block
cache + writeback policy adoption — the layer exists on x86_64, the
ports wait (ledger RES-07) (T3); Btrfs on-disk SHA-256 checksums,
written as zeros today (T3).

Design rules: the btrfs checksum work needs an in-kernel SHA-256 (D2
keeps the kernel off libatls — respect it, vendor a small kernel-local
implementation with test vectors); fsck tooling means CHECKS first
(a `--verify` mode or boot-time consistency walk with named
diagnostics), repair second; AHCI breadth is measured by what ELSE it
boots on, so the gate is a matrix, not a unit test.

**Definition of done:** four boxes closed; RES-07 closes with the
objects on the three shared port lists and the link green.

**Test gate:** the FS stress/shard cases green; a new btrfs checksum
vector test; fsck-mode tests for FAT32/ext2.

**Result:** —

---

### T4 — VFS and POSIX

**Status:** not started

**Objective:** the biggest cluster: the POSIX/VFS/libc gaps that make
AuraLite behave differently from every real system — named, bundled,
and scheduled instead of scattered through six prose sections.

Boxes: VFS path canonicalisation (T4); installation allowlist resolves
symlinks through the VFS (T4); lazy VMAs + file-backed `MAP_SHARED`
write-back (T4); `execvpe`/`fexecve` proven by an in-tree test (T4);
`epoll` on `select()` (T4); the TTY/stdio bundle — `scanf`, readline,
`/dev/ttyS0`, true VMIN/VTIME, column tracking (T4); libm last-ULP
review, float variants, errno domain errors (T4); IPC completion —
shared memory, hard links, persistent FIFO/symlink storage, full
symlink path following (T4); keyboard dead keys (T4).

Design rules:

| Area | Rule |
|---|---|
| Canonicalisation FIRST | The allowlist symlink fix consumes it — one phase, two boxes, right order |
| VMAs | Lazy fault-backed VMAs land before file-backed MAP_SHARED; the shared anonymous objects (shmem.c) already exist and are the template |
| TTY bundle | One line-discipline pass, not five patches; `/dev/ttyS0` rides the same discipline |
| epoll | On top of select() as documented; no new syscalls beyond the epoll triple |

**Definition of done:** nine boxes closed; the POSIX2024 matrix gains
entries rather than losing any.

**Test gate:** new host/guest tests per item; `make test-unit` EXIT 0;
the install-policy checks (insttest) green.

**Result:** —

---

### T5 — network and TLS

**Status:** not started

**Objective:** the net stack's honest edges — named in prose for
months — become scheduled work: blocking semantics, production TCP
behaviour, the idle RX drain, and the TLS 1.3 stack the crypto layer
was built for.

Boxes: remaining socket edge cases fully blocking (T5); production
TCP — sliding windows, congestion control, real packet queues (T5);
e1000 RX ring drains while idle (T5); TLS 1.3 handshake + record
layer, certificate validation (RSA-PKCS#1v1.5), HTTPS client —
INTERNET_PLAN N3–N6 (T5).

Design rules: the e1000 drain lands FIRST (it makes every long
integration run readable, which pays for the rest of the phase);
TCP work keeps the one-segment path as the documented fallback and
gates on the realinternet cases; the TLS cluster is INTERNET_PLAN
N3–N6's scope continuing under this plan's scheduling — its RFC-vector
test style is the gate.

**Definition of done:** four boxes closed; `rust-lang.org` over HTTPS
fetches from the guest or the phase says exactly which cipher/curve
refused.

**Test gate:** `test_socket_errno`, `test_https*`, net integration
shards green; new TCP throughput/ordering cases.

**Result:** —

---

### T6 — devices beyond QEMU

**Status:** not started

**Objective:** the driver cluster the ledger already measured: the USB
transfer schedulers, the two wireless transports, the modern NICs, and
writable USB storage.

Boxes: OHCI ED/TD transfer scheduling (T6); EHCI async/control/bulk
qTD + MSC backend (T6); xHCI command/event/transfer rings, slot
addressing, endpoint contexts (T6); generic HID report parsing and
OHCI/EHCI/xHCI HID transport (T6); Bluetooth USB transport + one
tested HCI controller path (T6); Wi-Fi chipset backend for the 802.11
MAC layer (T6); vmxnet3 / e1000e data-path drivers (T6); writable
FAT32 on USB, ext2 hotplug automount, isochronous devices (T6).

Design rules: uhci.c (555 lines) is the reference implementation per
RES-38's opener; e1000.c is the NIC reference per RES-46; every
controller lands with its QEMU device gate (`-device usb-ehci`,
`vmxnet3`) so "compiles" never passes for "works"; hardware-only
claims are loud-skipped in CI without the device (D2 convention).

**Definition of done:** eight boxes closed; RES-38 and RES-39 close
with receipts; RES-46's NIC half closes (its NVMe half is explicitly
NOT here — see §4).

**Test gate:** new per-controller integration cases with device-skip;
the existing USB case stays green.

**Result:** —

---

### T7 — GUI

**Status:** not started

**Objective:** RES-47's three boxes plus the apps gap — the compositor
stops being educational by measurement, not by adjectives.

Boxes: stronger compositor/client isolation and permission model (T7);
text input, clipboard and focus behaviour (T7); persisted user
settings/theme (T7); more GUI apps and richer editor/terminal (T7).

Design rules: RES-47's opener stands — gui_syscalls.c already gates 36
syscall cases behind require_owner/require_icon_owner, so the series
starts at a clipboard ACL, not at zero; settings persistence rides the
existing FS layout (a dotfile convention, no new daemon); new apps
follow the glcube/glshade packaging convention (Makefile hooks +
initrd + README row).

**Definition of done:** four boxes closed; RES-47 closes with
receipts.

**Test gate:** the GUI integration cases (bad-pointer, cleanup, dirty
rect) green; new ACL negative cases.

**Result:** —

---

### T8 — ports and oddities

**Status:** not started

**Objective:** the three OPEN rows that are neither subsystem work nor
tooling: the two narrowed oddities and the port-coupling remainder.
No TODO boxes tag this phase — its contract is the ledger rows
themselves.

Ledger rows: RES-02 (`-cpu max` shell-banner oddity: shell starts, the
first SYS_WRITE never lands; smoke explains it or the line appears);
RES-06 (the fd/OFD/pipe machinery is the x86-coupled remainder of
vfs.c — it compiles portable or its coupling is re-affirmed in writing
as the x86 process layer's); RES-18 (PIE loading on ports, waiting on
RES-17's ARM64 close — a PIE binary runs on one tenant, receipt).

Design rules: these are DIAGNOSIS-first phases; the receipt each row
names is the deliverable, and "re-affirmed as architecture" is an
honest close for RES-06 if the coupling is load-bearing.

**Definition of done:** RES-02, RES-06, RES-18 leave OPEN with their
named receipts.

**Test gate:** the port boot shards (rv64/a64/i386) green; the new
smoke case for RES-02 green or the oddity fixed.

**Result:** —

---

### T9 — tooling and close-out

**Status:** not started

**Objective:** the developer-experience boxes and the terminal
arithmetic, in the GL2 L7 style.

Boxes: GDB helper scripts / pretty-printers for kernel structures
(T9); integration-test timing flakiness around process spawn and
serial pacing (T9); CI screenshots artifact (T9).

Close-out duties: §5 of this plan filled with grep-backed arithmetic
(box count 40 → whatever closed it, OPEN rows 6 → whatever closed);
CHANGELOG one entry per phase; the residue ratchet baselines moved in
the landing commits; the status.md cell for whatever the series
touched stays honest; the checker flips with the last phase.

**Definition of done:** three boxes closed; the arithmetic section
filled; the coverage checker reports zero untagged boxes remaining —
TODO.md empty of open debt, every close receipt-carrying.

**Test gate:** `make test-unit` EXIT 0; integration shards green with
the flakiness fixes.

**Result:** —

---

## 4. What this plan deliberately does not do

- **The PENDING-USER rows (RES-30, RES-32, RES-33 and RES-48).** They
  are measurement packages waiting for the user's real hardware; no
  code closes them.  They stay PENDING-USER and outside every phase.
- **RES-54 (GLSL AST → TGSI).** It is a compiler plan, not residue
  sweep work — GL2's D7 hand-off.  It gets its own successor plan and
  its own opener; this plan only records the hand-off.
- **RES-36 (MSI/MSI-X) as a phase.** The IRQ-relevant slice this plan
  owns is T2's; the MSI/MSI-X data-path work rides with the driver
  phases that would consume it and is not scheduled here.
- **NVMe (RES-46's storage half).** T6 closes the row's NIC half
  (vmxnet3/e1000e, the boxes that exist); NVMe has no TODO box and
  gets none by this plan.
- **The hobby-grade FS edges (FSFULL_PLAN F6).** JBD2 recovery,
  subvolumes, GC, NTFS writing — named, refused, stays refused.
- **Re-opening completed plans.** Bugs found while landing T* are bugs
  in the phase that found them, never a secret phase eleven.

---

## 5. Terminal arithmetic (filled at close)

| Metric | Audit (2026-09-04) | At close |
|---|---|---|
| TODO.md unchecked boxes | 40 | |
| Ledger rows OPEN | 6 | |
| Ledger rows PENDING-USER | 4 (not ours) | 4 |
| Phases ✅ | 0/10 | 10/10 |

---

## Workflow (mandatory for every phase)

The GL_PLAN.md / GL2_PLAN.md loop, unchanged:

```
1. READ    — read ALL affected files before writing anything
2. PLAN    — this file: Status → IN PROGRESS on the phase
3. DESIGN  — show struct/API changes, list callers; cite a ledger row
             instead of re-arguing
4. IMPL    — code → host test → /gltest-style gate → docs
5. BUILD   — zero warnings, -Wall -Wextra -Werror
6. TEST    — host gate; QEMU gate; existing suites unmodified
7. DOCS    — this file's Result + table tick, TODO.md Done receipt,
             CHANGELOG.md, baselines moved in the same commit
```

Deliver as `.patch` files against the explicit base, exactly like
every series in this tree: `git apply --check` on a pristine clone
before presenting.
