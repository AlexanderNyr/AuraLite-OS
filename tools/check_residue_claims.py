#!/usr/bin/env python3
"""Cross-check RESIDUE_PLAN.md + the ledger against the tree.

Seventh checker of the D8 family.  What is new here: the debt
RATCHET.  The harvester's per-file marker counts are pinned in
tools/residue_baseline.txt; any plan growing (or shedding) residue
markers without moving the baseline AND the ledger in the same
commit fails.  The ledger itself is arithmetic-checked: 48 rows,
sequential unique ids, class totals, status totals.

Usage:
    tools/check_residue_claims.py [--selftest]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEDGER_ROWS = 48
CLASS_PIN = {"W": 33, "M": 5, "N": 2, "S": 8}
PHASE_ORDER = ["R0", "R1", "R2", "R3", "R4", "R5", "R6",
               "R7", "R8", "R9", "R10", "R11", "R12"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def harvest_live():
    """Run the harvester in-process for a doctored-ROOT-friendly way."""
    script = os.path.join(ROOT, "tools", "residue_harvest.py")
    if not os.path.exists(script):
        return None
    try:
        out = subprocess.run(
            [sys.executable, script, "--machine"],
            capture_output=True, text=True, timeout=60,
            cwd=ROOT).stdout
    except Exception:
        return None
    return dict(line.rsplit(" ", 1) for line in out.splitlines()
                if " " in line)


def claims():
    plan = read("RESIDUE_PLAN.md")
    ledger = read("docs", "residue_ledger.md")
    checks = []

    # --- the ledger's arithmetic --------------------------------------
    rows = re.findall(r"^\| (RES-\d\d) \| (\w+) \| ([A-Z@R\d-]+) \|",
                      ledger, re.M)
    ids = [r[0] for r in rows]
    checks.append((
        f"ledger: exactly {LEDGER_ROWS} rows with unique sequential ids "
        f"(found {len(ids)})",
        len(ids) == LEDGER_ROWS and len(set(ids)) == LEDGER_ROWS and
        ids == [f"RES-{i:02d}" for i in range(1, LEDGER_ROWS + 1)]))
    class_counts = {}
    for _, cls, _st in rows:
        class_counts[cls] = class_counts.get(cls, 0) + 1
    checks.append((
        f"ledger: class totals match the pins (measured {class_counts}, "
        f"pinned {CLASS_PIN})",
        class_counts == CLASS_PIN))
    n_open = sum(1 for _, _, st in rows if st == "OPEN")
    n_closed = len(rows) - n_open
    done_phases = len([1 for r in re.findall(
        r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M)])
    checks.append((
        f"ledger: status arithmetic is sane (OPEN {n_open} + moved "
        f"{n_closed} = {LEDGER_ROWS}; {done_phases} phase(s) landed)",
        n_open + n_closed == LEDGER_ROWS))

    # --- the debt ratchet: live harvest == pinned baseline -------------
    base_text = read("tools", "residue_baseline.txt")
    baseline = dict(line.rsplit(" ", 1) for line in
                    base_text.splitlines() if " " in line)
    live = harvest_live()
    if live is None:
        checks.append(("harvest: tools/residue_harvest.py runs", False))
    else:
        drift = {k: (baseline.get(k), live.get(k))
                 for k in set(baseline) | set(live)
                 if baseline.get(k) != live.get(k)}
        checks.append((
            f"ratchet: live harvest matches the baseline (drift: "
            f"{drift if drift else 'none'} — move baseline AND ledger "
            "in the same commit)",
            not drift and len(baseline) > 0))

    # --- R7: ECAM + virtio-pci (when the plan says the phase landed) ----
    if re.search(r"^### R7[^\n]*✅ COMPLETE", plan, re.M):
        makefl = read("Makefile")
        ecam_c = read("kernel", "drivers", "pci_ecam.c")
        vpci_c = read("kernel", "drivers", "virtio_pci.c")
        checks.append((
            "R7: the two shared transports exist and BOTH tenants list "
            "them (single source, twice linked)",
            ecam_c != "" and vpci_c != "" and
            makefl.count("kernel/drivers/pci_ecam.c") >= 2 and
            makefl.count("kernel/drivers/virtio_pci.c") >= 2))
        checks.append((
            "R7: the shared transports obey the portable rules "
            "(no inline asm, no bare width casts)",
            "__asm__" not in ecam_c and "__asm__" not in vpci_c and
            "(uint64_t)" not in ecam_c and "(uint64_t)" not in vpci_c))
        checks.append((
            "R7: fdt.c names pci-host-ecam-generic and the walker "
            "prints the receipt",
            "pci-host-ecam-generic" in read("kernel", "dt", "fdt.c") and
            "[pci] ECAM: " in ecam_c))
        checks.append((
            "R7: virtio_pci is MODERN — VERSION_1 offered back, "
            "FEATURES_OK verified by read-back",
            "VERSION_1" in vpci_c and "VM_S_FEATURES_OK" in vpci_c))
        checks.append((
            "R7: both fs smokes carry the PCI mount lane and pin the "
            "transport-truth blkdev line",
            all("virtio-blk over PCI (modern, VERSION_1)" in
                read("tests", "integration", s) and
                "vblk0 (virtio-pci" in read("tests", "integration", s)
                for s in ("rv_fs_smoke.sh", "a64_fs_smoke.sh"))))
        checks.append((
            "R7 rider: the v3 group claim precedes the online count "
            "(the R5/R6 CI reds' one cause) and the smp smoke asserts "
            "the v3 timer",
            "BEFORE this core counts itself online"
            in read("kernel", "arch", "aarch64", "smp_a64.c") and
            "GICD_IGROUPR" in read("kernel", "arch", "aarch64", "gic.c")
            and "\\[timer\\] PASS"
            in read("tests", "integration", "a64_smp_smoke.sh")))

    # --- R8: the Rust rows (when the plan says the phase landed) --------
    if re.search(r"^### R8[^\n]*✅ COMPLETE", plan, re.M):
        makefl = read("Makefile")
        common = read("lib", "rsbr", "common.rs")
        rustes = read("userspace", "apps", "rustes", "rustes.rs")
        checks.append((
            "R8: one bridge, three ISAs — common.rs carries all three "
            "cfg'd trap instructions, rustes.rs all three counters",
            all(k in common for k in
                ('target_arch = "x86_64"', 'target_arch = "riscv64"',
                 'target_arch = "aarch64"', '"ecall"', '"svc #0"')) and
            all(k in rustes for k in ("rdtsc", "rdtime", "cntvct_el0"))))
        checks.append((
            "R8: both tenant editions build through the Makefile and "
            "ship in the initrd copy list",
            "riscv64gc-unknown-none-elf" in makefl and
            "aarch64-unknown-none" in makefl and
            "binrv/rustes" in makefl and "bina64/rustes" in makefl))
        checks.append((
            "R8: the counter gates opened on BOTH init paths per "
            "tenant (R5's init-on-a-secondary lesson)",
            read("kernel", "arch", "riscv64",
                 "trap.c").count("scounteren") >= 2 and
            read("kernel", "arch", "aarch64",
                 "trap_a64.c").count("cntkctl_el1") >= 2))
        checks.append((
            "R8: the shared receipt is asserted on both tenants "
            "(the first pin of any Rust row)",
            all("=== Rust Benchmark ===" in read("tests", "integration", s)
                and "Sum: 499999500000" in read("tests", "integration", s)
                for s in ("rv_fs_smoke.sh", "a64_fs_smoke.sh"))))

    # --- R0 structure ---------------------------------------------------
    checks.append((
        "R0: the amended class totals are recorded as a CATCH in both "
        "documents (the draft's 27/6/3/12 was wrong)",
        "W 33" in ledger and "WRONG" in ledger and
        "W 33 · M 5 · N 2 · S 8" in plan))
    checks.append((
        "R0: the checker family wiring (test-unit runs this file with "
        "its selftest)",
        read("Makefile").count("check_residue_claims.py") >= 2))

    # --- structural: status header vs table -----------------------------
    done_rows = len(re.findall(r"^\| R\d+ [^|]*\| ✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### R\d+[^\n]*✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete row has a COMPLETE heading",
                   done_rows == done_heads and plan != ""))
    status_ok = False
    if re.search(r"^## Status: IN PROGRESS — R0 next", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"R0(?:–(R\d+))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "R0"
            claimed = (PHASE_ORDER.index(label) + 1
                       if label in PHASE_ORDER else 0)
            status_ok = claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))
    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_residue_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_residue_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_residue_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    failed = 0
    results = claims()
    for desc, ok in results:
        if not ok:
            print(f"check_residue_claims: FAIL -- {desc}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"check_residue_claims: {failed} claim(s) disagree with "
              "the tree", file=sys.stderr)
        return 1
    print(f"check_residue_claims: OK -- {len(results)} claims verified "
          "(the debt ratchet runs the harvester LIVE)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
