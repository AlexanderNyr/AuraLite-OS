# AuraLite OS — Real-Hardware Package + String-Ops Parity Plan

## Status: COMPLETE ✅ — H0–H5 all landed (numbers in §5, receipts protocol in §6); closed 2026-08-21

| Phase | Result | Deliverable |
|-------|--------|-------------|
| H0 — the rig: rv64/a64 membench, x86 feature receipts | ✅ complete | `patches/HW_H0_rig.patch` |
| H1 — word-wide portable string ops (the rv64/a64 decision) | ✅ complete | `patches/HW_H1_stringops.patch` |
| H2 — ERMSB: the receipt and the crossover | ✅ complete | `patches/HW_H2_ermsb.patch` |
| H3 — PAT + write-combining framebuffer | ✅ complete | `patches/HW_H3_pat.patch` |
| H4 — PCID: the measured absence and the deferral protocol | ✅ complete | `patches/HW_H4_pcid.patch` |
| H5 — close-out: docs, CI, the hardware receipt protocol | ✅ complete | `patches/HW_H5_close.patch` |

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

> **Correction, H1 (same day, the O6 tradition — the number was
> real, the attribution was not):** the H1 objdump showed clang had
> NOT unrolled anything — both kernels' memcpy was a plain
> byte-per-iteration loop (`lbu/sb`, `ldrb/strb`).  The 249/197 MB/s
> baselines are simply what ~1.2 G guest-insns/s of modern TCG does
> to a 4–5-instruction byte loop on this host.  The x86 "11 MB/s"
> legend was a fact about a slower TCG epoch, not about codegen.
> Nothing in H0's numbers changes; the mechanism paragraph above was
> wrong and this note is its receipt.

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

### Phase H1 — Word-wide portable string ops (the rv64/a64 decision) ✅ COMPLETE

**Objective:** pay the §7 residue line the D1 way: write the 8-byte
bodies (the memcmp/strlen shape, D3 — no asm), MEASURE them against
H0's now-known-unrolled baseline, and keep them only if they win.

#### Tasks

- [x] `kernel/lib/string.c`: word-wide bodies — aligned-word fast
      path, `__builtin_memcpy` 8-byte loads/stores for the unaligned
      middle (defined behaviour), byte head/tail.  `-mstrict-align`
      on a64 makes the aligned path mandatory, not stylistic.
- [x] The x86_64 kernel keeps `string_fast.c` (shadowing unchanged) —
      byte-identity control on its objects.
- [x] Host unit tests compile these exact bodies; overlap/alignment
      edges extended if the sweep finds a gap.
- [x] Membench re-run on both tenants; §5 columns filled.

#### Result

**The fork resolved in favour of the code — decisively, and for a
corrected reason.**  The phase opened with an objdump (before
touching anything), which overturned H0's mechanism story: clang had
NOT unrolled the byte loops — both kernels' memcpy was a plain
byte-per-iteration loop, and H0's healthy-looking baselines were
just modern TCG executing ~1.2 G guest-insns/s.  The correction is
recorded in H0's Result (the O6 tradition).  That made the word
loops' case straightforward: 8 bytes per iteration against 1.

The bodies: `sw_word` (`uint64_t` + `__attribute__((may_alias))`) is
the load-bearing type — the cast asserts the 8-byte alignment that
`-mstrict-align` a64 REQUIRES before clang will emit a wide access
(a `__builtin_memcpy`-only spelling would silently degrade to byte
loads there, because the builtin cannot prove alignment), and the
attribute waives the aliasing rule that a bare `uint64_t *` into a
byte buffer would break.  memcpy forks on co-alignment: byte head
earns both sides 8-byte alignment, then one aligned load + store per
8; the mixed-alignment middle keeps aligned STORES and takes
`__builtin_memcpy` loads (one `ld` on rv64; byte loads on
strict-align a64, honestly — still one wide store per 8, and mixed
alignment is the rare case).  memmove's backward path plays the same
trick from the top end.  Verified codegen, not assumed: objdump
shows `ld/sd` (rv64) and `ldr/str x` (a64) in the loops.

**Numbers (TCG, same sandbox, §5):** rv64 memcpy 64 KiB 249 → **814**
MB/s, 1 MiB-eq 413 → **2449** (5.9×), memset 434 → **2553** (5.9×),
memmove-overlap 370 → **887**; a64 memcpy 197 → **623**, 288 →
**1964** (6.8×), memset 568 → **3178** (5.6×), memmove 254 → **617**.
(The 64 KiB single-pass rows carry first-pass TCG translation cost;
the 1 MiB-eq rows are the steady state.)

**Controls:** x86 `string.o` `.text` byte-identical before/after
(the `#ifndef ARCH_X86_64` fence plus `string_fast.c` shadowing —
measured, not trusted).  The host suite compiles these exact bodies:
`test_string` grew a word-path torture sweep — every src/dst offset
pair in a word crossed with sizes straddling the n≥16 threshold and
word boundaries, plus overlap sweeps in both directions with canary
bytes around the window — 43/43 (was 40).  Gates: rv boot 47 OK /
shell 22 OK, a64 boot 45 OK / shell 15 OK, x86 17/17.

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

### Phase H2 — ERMSB: the receipt and the crossover ✅ COMPLETE

**Objective:** the O1/O3 residue: on ERMSB hardware `rep movsb` has
no setup-cost cliff and the `SMALL_N = 64` crossover in
`string_fast.c` is tuned for the wrong machine.  Make the crossover
ERMS-aware; keep TCG behaviour bit-identical.

#### Tasks

- [x] Boot-time ERMS detection feeds `string_fast.c` (one branch:
      crossover 64 → 0 when ERMS; the movsq bulk form stays — ERMSB
      covers it, and TCG needs it).
- [x] `-cpu max` lane: receipt shows `erms=1` and the crossover line
      names the active threshold.
- [x] Hardware receipt slot (§6): membench small-copy row on metal.

#### Result

The crossover is RUNTIME now: `string_fast_init()` (called from the
H0 receipt printer — the receipt's first consumer) reads
CPUID.7.0:EBX.9 once and drops `small_n` 64 → 0 on ERMS parts, per
the SDM's fast-string contract; the threshold line prints on every
boot so each lane's smoke pins which world it booted in.  The movsq
bulk form stays — ERMSB covers it and TCG *needs* it (O1's measured
lesson).

**Lanes, both green:** qemu64 — `crossover: 64 (no ERMS)` asserted
in perf_smoke (30 assertions), membench table produced; the 1 MiB
rows moved 534 → 1136 MB/s BETWEEN RUNS OF THE SAME BINARY earlier
in the session too — this sandbox's TCG noise band is that wide, and
the honest statement is "within noise", not "unchanged to the
megabyte".  `-cpu max` — the new `x86_cpumax_smoke.sh` (5
assertions): `erms=1` receipt, `crossover: 0 (ERMS fast-string)`,
kernel reaches the shell handoff with the 0-byte threshold live, no
panic.

**One pre-existing fact found and fenced, not fixed:** under
`-cpu max` the USERSPACE shell's banner never appears on the serial
log, though the shell process starts (`shell active; kmain idling`
prints; the prompt does not).  The control run — same boot, pre-H2
kernel — behaves identically, so the smoke asserts the kernel-side
handoff and the oddity is recorded here as `-cpu max` residue for
whoever needs the interactive lane someday.

The PERFORMANCE half stays a §6 metal receipt: TCG emulates
rep-string one iteration at a time regardless of ERMS, so the
threshold's wall-clock effect is invisible here by construction —
this phase proves the detection and the wiring, which is what TCG
can prove (D2).

#### Test gate

- qemu64 lane: behaviour and numbers unchanged (measured); `-cpu max`
  lane: receipt + threshold line; suites green.

#### Deliverable

`patches/HW_H2_ermsb.patch`

---

### Phase H3 — PAT + write-combining framebuffer ✅ COMPLETE

**Objective:** the O4 residue: the framebuffer is mapped WB-or-UC
today; real hardware wants WC.  Program IA32_PAT entry 4 to WC,
plumb a PTE mapping kind that selects it, remap the framebuffer.

#### Tasks

- [x] `IA32_PAT` PA4 := WC (low four entries keep reset defaults —
      existing mappings keep their meaning; the receipt line proves
      it).
- [x] Paging: a WC mapping kind (PAT bit + PCD/PWT selection for
      entry 4), used by the framebuffer map path only.
- [x] Probe line: the fb PTE decoded at boot (`[mm] fb: WC via PAT4`).
- [x] Correctness under TCG: gui/graphics/compositor cases
      pixel-identical (TCG ignores memory types — which is exactly
      why this phase's perf claim is a §6 receipt slot, not a TCG
      number).

#### Result

**PAT programming lives where per-CPU state already lives.**
`paging_cpu_features_init()` — the one function that runs on the BSP
AND in every AP's `ap_entry()` (it exists because EFER/CR4 reset per
CPU; PAT resets the same way) — now writes PA4 := WC, keeping the
low four entries at reset defaults so every existing mapping keeps
its meaning.  An AP left at reset PAT while the BSP writes WC PTEs
would be attribute aliasing on metal that TCG would never show; the
placement closes that hole by construction.  The line printed is the
READBACK, not the intent (D1): `IA32_PAT: PA4=WC (readback
0x0007040100070406)` — asserted verbatim on the BIOS lane
(perf_smoke) and the UEFI lane (gui_dirty_uefi).

**The remap is exact and self-describing.**  `paging_fb_set_wc()`
walks the framebuffer's HHDM range 4 KiB at a time; `walk_pte`'s
existing `split_huge_page` machinery (built for MMIO BARs — this is
the same shape of job) carves the boot-time huge pages so ONLY the
fb's pitch×height bytes change type; each PTE gets PAT=1 PCD=0 PWT=0
(entry 4; on a 4-KiB PTE the PAT bit sits where PS sits one level up,
and the code says so out loud), each page gets `invlpg`.  The probe
line decodes what the FIRST PTE actually says after the loop, not
what the function meant: `fb: WC via PAT4 (1000 pages; PTE PAT=1
PCD=0 PWT=0)` — and 1000 pages is 1280×800×4 bytes exactly, the
UEFI GOP mode\'s arithmetic showing its work.

**Both worlds pinned, including the empty one:** the BIOS lane has
no linear framebuffer, so its assertion is the honest skip line
(`fb: none present; WC remap skipped`) — a lane that asserts nothing
would let the remap silently stop running.  PAT-less CPUs refuse
separately (D4: runtime, no knob).

**Gates:** the full gui shard 16/16 (1321 s — dirty-rect, compositor,
opengl, virgl, gbrowser, w32, doom all pixel-green over WC PTEs),
perf_smoke 32 assertions, cpumax 5/5, x86 17/17, ratchets
359/69/0/29.  THROUGHPUT stays a §6 metal receipt: TCG ignores
memory types by construction — what this phase proves is the split,
the flags, the flush, and a boot that still draws (D2).

#### Test gate

- PAT readback shows the WC entry; fb PTE probe line asserted; gui
  shard green; qemu64/`-cpu max` both boot clean.

#### Deliverable

`patches/HW_H3_pat.patch`

### Phase H4 — PCID: the measured absence and the deferral protocol ✅ COMPLETE

**Objective:** the O5 residue, resolved the D1 way.  H0 measured
that NO QEMU configuration available here executes a PCID-enabled
kernel (`-cpu max` under TCG reports `pcid=0 invpcid=0`; KVM is
absent in this sandbox AND on the shared CI runners).  TLB-
correctness code that cannot be executed anywhere in the gate suite
is exactly what this project refuses to land — O5 said "pretending
TCG numbers validate them would be theatre", and un-executed
correctness paths are a worse theatre than un-validated numbers.

#### Tasks

- [x] The receipt line (H0's) stays the standing probe: the first
      lane that prints `pcid=1` — a KVM runner, a metal boot — is
      the lane this phase's implementation half re-opens on.
- [x] The design is WRITTEN (not coded): PCID allocation, generation
      wrap, NOFLUSH re-entry, the O5 filter's generation interplay,
      `invpcid`-vs-CR3-toggle — reviewed against the O5 shootdown
      code so the future implementer inherits decisions, not a
      blank page.
- [x] `/proc/perf` reserves the counter names
      (`cr3_noflush_switches`, `pcid_generation_wraps`) at zero —
      the metal receipt has a slot the moment it exists.

#### Result — the written design

Reviewed against `kernel/arch/x86_64/tlb_shootdown.c` (whose own
header already names this exact residue at lines 27–37: the O5
sender-side skip is an ARCHITECTURAL fact only while "CR3 load =
full flush" holds, and PCID is the feature that breaks it).

**D-PCID-1: allocation.**  One PCID per address space, allocated at
CR3-install time from a per-CPU 12-bit bump counter (1..4095; PCID 0
stays the kernel\'s).  Per-CPU, not global: PCIDs are a per-TLB
namespace, a global allocator would serialise spawns for no
architectural gain, and the wrap arithmetic stays local.

**D-PCID-2: generation wrap.**  When the bump counter wraps, bump a
per-CPU GENERATION, do one full non-PCID flush (CR4.PCIDE toggle or
`invpcid` type 2 all-context), and lazily re-allocate PCIDs on next
switch (an address space remembers `(cpu, pcid, generation)`; a
stale generation means "allocate fresh, no flush needed — the wrap
already flushed").  Count it in `pcid_generation_wraps`.

**D-PCID-3: the switch.**  Re-entering a live `(pcid, generation)`
loads CR3 with bit 63 (NOFLUSH) and counts `cr3_noflush_switches` —
the counter IS the win, measured, exactly like O5\'s
`tlb_ipis_skipped`.

**D-PCID-4: the O5 filter learns generations.**  The sender-side
skip\'s justification inverts: with PCID a target CPU whose current
CR3 differs from the victim MAY STILL hold stale entries for it.
The filter therefore skips only when the target\'s recorded
generation for the victim address space is stale (wrapped since it
last ran there) — otherwise it must IPI, and the handler uses
`invpcid` type 0 (per-VA, per-PCID) where CPUID.7.0:EBX.10 says so,
falling back to a NOFLUSH-less CR3 reload of the victim PCID.
`cpu_cr3_shadow[]` grows a paired `cpu_as_generation[]`; the mailbox
grows the target PCID.

**D-PCID-5: the gate that re-opens this phase.**  A lane printing
`pcid=1` in the H0 receipt (KVM runner, metal serial capture) runs:
boot → full core shard → `/proc/perf` shows `cr3_noflush_switches`
moving and `tlb_ipis_skipped` NOT regressing to zero.  Until such a
lane exists, the implementation does not.

Counters reserved and visible now (`/proc/perf`: both names print at
0 — asserted in perf_smoke), so the first PCID-capable boot has its
receipt slots waiting.  No behavioural change anywhere else: the
diff is two enum rows, two name strings, and this text.

#### Test gate

- The design section reviewed against kernel/arch/x86_64/
  tlb_shootdown.c; receipts unchanged on both TCG lanes; no
  behavioural diff anywhere (byte-identity control).

#### Deliverable

`patches/HW_H4_pcid.patch`

### Phase H5 — Close-out: docs, CI, the hardware receipt protocol ✅ COMPLETE

**Objective:** the plan closes through checker arithmetic; the
hardware half becomes a protocol someone with metal can run in ten
minutes.

#### Tasks

- [x] `docs/status.md`: the feature rows (PAT/WC, PCID, ERMS) with
      their TCG-correctness/hardware-performance split stated.
- [x] CI: a `-cpu max` lane where it pays (the H2 smoke joins
      qemu-integration); the qemu64 lanes stay primary.
- [x] §6 receipt protocol finalised: exact commands, exact lines to
      paste back, and the table that holds them.
- [x] `check_hw_claims.py` closed to full coverage; terminal Status
      arithmetic.

#### Result

The docs carry the split honestly (a "✅ TCG-half" is a new status
word, and it means exactly what it says); the `-cpu max` lane runs
on every push now (detection + wiring, the halves TCG can prove);
§6 is a paste-the-line-back protocol with the exact expected
formats; and the checker closes the plan the D8 way — `## Status:
COMPLETE` is accepted only against six green rows, which this
paragraph could not precede.  Final count: 25 claims + selftest,
wired into test-unit since H0.

What this plan measured that its planner did not expect — the
series' real yield, in D1's spirit: TCG at ~1.2 G insn/s made byte
loops look healthy until the objdump said otherwise (H0→H1); clang
never unrolled them (the correction is dated, in H0's Result);
`-cpu max` has no PCID, which turned an implementation phase into a
design phase before any TLB code existed to regret (H0→H4); and the
one first-draft that put cpuid receipts in portable code was caught
by ratchet 2 within the minute (H0).  Every one of those catches
was a rig or a ratchet doing its job.

#### Test gate

- Checker green with terminal arithmetic; the CI lane runs; docs
  rows match the tree.

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
| rv64 memcpy 64 KiB (MB/s) | 249 | **814** | | |
| rv64 memcpy 1 MiB-eq (MB/s) | 413 | **2449** | | |
| rv64 memset 1 MiB-eq (MB/s) | 434 | **2553** | | |
| rv64 memmove-overlap 64 KiB (MB/s) | 370 | **887** | | |
| a64 memcpy 64 KiB (MB/s) | 197 | **623** | | |
| a64 memcpy 1 MiB-eq (MB/s) | 288 | **1964** | | |
| a64 memset 1 MiB-eq (MB/s) | 568 | **3178** | | |
| a64 memmove-overlap 64 KiB (MB/s) | 254 | **617** | | |
| x86 receipts (qemu64) | pat=1 pcid=0 invpcid=0 erms=0; PAT=0x0007040600070406 | | | |
| x86 receipts (-cpu max) | pat=1 **pcid=0 invpcid=0** erms=1; PAT same | | | |

## 6. Hardware receipt slots (D2 — filled only from metal)

The protocol: boot `release/auralite.iso` on the target machine with
a serial capture (or a phone camera on the console), run the listed
command, paste the listed line(s) back into this table with the
machine named.  Ten minutes, no toolchain needed on the target.

| Receipt | Command | Line(s) to paste | Machine / value |
|---|---|---|---|
| Feature ground truth | (boots by itself) | `[cpu]   features: ...` + `[vmm] IA32_PAT: PA4=WC (readback ...)` | — |
| ERMSB crossover active | (boots by itself) | `[cpu]   memcpy small-copy crossover: ...` | — |
| ERMSB small-copy throughput | `run membench` at the shell | the `MEMBENCH memcpy-a 64` and `4096` rows | — |
| WC framebuffer proof + flip rate | (boots by itself); then `cat /proc/perf` after ~30 s of GUI | `[vmm] fb: WC via PAT4 (...)` + both `compositor_pixels_*` counters, twice ~30 s apart | — |
| PCID present? (the D-PCID-5 trigger) | (boots by itself) | `[cpu]   features: ... pcid=1 invpcid=...` — if 1, HW_PLAN H4 re-opens | — |
| PCID win (post-re-open only) | `cat /proc/perf` after a busy minute | `cr3_noflush_switches N` (N > 0), `tlb_ipis_skipped` not collapsed | — |
