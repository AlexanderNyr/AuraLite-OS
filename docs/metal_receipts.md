# Metal receipts v2 — the user-executable package (RESIDUE_PLAN R11)

HW_PLAN §6 defined the protocol; this file is its v2 package: one
script, one boot, one paste-back section per receipt.  Ten minutes on
the target machine, no toolchain needed there.

## How to run it

```
tools/metal_receipts.sh            # builds release/auralite.iso, prints these steps
tools/metal_receipts.sh --null-test  # QEMU/TCG rehearsal: every receipt line prints
```

Boot `release/auralite.iso` on the target with a serial capture (or a
phone camera on the console).  WHPX/Hyper-V counts as metal for the
PCID block — that is the machine whose log already said `pcid=1
invpcid=0` and re-opened HW H4 (the D-PCID-5 trigger, RESIDUE R11
implemented it).

## The receipt slots (v2)

Paste the line(s) back into this table with the machine named.
`pending-user` is a STATUS, not a failure (plan D5/R11).

| # | Receipt | Command | Line(s) to paste | Machine / value |
|---|---------|---------|------------------|-----------------|
| 1 | Feature ground truth | (boots by itself) | `[cpu]   features: ...` + `[vmm] IA32_PAT: PA4=WC (readback ...)` | pending-user |
| 2 | ERMSB crossover active | (boots by itself) | `[cpu]   memcpy small-copy crossover: ...` | pending-user |
| 3 | ERMSB small-copy throughput | `run membench` at the shell | the `MEMBENCH memcpy-a 64` and `4096` rows | pending-user |
| 4 | WC framebuffer proof + flip rate | (boots by itself); then `cat /proc/perf` after ~30 s of GUI | `[vmm] fb: WC via PAT4 (...)` + both `compositor_pixels_*` counters, twice ~30 s apart | pending-user |
| 5 | **PCID enable (NEW: the code is in)** | (boots by itself) | `[cpu]   features: ... pcid=1 ...` AND `[vmm] PCID enabled (hash-slot alloc, CR3-reload+generation fallback, no invpcid)` | pending-user |
| 6 | **PCID win (NEW)** | `cat /proc/perf` after a busy minute (open apps, run programs) | `cr3_noflush_switches N` with N > 0, `pcid_generation_wraps M`, and `tlb_ipis_skipped` NOT collapsed to 0 | pending-user |
| 7 | **IOAPIC base discovery (NEW)** | (boots by itself) | `[ioapic] base 0x... (MADT agree)` — or the named-disagreement line, which is exactly the machine we want to hear about | pending-user |
| 8 | O3 wall-clock | stopwatch: power-on to `auralite#` | seconds, plus which boot path (BIOS/UEFI) | pending-user |
| 9 | Fast-boot A/B (fw_cfg is QEMU-only; on metal use the build knob) | `make SELFTEST=off iso` vs default, same stopwatch | the two wall-clock numbers | pending-user |

## The WHPX-targeted PCID block (receipt 5+6 in one sitting)

The user's own WHPX boot log is the ONLY known lane with `pcid=1`.
On that machine:

1. Boot `release/auralite.iso` under the same WHPX setup as the log
   that showed `pcid=1 invpcid=0`.
2. Confirm slot 5's two lines appear (feature line AND the
   `[vmm] PCID enabled` line — the second one proves CR4.PCIDE was
   actually set, not just detected).
3. Use the machine for ~a minute: open two GUI apps, `run membench`,
   switch windows.
4. `cat /proc/perf` — paste the `cr3_noflush_switches`,
   `pcid_generation_wraps`, `tlb_shootdowns_*` and `tlb_ipis_skipped`
   lines.
5. The D-PCID-5 acceptance: `cr3_noflush_switches` moving while
   `tlb_ipis_skipped` has not regressed to zero.

If any of this looks wrong ON THE METAL (a hang after the
`PCID enabled` line is the most informative failure this package can
produce), the serial capture of the last 20 lines is the receipt —
paste it, named as a failure.  That is D1 working, not the package
failing.

## What the NULL test proves (and what it cannot)

`--null-test` boots the ISO under QEMU TCG and greps that every
BOOT-TIME receipt line above still prints (slots 1, 2, 5-fallback
`pcid=0`, 7, and the perf counter NAMES through `/proc/perf`).  TCG
has no PCID (measured: `+pcid` → "TCG doesn't support requested
feature"), so slots 5/6 can only be format-checked here: the counters
must EXIST and must read 0 — a nonzero value on a pcid=0 boot would
mean the accounting fired without the feature, which the perf smoke
pins as a failure.
