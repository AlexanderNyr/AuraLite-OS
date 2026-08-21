# AuraLite OS — Real-Hardware Package + String-Ops Parity Plan

## Status: IN PROGRESS 🚧 — H0 complete (phases H0–H5)

| Phase | Result | Deliverable |
|-------|--------|-------------|
| H0 — the rig: rv64/a64 membench, x86 feature receipts | ✅ complete | `patches/HW_H0_rig.patch` |
| H1 — word-wide portable string ops (the rv64/a64 win) | pending | `patches/HW_H1_stringops.patch` |
| H2 — ERMSB: the receipt and the crossover | pending | `patches/HW_H2_ermsb.patch` |
| H3 — PAT + write-combining framebuffer | pending | `patches/HW_H3_pat.patch` |
| H4 — PCID: the measured absence and the deferral protocol | pending | `patches/HW_H4_pcid.patch` |
| H5 — close-out: docs, CI, the hardware receipt protocol | pending | `patches/HW_H5_close.patch` |

## 1. Where this plan comes from

`OPT_PLAN.md` closed COMPLETE with its residue named, not hidden
(§5/§7 there): the **real-hardware-only wins** — PAT/WC framebuffer
mapping (named in O4), PCID and the generation scheme it forces on
the shootdown filter (named in O5), ERMSB crossover tuning (named in
O1/O3) — and the **cross-arch string-ops line**: the portable
memset/memcpy/memmove in `kernel/lib/string.c` are still the
byte-at-a-time loops O1 replaced on x86_64, and since ARM64_PLAN A5a
the rv64 AND a64 kernels both link exactly those bodies.  Every frame
of a64 initrd load, every rv64 ELF spawn, pays a loop iteration per
byte today.

This plan picks up both threads.  It is deliberately small: six
phases, two of them measurable under TCG right now (H0, H1), one
with a real `-cpu max` lane (H2 — ERMS is exposed there, measured in
H0), one whose correctness half is TCG-checkable (H3), and one that
H0's own measurements re-scoped into a deferral protocol (H4 — no
QEMU configuration here can execute PCID at all) — which decision D2
below is about.

## 2. Decisions

### D1. Measured, not assumed (inherited)
No number, no claim.  TCG numbers are labelled TCG; hardware numbers
do not exist until someone boots hardware, and the plan says so
instead of extrapolating.

### D2. Every hardware item splits into a TCG-checkable half and a named receipt
PAT programming, WC PTEs, ERMSB detection, PCID switching are all
CORRECTNESS under QEMU: MSR readbacks, PTE attribute probes, CPUID
receipts, still-green suites.  Those land and gate here.  The
PERFORMANCE halves (WC flip throughput, ERMSB small-copy crossover,
PCID switch latency) are recorded as **hardware receipt slots** in §6
— one command each, paste-the-line-back.  Pretending TCG validates
them would be theatre (OPT D1's exact words).

### D3. The asm ratchet stands
Ratchet 4 holds portable files at zero inline assembly.  Word-wide
string ops stay portable C (`__builtin_memcpy` word loads — the
memcmp/strlen shape O1 already landed); anything that needs an
instruction (`rdmsr`, `invpcid`, `dc zva` if it ever argues its way
in) lives in the arch tree.

### D4. Runtime detection only
PCID, INVPCID, ERMS, PAT are CPUID-gated at boot — no build knobs.
The same kernel binary must boot qemu64 (none of them), `-cpu max`
(all of them), and metal, and print what it found each time: the
feature line IS the compatibility matrix.

## 3. Phases

### Phase H0 — The rig: rv64/a64 membench, x86 feature receipts ✅ COMPLETE

**Objective:** numbers before changes (the O0 discipline, third
edition): the byte-loop baseline on both DTB tenants, and the x86
feature/MSR ground truth under TCG.

#### Tasks

- [x] `kernel/arch/riscv64/membench_rv.c` + `kernel/arch/aarch64/
      membench_a64.c`: boot-time bench over the LINKED string ops
      (whatever `kernel/lib/string.c` provides — today the byte
      loops, after H1 the word loops; the bench measures the link,
      not a copy).  memcpy 64 KiB / 1 MiB-equivalent, memset, memmove
      (overlapping), MB/s from each tenant's own clock (`rdtime` @
      timebase, `CNTVCT` @ CNTFRQ).  Buffers are static 64 KiB pairs;
      "1 MiB" is 16 passes — TCG models no cache hierarchy, so
      streaming-vs-resident is not a distinction it can measure, and
      the file says so.
- [x] x86_64 boot receipts: `[cpu] features: pat=_ pcid=_ invpcid=_
      erms=_` (CPUID 1:EDX.16, 1:ECX.17, 7.0:EBX.10, 7.0:EBX.9) and
      `[cpu] IA32_PAT = 0x...` (rdmsr 0x277) — the H2/H3/H4 ground
      truth, printed every boot so the smoke can pin it.
- [x] Smokes: rv_boot + a64_boot each assert the bench table;
      test_perf_smoke asserts both receipt lines.
- [x] `tools/check_hw_claims.py` ships WITH the plan (the D8
      tradition, fifth checker in the family), wired into test-unit.

#### Result

**Two surprises, both measured — which is the rig's whole job.**

First: the DTB tenants' "byte loops" are NOT x86's byte loops.
Baselines (QEMU/TCG, this sandbox; §5): rv64 memcpy 249 MB/s at
64 KiB / 413 at 1 MiB-eq, memset 434, memmove 370; a64 memcpy
197/288, memset 568, memmove 254.  Fact-1-shaped expectations said
~10 MB/s (x86 measured 11 before O1) — but clang at -O2 already
unrolls/widens these loops on rv64gc and aarch64, where there is no
`-mno-sse`-era constraint story.  The OPT §7 residue line ("byte
loops, ready for adoption") was true about the SOURCE; the generated
code had already stopped being byte-at-a-time.  H1's bar is set by
this number, not by the x86 legend — see the reworded gate there.
The rv64 kernel also ADOPTED `kernel/lib/string.c` in this phase
(it linked no string ops at all before; the bench forced the
question the residue table had left open).

Second: **`-cpu max` under TCG does not expose PCID.**  Receipts:
qemu64 → `pat=1 pcid=0 invpcid=0 erms=0`; `-cpu max` → `pat=1
pcid=0 invpcid=0 erms=1`.  Both lanes: `IA32_PAT =
0x0007040600070406` (reset default — WB/WT/UC-/UC twice, no
write-combining entry; H3's starting point is now a printed fact).
So ERMSB has a TCG lane (`-cpu max`) and PCID has NONE — no QEMU
configuration in this sandbox can EXECUTE a PCID-enabled kernel.
H4 is re-scoped accordingly (see the phase): TLB-correctness code
without an executable lane is exactly what this project refuses to
land.

#### Test gate

- Both bench tables print and are smoke-asserted; both receipt lines
  print and are smoke-asserted; checker green with selftest; all
  standing suites green.

#### Deliverable

`patches/HW_H0_rig.patch`

---

### Phase H1 — Word-wide portable string ops (the rv64/a64 decision)

**Objective:** pay the §7 residue line the D1 way: write the 8-byte
bodies (the memcmp/strlen shape, D3 — no asm), MEASURE them against
H0's now-known-unrolled baseline, and keep them only if they win.

#### Tasks

- [ ] `kernel/lib/string.c`: word-wide bodies — aligned-word fast
      path, `__builtin_memcpy` 8-byte loads/stores for the unaligned
      middle (defined behaviour), byte head/tail.  `-mstrict-align`
      on a64 makes the aligned path mandatory, not stylistic.
- [ ] The x86_64 kernel keeps `string_fast.c` (shadowing unchanged) —
      byte-identity control on its objects.
- [ ] Host unit tests compile these exact bodies; overlap/alignment
      edges extended if the sweep finds a gap.
- [ ] Membench re-run on both tenants; §5 columns filled.

#### Test gate

- The honest fork, stated up front: if the explicit word loops beat
  H0's numbers on memcpy at BOTH tenants, they land; if they merely
  tie the auto-unrolled byte loops, the phase lands as the MEASURED
  CLOSURE of the residue line (numbers in the table, source
  unchanged or reverted) — a no-op with a receipt is a valid D1
  outcome, silent regression is not.  Either way: x86 objects
  byte-identical; every suite green.

#### Deliverable

`patches/HW_H1_stringops.patch`

---

### Phase H2 — ERMSB: the receipt and the crossover

**Objective:** the O1/O3 residue: on ERMSB hardware `rep movsb` has
no setup-cost cliff and the `SMALL_N = 64` crossover in
`string_fast.c` is tuned for the wrong machine.  Make the crossover
ERMS-aware; keep TCG behaviour bit-identical.

#### Tasks

- [ ] Boot-time ERMS detection feeds `string_fast.c` (one branch:
      crossover 64 → 0 when ERMS; the movsq bulk form stays — ERMSB
      covers it, and TCG needs it).
- [ ] `-cpu max` lane: receipt shows `erms=1` and the crossover line
      names the active threshold.
- [ ] Hardware receipt slot (§6): membench small-copy row on metal.

#### Test gate

- qemu64 lane: behaviour and numbers unchanged (measured); `-cpu max`
  lane: receipt + threshold line; suites green.

#### Deliverable

`patches/HW_H2_ermsb.patch`

---

### Phase H3 — PAT + write-combining framebuffer

**Objective:** the O4 residue: the framebuffer is mapped WB-or-UC
today; real hardware wants WC.  Program IA32_PAT entry 4 to WC,
plumb a PTE mapping kind that selects it, remap the framebuffer.

#### Tasks

- [ ] `IA32_PAT` PA4 := WC (low four entries keep reset defaults —
      existing mappings keep their meaning; the receipt line proves
      it).
- [ ] Paging: a WC mapping kind (PAT bit + PCD/PWT selection for
      entry 4), used by the framebuffer map path only.
- [ ] Probe line: the fb PTE decoded at boot (`[mm] fb: WC via PAT4`).
- [ ] Correctness under TCG: gui/graphics/compositor cases
      pixel-identical (TCG ignores memory types — which is exactly
      why this phase's perf claim is a §6 receipt slot, not a TCG
      number).

#### Test gate

- PAT readback shows the WC entry; fb PTE probe line asserted; gui
  shard green; qemu64/`-cpu max` both boot clean.

#### Deliverable

`patches/HW_H3_pat.patch`

---

### Phase H4 — PCID: the measured absence and the deferral protocol

**Objective:** the O5 residue, resolved the D1 way.  H0 measured
that NO QEMU configuration available here executes a PCID-enabled
kernel (`-cpu max` under TCG reports `pcid=0 invpcid=0`; KVM is
absent in this sandbox AND on the shared CI runners).  TLB-
correctness code that cannot be executed anywhere in the gate suite
is exactly what this project refuses to land — O5 said "pretending
TCG numbers validate them would be theatre", and un-executed
correctness paths are a worse theatre than un-validated numbers.

#### Tasks

- [ ] The receipt line (H0's) stays the standing probe: the first
      lane that prints `pcid=1` — a KVM runner, a metal boot — is
      the lane this phase's implementation half re-opens on.
- [ ] The design is WRITTEN (not coded): PCID allocation, generation
      wrap, NOFLUSH re-entry, the O5 filter's generation interplay,
      `invpcid`-vs-CR3-toggle — reviewed against the O5 shootdown
      code so the future implementer inherits decisions, not a
      blank page.
- [ ] `/proc/perf` reserves the counter names
      (`cr3_noflush_switches`, `pcid_generation_wraps`) at zero —
      the metal receipt has a slot the moment it exists.

#### Test gate

- The design section reviewed against kernel/arch/x86_64/
  tlb_shootdown.c; receipts unchanged on both TCG lanes; no
  behavioural diff anywhere (byte-identity control).

#### Deliverable

`patches/HW_H4_pcid.patch`

---

### Phase H5 — Close-out: docs, CI, the hardware receipt protocol

**Objective:** the plan closes through checker arithmetic; the
hardware half becomes a protocol someone with metal can run in ten
minutes.

#### Tasks

- [ ] `docs/status.md`: the feature rows (PAT/WC, PCID, ERMS) with
      their TCG-correctness/hardware-performance split stated.
- [ ] CI: a `-cpu max` lane where it pays (the H4 smoke); the
      qemu64 lanes stay primary.
- [ ] §6 receipt protocol finalised: exact commands, exact lines to
      paste back, and the table that holds them.
- [ ] `check_hw_claims.py` closed to full coverage; terminal Status
      arithmetic.

#### Deliverable

`patches/HW_H5_close.patch`

## 4. What this plan deliberately does not do

- No SSE/AVX memcpy: the kernel is `-mno-sse` for FPU-state reasons
  that predate this plan and are not up for renegotiation here.
- No `dc zva` a64 memset backend yet: it argues its way in only if
  H1's a64 memset number leaves something worth taking (named
  deferral, same bar as OPT's ThinLTO).
- No i386 string-ops work: `rep movsd` transfers exist there already
  per the OPT residue; the i386 tree stays frozen unless a gate asks.
- No userspace copies of any of this (the kernel/libc seam rule).

## 5. The numbers (filled per landed phase — D1)

All TCG on this sandbox unless a receipt row says otherwise.

| Metric | H0 baseline | H1 | H2 | H3 |
|---|---|---|---|---|
| rv64 memcpy 64 KiB (MB/s) | 249 | | | |
| rv64 memcpy 1 MiB-eq (MB/s) | 413 | | | |
| rv64 memset 1 MiB-eq (MB/s) | 434 | | | |
| rv64 memmove-overlap 64 KiB (MB/s) | 370 | | | |
| a64 memcpy 64 KiB (MB/s) | 197 | | | |
| a64 memcpy 1 MiB-eq (MB/s) | 288 | | | |
| a64 memset 1 MiB-eq (MB/s) | 568 | | | |
| a64 memmove-overlap 64 KiB (MB/s) | 254 | | | |
| x86 receipts (qemu64) | pat=1 pcid=0 invpcid=0 erms=0; PAT=0x0007040600070406 | | | |
| x86 receipts (-cpu max) | pat=1 **pcid=0 invpcid=0** erms=1; PAT same | | | |

## 6. Hardware receipt slots (D2 — filled only from metal)

| Receipt | Command | Line to paste | Value |
|---|---|---|---|
| ERMSB small-copy crossover | `run membench` on metal | the <64 B rows | — |
| WC framebuffer flip rate | gui + `/proc/perf` compositor counters on metal | px/s before/after H3 | — |
| PCID switch cost | boot metal, `/proc/perf` | `cr3_noflush_switches` + wall-clock delta | — |
