# AuraLite OS — Realtek Gigabit NIC Driver Plan (the `r8169` family)

## Status: COMPLETE — RT0 ✅ RT1 ✅ RT2 ✅ RT3 ✅

> This is a feature plan in the style of `FSFULL_PLAN.md`, `OTA_PLAN.md`
> and `LX_COMPAT_PLAN.md`, written against the tree as it stands.  It
> follows the same structure: dependency-ordered phases, a definition of
> done and a test gate for every phase, one `.patch` per phase.
>
> The 100 Mbit Realtek **RTL8139** already has a full, CI-gated driver
> (`drivers/rtl8139/`, landed 2026-08-26 as the "RTL8139 update").  Its
> gigabit sibling — the **RTL8169/8168/8111** family, the chip Linux
> calls `r8169` and the most common onboard NIC on real motherboards —
> is catalogued but driverless, and it is the *one* NIC in the
> virtual-hardware catalog that QEMU cannot emulate.  So this plan
> **inverts the gate** the 8139 used: the 8139 proved its data path
> under `-device rtl8139`; the 8169 has no such device, so the data path
> is proved against a **register-level model of the chip running on the
> host**, and real silicon becomes the M-class paste-back.  It is the
> same discipline the tree already applies one level down — the
> arithmetic-first host gate (`tcp_x5.h`, `rtl8139_ring.h`) extended
> from "the arithmetic" to "the whole chip".

**Baseline:** commit `c2aff92` (the "L5 update"; the post-LX tree —
`net_init` probes e1000 → virtio-net → vmxnet3 → e1000e → rtl8139, and
the 8169/8168 rows sit in the vmdrv catalog as "known / no data path").

**Ledger integration:** RT0 adds no ledger rows (coverage registers at
RT3, the LX L5 / OTA O5 / FSFULL F7 precedent).  The real-silicon
confirmation is an M-class row `PENDING-USER@RT3` — the RES-30/32/33/48
discipline ("package slot ships; user paste-back is the number's only
source"), because an onboard 8168/8169 (or a PCI-passthrough'd one) is
the only place the true data path can ever be observed.

---

## 1. Where things actually stand

Measured on the baseline tree, not assumed from headers.

### 1.1 The 8139 is done; the 8169 is the gap

* `drivers/rtl8139/` is a complete data path: port-I/O register file via
  BAR0, an 8 KiB RX **ring buffer** walked through CAPR/CBR, four
  hardware TX descriptors round-robin, INTx-driven RX that wakes
  sleepers (receipt `[rtl8139] RX via IRQ wake`), runt padding to
  60 bytes, and an explicit refusal when a DMA buffer lands above the
  4 GiB the 32-bit `RBSTART`/`TSAD` registers can express.  Gated under
  QEMU (`test_rtl8139.sh` 18/18: DHCP + ICMP + DNS + 16 concurrent TCP)
  and on the host (`test_rtl8139_ring.c`, 203 checks pinning the ring
  arithmetic and its two bug classes).
* `net_init()` (`kernel/net/net.c`) probes **e1000 → virtio-net →
  vmxnet3 → e1000e → rtl8139**; the comment states the priority rule:
  *paravirtual before emulated-gigabit before 100 Mbit*.  The first
  `netdev_register()` wins.
* The gigabit family is catalogued as known-without-a-data-path in three
  places that must all flip at close-out:
  - `drivers/vm/virtual_drivers.c`: `10ec:8168 "Realtek RTL8168/8111
    Gigabit" r8169 "known / no data path"` and `10ec:8169 "Realtek
    RTL8169 Gigabit" r8169 "known / no data path"` — note the key
    already says `r8169`, the Linux driver name;
  - `docs/status.md`: *"vmxnet3 / e1000e / RTL8169 🚧 — recognised by
    the virtual-driver probe, but no data path yet.  RTL8169/8168 is a
    different chip from the 8139 (descriptor rings, not a ring buffer)
    and QEMU does not emulate it, so it could not be gated."*;
  - `docs/virtual_driver_matrix.md` (the "Alternative NICs" known table)
    and `docs/driver_guide.md` ("Not supported").
* The chip is a **different machine** from the 8139: descriptor rings
  with an OWN-bit ownership hand-off (not a ring buffer with CAPR/CBR),
  an MMIO register file, a MAC loaded through IDR0–5 + MAR0–7, and
  separate TX/RX-config, command, status and interrupt-mask registers.
  The exact offsets and fields are **pinned in RT1** from the RTL8169
  datasheet with Linux's `r8169.h` as the cross-check — they are not
  recalled into this plan.

### 1.2 The gating problem, measured

* QEMU emulates the 8139 (`-device rtl8139`, the harness's `IL_NIC`
  knob) but has **no 8169/8168 model** — there is no `-device r8169`,
  and `IL_NIC` can only select e1000 / e1000e / vmxnet3 / virtio-net /
  rtl8139.  The 8139's gate shape (boot the guest, assert the DHCP +
  ICMP + DNS + TCP receipts over the NIC) is therefore unavailable for
  the 8169 by construction — the repo's own docs already say so, and
  the plan does not pretend otherwise.
* The tree already has two honest mechanisms for code QEMU cannot reach:
  1. the **D2 "pure-C core, host-gated" pattern** — the arithmetic that
     is easy to get wrong lives in a header with no hardware in it, so a
     HOST test drives the cases QEMU cannot be asked to produce
     (`tcp_x5.h`/`tcp_cc.h` for TCP; `rtl8139_ring.h` + 203 checks for
     the NIC arithmetic, with a negative control that must fail);
  2. the **M-class metal receipt** (`docs/metal_receipts.md` slots 1–9;
     ledger RES-30/32/33/48, status `PENDING-USER@Rn`).
* This plan uses both, in order: RT1/RT2 extend mechanism (1) from "the
  arithmetic" to "the whole chip" — a register-level 8169 model the
  driver core is run against on the host, so a real data-path state
  machine is still proven in CI, just against a model instead of QEMU;
  RT3 ships mechanism (2) — the real-silicon paste-back — because the
  model proves the driver, and only metal proves the chip.

### 1.3 What is reused, not rebuilt

* `kernel/net/netdev.h` — `send`/`recv`/`recv_wait`/`get_mac`/`link_up`/
  `rx_pop_idle`; the 8139 fills this struct (`netdev_register` at
  `rtl8139.c:656`) and the whole IPv4/ARP/DHCP/UDP/TCP stack runs on it
  unchanged.  The 8169 fills the same struct.
* `pci_find_device(vendor, device)` — the probe the 8139 uses
  (`rtl8139.c:382`); the 8169 matches `10ec:8168`/`10ec:8169` the same
  way, one loop over the supported table.
* The IRQ-receipt discipline (R9/RES-28): a one-shot
  `[r8169] RX via IRQ wake` line proves the interrupt — not the polling
  fallback — did the work.
* The width-sweep budget (`tools/check_width_sweep.py`: casts **370**,
  x64-includes **69**, asm-files **27**).  The 8139 spent **zero**
  portable-include budget by declaring `irq_register_handler` locally
  rather than including `kernel/arch/x86_64/irq.h`; the 8169 must do the
  same, and any new portable `(uint64_t)` casts must be width-clean
  (register/user-pointer widths, not pointer truncations) with the
  ratchet moved same-commit.

---

## 2. Non-goals / deferrals (parked, each with a reason)

* **C+ mode descriptors / hardware checksum & VLAN offload.**  The
  stack computes its own checksums; the legacy (non-C+) descriptor path
  is the honest first rung.  C+ joins only if a ladder application
  measures a need for offload.
* **MSI / MSI-X.**  INTx first, exactly like every NIC driver in this
  tree; MSI is RES-36's named residue and stays there.
* **Wake-on-LAN, EEPROM, jumbo frames, flow control.**  Not on the
  ladder.
* **RTL8125 2.5GbE.**  A different generation; its own plan if it ever
  becomes reachable hardware.
* **8136/8101/8102/8103** (the 100 Mbit "8100E" family that uses the
  8169-style descriptor path).  Linux folds them into `r8169`; the
  catalog here lists only 8168/8169, so they join only when a machine
  actually presents one — measured, not assumed.

---

## 3. The shape of the work (the inverted gate)

Three implementation phases after the plan lands, each with a machine
checkable gate:

1. **Pin the surface.**  The register map and descriptor formats become
   a pure-C header with a host unit test — the 8139 lesson (the CAPR −16
   bias, the two-ring-sizes trap) lives in exactly such a header.
2. **Prove the driver against a model.**  The driver core is written
   against a narrow register-I/O seam; a register-level model of the
   8169 is built and self-tested; the driver's full probe/TX/RX/IRQ
   state machine runs against the model on the HOST.  This is the
   "virtual 8169" QEMU lacks — in CI, W-class, no silicon needed.
3. **Wire it in and hand the truth to metal.**  The driver joins
   `net_init`'s chain, the three catalog/docs rows flip, and the
   real-silicon confirmation ships as a metal-receipt slot
   (PENDING-USER).

No new QEMU shard: the whole plan adds zero CI wall-clock beyond
`make test-unit` — the one plan in this tree whose gate got *cheaper*
because the hardware QEMU cannot fake.

---

## 4. Phases

### Phase RT0 — Plan landing ✅ DONE

#### Tasks

* This document, measured against `c2aff92`.
* The marker baseline gains the `REALTEK_PLAN.md` row in the same
  commit (the residue ratchet must not drift; OTA O0's precedent).
* No code, no ledger rows: coverage registers at RT3.

#### Test gate

* `make test-unit` green with the new plan file present (ratchet row
  moved same-commit).
* Existing shards untouched.

#### Deliverable

`docs/plans/REALTEK_PLAN.md` + the `tools/residue_baseline.txt` row.

### Phase RT1 — The 8169 surface, host-pinned ✅ DONE (2026-09-11)

*Measured first, mapped second.*  Every register offset and descriptor
field comes from the RTL8169 datasheet (Rev. 1.21 and the
RTL8169SC/8110SC register spec), cross-checked against Linux's `r8169`
driver, and is host-pinned before a line of driver code exists — the
8139 precedent, where the ring arithmetic (not the driver) carried the
two bug classes.

#### What shipped

* `drivers/r8169/r8169_desc.h` (new): the register map and the legacy
  TX/RX descriptor surface as pure C with no hardware in it — MAC0–5 /
  MAR0–7, the 64-bit `TNPDS`/`RDSAR` ring-base pairs, CR/TPPOLL/IMR/
  ISR/TCR/RCR/Cfg9346/RMS/C+CR/ETThR **with their per-register access
  widths** (`r8169_reg_width()`: ISR/IMR are 16-bit, CR/TPPOLL are
  8-bit — reading ISR as 32-bit spans a neighbour), the ChipCmd/
  TxPoll/interrupt/RCR-accept/Cfg9346 content bits, the 16-byte
  `r8169_tx_desc`/`r8169_rx_desc` layouts, and the decision core:
  `r8169_rx_classify()` (OWN-stop / error-drop / fragment-drop /
  FCS-stripped length / desync-reset), `r8169_rx_advance()`,
  `r8169_tx_opts1()`, EOR-preserving `r8169_rx_rearm_opts1()` /
  `r8169_tx_free_opts1()`, and the 256-byte-alignment +
  low/high base-address split helpers.
* `tests/unit/test_r8169_desc.c` (new): the host gate — **74 checks** —
  driving the cases QEMU cannot be asked to produce (FCS stripping, the
  hardware error bits, the OWN hand-off, fragmented frames, impossible
  lengths, count-modulo wrap, EOR preservation on rearm, and the pinned
  surface relationships: the 14-bit length field *is* the armed buffer
  16383, 16-byte descriptors, the 1024-descriptor cap, 256-byte
  alignment).  The negative control is proven, not asserted: planting
  the dropped-FCS bug fires 3 failures, planting the dropped-EOR-on
  rearm bug fires 2.
* Registered in `make test-unit` beside `test_rtl8139_ring`.
* Zero width-sweep spend: no `(uint64_t)` casts, no `kernel/arch/x86_64`
  includes, no inline asm in the header — the 8139 rule.

#### Result (measured on the host gate)

`test_r8169_desc` PASSES 74/74 in `make test-unit`; `check_width_sweep.py`
OK (casts 370/370, x64-includes 69/69, asm-files 27/27); the marker
ratchet green (`REALTEK_PLAN.md` stays at 5).

#### Deviations from the task list (honest, measured)

* The §RT2 "4 GiB DMA refusal" question resolves at the surface: the
  datasheet shows the 8169's descriptor-base and buffer addresses are
  **64-bit** (`TNPDS`/`RDSAR` are low+high pairs; the descriptor `addr`
  field is a full u64), so there is no 32-bit truncation trap to refuse
  the way the 8139's `RBSTART`/`TSAD` forced.  The honest rule the
  header pins instead: the **high register must be programmed**
  (`r8169_desc_base_lo/hi`), and a driver that writes only the low
  register is the 8139 bug in disguise.
* The plan cited Linux's `r8169.h` as the cross-check; on the current
  tree that file is only a type/enum header — the register map lives in
  `r8169_main.c` (v6.12) / `r8169.c` (v3.10), which are the sources the
  offsets and bits were actually pinned from.  Citation corrected by
  measurement.

#### Tasks

* `drivers/r8169/r8169_desc.h` (new): the register map and the legacy
  TX/RX descriptor formats as a pure-C header with no hardware in it —
  the MAC registers (IDR0–5, MAR0–7), command/status/interrupt-mask
  registers, TX/RX config, the descriptor status/OWN/length fields, and
  the ring/ownership arithmetic (wrap, OWN-bit hand-off, FCS strip).
  The 8139's two bug classes are named here as the things to get right:
  the **modulus vs allocation** trap (what the ring wraps at vs what is
  allocated) and the **ownership hand-off** (who owns a descriptor when).
* `tests/unit/test_r8169_desc.c` (new): the host gate, driving the
  cases QEMU cannot be asked to produce — a frame that wraps the ring
  end, a CRC-errored frame, a length field that lies, an OWN-bit
  protocol violation — with a negative control that must fail when the
  wrong modulus or a wrong ownership rule is put back.
* Registered in `make test-unit`.

#### Test gate

`test_r8169_desc` green in CI (test-unit); negative control included;
ratchet green.

#### Deliverable

the descriptor/register model + host test + patch.

### Phase RT2 — The driver core, proved against a host chip model ✅ DONE (2026-09-11)

The decisive phase.  The 8169 has no QEMU model, so the *chip* becomes
software under test: the driver core is written against a narrow
register-I/O seam (read/write callbacks, not raw MMIO), and a host test
links it against a register-level model of the 8169 and drives the full
state machine end to end — on the host, in CI, no silicon, no QEMU.

*Measured first, modelled second.*  The model is reconstructed from the
registers the driver programs (the ring pointers come from TNPDS/RDSAR,
not a side channel), every register access is width-checked against
`r8169_reg_width()` (a wrong-width access is a *fault*, not a silent
truncation), and the model's own self-test is load-bearing: it asserts
the fault counter moves on illegal accesses, so a model wrong the same
way as the driver is caught, not passed.

#### What shipped

* `drivers/r8169/r8169_core.h` (new): the driver state machine as pure
  C with no hardware in it — reset, MAC load, ring configuration
  (256-byte-alignment refusal by name, BOTH halves of the 64-bit
  TNPDS/RDSAR pairs programmed, Linux-pinned TCR/RCR/RMS/ETThr values),
  link poll (PHY status is inverse-free), TX (runt pad to 60 bytes,
  OWN hand-off, TxPoll kick, completion poll, underrun/timeout errors),
  RX drain (FCS strip, error/fragment drop, desync reset, EOR-preserving
  rearm), and ISR ack.  All device access goes through the typed seam
  `struct r8169_io` — `rd8/rd16/rd32/wr8/wr16/wr32/relax` callbacks, so
  the width discipline is structural (a 16-bit register cannot be
  touched with a 32-bit accessor).
* `drivers/r8169/r8169.{c,h}` (new): the kernel driver.  `r8169.h` is
  the netdev-facing surface (probe `10ec:8168`/`10ec:8169`, `init`/
  `get_mac`/`link_up`/`send`/`recv`/`recv_wait`/`register_netdev`).
  `r8169.c` is the glue that binds the seam to BAR0 port I/O, probes
  PCI, allocates the 64-slot TX ring + 256-slot RX ring + 16383-byte
  RX buffers, and runs the INTx handler that drains RX into a 64-slot
  software queue and wakes sleepers (`[r8169] RX via IRQ wake` once —
  the R9/RES-28 receipt).  It includes the exact core the host gate
  proves, so the tested object and the shipped driver are one.
* `tests/unit/r8169_model.h` (new): the register-level model of the
  8169 — register file with per-register width faults, ISR W1C, CR
  reset/writable-mask semantics, Cfg9346 lock, TX DMA (OWN consumption,
  byte capture, TxOK latch, planted underrun → TXERR) and RX delivery
  (armed-buffer write, full-wire-length descriptor, RxOK latch, planted
  OWN-protocol violation → fault) — plus its own self-test.
* `tests/unit/test_r8169_driver.c` (new): the host gate — **1635
  checks** — model self-test, then the full data path driven against
  the model: reset → MAC load → config (HIGH registers programmed, EOR
  exactly on the last descriptor, rings reconstructed from the
  registers alone) → TX (byte-exact frame, runt pad, underrun, timeout)
  → RX (FCS strip, error drop, desync reset, 256-slot full-ring wrap
  with EOR survival) → IRQ/status.  Negative controls are proven, not
  asserted: planting the dropped-FCS bug fires **260** failures,
  planting the dropped-EOR-on-rearm bug fires **1**, planting the
  dropped-runt-pad bug fires **2**, and planting the model's OWN-bit
  hand-off removal fires **1** (the model's self-test catches its own
  bug — the "wrong the same way" trap, closed).
* Registered in `make test-unit` beside `test_r8169_desc`.

#### Result (measured on the host gate)

`test_r8169_driver` PASSES 1635/1635; `test_r8169_desc` still 74/74,
`test_rtl8139_ring` 203/203; full `make test-unit` green (rc=0);
`check_width_sweep.py` OK (casts 370/370, x64-includes 69/69, asm-files
27/27 — **zero** portable-include spend, the 8139 rule); the marker
ratchet green (`REALTEK_PLAN.md` stays at 5); `drivers/r8169/r8169.c`
compiles clean in the kernel build.

#### Deviations from the task list (honest, measured)

* The task list said "MMIO window map".  RT1's measured surface shows
  the 8169 answers the same 256-byte register file at BAR0 (I/O space)
  and BAR1 (memory space); the driver uses **BAR0 port I/O**, exactly
  like the 8139, so there is no MMIO window to map.  Recorded, not
  silently changed.
* The task list's "4 GiB DMA refusal by name" resolves to *not
  applicable*: the datasheet shows the 8169's ring-base and descriptor
  addresses are **64-bit** (RT1's recorded deviation).  The honest
  analog the core enforces instead is the **256-byte-alignment refusal
  by name** plus programming **both halves** of TNPDS/RDSAR.
* "links `r8169.c` against the model" resolves by construction:
  `r8169.c` is kernel glue (PCI probe, DMA allocation, INTx, netdev)
  that cannot run on the host; the host gate drives `r8169_core.h`, the
  pure-C state machine that `r8169.c` includes verbatim.  They are one
  object — the D2 pattern — so nothing the gate proves can drift from
  what ships.

#### Tasks

* `drivers/r8169/r8169.{c,h}` (new): probe (`pci_find_device`,
  `10ec:8168`/`10ec:8169`), MMIO window map, MAC load, TX/RX descriptor
  rings, INTx handler that drains into a software RX queue and wakes
  sleepers (`[r8169] RX via IRQ wake` once — the R9/RES-28 receipt),
  runt padding to 60 bytes, and the 4 GiB DMA refusal **by name**
  (the 8139 lesson, restated for the 8169's address width — measured
  from the datasheet, not recalled).  All device access goes through
  the seam.
* `tests/unit/r8169_model.h` (new): the register-level model of the 8169
  — MMIO register-file semantics, descriptor rings with the OWN-bit
  hand-off, interrupt status — plus its **own self-test against the
  datasheet's documented invariants**.  A model that is wrong the same
  way the driver is wrong passes both, so the model's own test is
  load-bearing, not decoration.
* `tests/unit/test_r8169_driver.c` (new): links `r8169.c` against the
  model and drives the whole data path — probe → MAC load → **TX** (the
  driver writes a descriptor, the model consumes it and asserts the
  frame bytes and the OWN-bit hand-off) → **RX** (the model delivers a
  frame, the driver must hand a correctly-stripped packet to the netdev
  interface) → IRQ/status handling.  This is the data-path proof.
* Registered in `make test-unit`.

#### Test gate

the three host tests green in CI; width-sweep ratchets at budget (zero
portable-include spend, the 8139 rule); ratchet green.

#### Deliverable

driver core + chip model + host gate + patch.

### Phase RT3 — Wiring, catalog flip, metal package, close-out ✅ DONE (2026-09-11)

The close-out: the driver joins the boot-time NIC chain, the catalog
and the docs stop calling the 8169 driverless, the metal confirmation
ships as a paste-back slot instead of silicon, and a claim checker pins
the whole plan to the tree.

#### What shipped

* `r8169_init()` / `r8169_register_netdev()` join `net_init()`'s
  fallback chain **after e1000e, before rtl8139** — gigabit before
  100 Mbit, the priority rule the comment already states.  On a machine
  whose only NIC is an 8168/8169, the driver is now the active netdev.
* The catalog/docs flip same-commit: `virtual_drivers.c`'s two rows
  move from "known / no data path" to "host-model data path"; the
  `docs/status.md` 🚧 row splits (its vmxnet3/e1000e half was already ✅;
  the RTL8169 half flips to ✅ with the PENDING-USER caveat, so the
  status-wip count moves 7 → 6); `docs/driver_guide.md`'s "Not
  supported" paragraph retracts into an RTL8169 networking section;
  `docs/virtual_driver_matrix.md` moves the row into the active table.
* **The metal confirmation ships, not the silicon:** `docs/metal_receipts.md`
  gains **slot 10** — boot on a machine with an onboard 8168/8169 (or
  PCI-passthrough one into the VM), paste the `[r8169] found …`,
  `MAC …`, `ready: … link=up` and `RX via IRQ wake` lines.  M-class,
  `PENDING-USER@RT3` — a *status*, not a failure (the RES-30 precedent:
  the host gate proves the driver; only the user's machine can prove the
  chip).
* `tools/check_realtek_claims.py` in the checker family: every ✅ phase
  pinned to its artefacts (the descriptor header, the model, the driver,
  the host tests, the catalog rows) *and* its greppable receipts (the
  `r8169` catalog key, the `[r8169]` receipt strings, the net_init chain
  entry), with the usual planted-violation negative control.  Wired into
  `make test-unit` and the workflow's claim-check step.
* docs rows same-commit (`docs/status.md`, `docs/driver_guide.md`,
  `docs/virtual_driver_matrix.md`), the two ledger coverage rows
  (RES-55 W → DONE@RT3, RES-56 M → PENDING-USER@RT3), the checker's
  row/class pins and the debt baseline moved, this plan → COMPLETE.

#### Result (measured on the tree)

`make test-unit` green (rc=0): `test_r8169_desc` 74/74,
`test_r8169_driver` 1635/1635, `check_realtek_claims.py` OK (4 phases,
21 artefact + 30 receipt pins) with SELFTEST OK; the debt-ledger checker
OK (56 rows, classes W 35 · M 7 · N 4 · S 10); `check_width_sweep.py` OK
(casts 370/370, x64-includes 69/69, asm-files 27/27 — zero
portable-include spend).  `kernel/net/net.c` and
`drivers/vm/virtual_drivers.c` compile clean as kernel objects with the
driver wired in; on machines without the chip the r8169 sections are
garbage-collected and the chain falls through to the 8139 as before.

#### Deviations from the task list

* None — the phase landed as written.  The only recorded resolution is
  that "ledger coverage rows" (plural) names exactly the two rows the
  Ledger-integration note at the top of this plan already split out:
  one W-class coverage row (RES-55) and one M-class row (RES-56).

#### Tasks

* `r8169_init()` / `r8169_register_netdev()` join `net_init()`'s
  fallback chain **after e1000e, before rtl8139** — gigabit before
  100 Mbit, the priority rule the comment already states.  On a machine
  whose only NIC is an 8168/8169, the driver is now the active netdev.
* The catalog/docs flip same-commit: `virtual_drivers.c`'s two rows
  move from "known / no data path" to the host-model status; the
  `docs/status.md` 🚧 row splits (its vmxnet3/e1000e half is already ✅
  under RESIDUE2 T6; the RTL8169 half flips to the honest new status);
  `docs/driver_guide.md`'s "Not supported" paragraph retracts;
  `docs/virtual_driver_matrix.md` moves the row.
* **The metal confirmation ships, not the silicon:** `docs/metal_receipts.md`
  gains **slot 10** — boot on a machine with an onboard 8168/8169 (or
  PCI-passthrough one into the VM), paste the `[r8169] found …`,
  `MAC …`, `ready: … link=up` and `RX via IRQ wake` lines.  M-class,
  `PENDING-USER@RT3` — a *status*, not a failure (the RES-30 precedent:
  the host gate proves the driver; only the user's machine can prove the
  chip).
* `tools/check_realtek_claims.py` in the checker family: every ✅ phase
  pinned to its artefacts (the descriptor header, the model, the driver,
  the host tests, the catalog rows) *and* its greppable receipts (the
  `r8169` catalog key, the `[r8169]` receipt strings, the net_init chain
  entry), with the usual planted-violation negative control.  Wired into
  `make test-unit` and the workflow's claim-check step.
* docs rows same-commit (`docs/status.md`, `docs/driver_guide.md`,
  `docs/virtual_driver_matrix.md`), ledger coverage rows, baseline
  moved, this plan → COMPLETE.

#### Test gate

CI: host gates + checker green in `test-unit`; the metal slot ships as
`PENDING-USER`; ratchet green.

#### Deliverable

wiring + docs + metal package + checker + patch; plan COMPLETE.

---

## 5. Risks, named

* **The no-QEMU gate is the whole reason this plan is shaped the way it
  is.**  Mitigated by construction: the host chip model keeps the data
  path provable in CI (W-class), and the metal slot is the only honest
  place the real silicon can be confirmed (M-class, PENDING-USER).  The
  alternative — shipping a driver with *no* gate — is the one outcome
  this tree's discipline rules out, and the one this plan exists to
  avoid.
* **Model fidelity (the "wrong the same way" trap).**  A model that
  shares the driver's mistake passes both.  The counter is the model's
  own self-test against datasheet invariants, plus the stated boundary:
  the model proves the *driver*; the metal paste-back is the only proof
  of the *chip*.
* **The descriptor-mode zoo.**  Legacy vs C+ vs VLAN variants diverge
  per chip revision.  RT1 pins legacy mode first; C+ stays named out
  (section 2) rather than half-implemented.
* **DMA address width.**  The 8139's 32-bit `RBSTART`/`TSAD` refusal
  recurs in the 8169's descriptor address fields — measured and refused
  by name in RT2, not silently truncated.
* **The width-sweep budget.**  The 8139 stayed at zero portable-include
  spend; the 8169 must too, or the ratchet moves same-commit with the
  reasoning written at the pin.

---

## 6. Close-out checklist

- [x] RT0: `REALTEK_PLAN.md` lands; baseline row moved same-commit; ratchet green (2026-09-11)
- [x] RT1: `test_r8169_desc.c` green in test-unit (74/74), negative control included (2026-09-11)
- [x] RT2: `test_r8169_driver.c` + `r8169_model.h` self-test green; width-sweep ratchets at budget (2026-09-11)
- [x] RT3: net_init chain wired; catalog/docs flip; `check_realtek_claims.py` green in test-unit; ledger rows + baseline moved same-commit; plan → COMPLETE (2026-09-11)
- [x] RT3: metal-receipt slot 10 ships as `PENDING-USER` (a status, not a failure) (2026-09-11)
