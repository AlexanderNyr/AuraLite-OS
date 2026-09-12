#!/usr/bin/env python3
"""Cross-check REALTEK_PLAN.md's phase claims against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md (and pinned by
the FSFULL F6 / OTA O5 / LX L5 checkers): a plan's status lines are
prose, and prose does not fail a build.  REALTEK_PLAN.md lands the
RTL8169/8168/8111 gigabit driver — the one catalogued NIC QEMU cannot
emulate, so its data path is proved against a register-level host model
instead — across four phases (RT0..RT3).  Each phase's "done" is only
durable if the artefacts and greppable RECEIPTS the phase claims still
exist in the tree.

Ties the plan to the tree:
  - every phase RT0..RT3 has a `### Phase RTx` section;
  - the Status line says COMPLETE;
  - done phases (✅ DONE in the heading) are backed by the files the
    phase names — the descriptor surface (RT1), the driver core + chip
    model + host gates (RT2), the net_init wiring + catalog/docs flip +
    metal slot + this checker (RT3) — AND by their greppable receipts
    (the `r8169` catalog key, the `[r8169]` receipt strings, the
    `r8169_init()` chain entry, the `PENDING-USER@RT3` ledger row).
  - the RT3 wiring is structural: this checker runs in `make test-unit`
    and in the workflow's claim-check step.

Deliberately NOT asserted: that a QEMU case exists (there is none by
construction — no 8169 model) and that the metal slot is filled (it
ships PENDING-USER, a status, not a failure; the user's paste-back is
the number's only source).

Usage:
    tools/check_realtek_claims.py --check
    tools/check_realtek_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASES = ["RT%d" % n for n in range(0, 4)]

# Durable artefacts each done phase must have (existence checks).
ARTEFACTS = {
    "RT0": ["docs/plans/REALTEK_PLAN.md",
            "tools/residue_baseline.txt"],
    "RT1": ["drivers/r8169/r8169_desc.h",
            "tests/unit/test_r8169_desc.c",
            "Makefile"],
    "RT2": ["drivers/r8169/r8169_core.h",
            "drivers/r8169/r8169.h",
            "drivers/r8169/r8169.c",
            "tests/unit/r8169_model.h",
            "tests/unit/test_r8169_driver.c",
            "Makefile"],
    "RT3": ["kernel/net/net.c",
            "drivers/vm/virtual_drivers.c",
            "docs/status.md",
            "docs/driver_guide.md",
            "docs/virtual_driver_matrix.md",
            "docs/metal_receipts.md",
            "docs/residue_ledger.md",
            "tools/check_realtek_claims.py",
            ".github/workflows/integration.yml",
            "tools/residue_baseline.txt"],
}

# Greppable receipts: (phase, path, required substring).  A phase marked
# done whose receipt vanished from the file is a lie about the tree.
RECEIPTS = [
    ("RT0", "tools/residue_baseline.txt", "REALTEK_PLAN.md"),
    ("RT1", "drivers/r8169/r8169_desc.h", "R8169_TNPDS_LOW"),
    ("RT1", "drivers/r8169/r8169_desc.h", "r8169_rx_classify"),
    ("RT1", "tests/unit/test_r8169_desc.c", "test_r8169_desc"),
    ("RT1", "Makefile", "test_r8169_desc"),
    ("RT2", "drivers/r8169/r8169_core.h", "struct r8169_io"),
    ("RT2", "drivers/r8169/r8169_core.h", "r8169_core_send"),
    ("RT2", "drivers/r8169/r8169_core.h", "r8169_core_recv"),
    ("RT2", "drivers/r8169/r8169.c", "[r8169] found"),
    ("RT2", "drivers/r8169/r8169.c", "[r8169] RX via IRQ wake"),
    ("RT2", "drivers/r8169/r8169.c", "r8169_register_netdev"),
    ("RT2", "tests/unit/r8169_model.h", "r8169_model_tx_drain"),
    ("RT2", "tests/unit/r8169_model.h", "r8169_model_rx_deliver"),
    ("RT2", "tests/unit/test_r8169_driver.c", "model_self_test"),
    ("RT2", "tests/unit/test_r8169_driver.c", "test_full_ring_wrap"),
    ("RT2", "Makefile", "test_r8169_driver"),
    ("RT3", "kernel/net/net.c", "r8169_init() == 0"),
    ("RT3", "kernel/net/net.c", "r8169_register_netdev"),
    ("RT3", "drivers/vm/virtual_drivers.c", '"r8169"'),
    ("RT3", "drivers/vm/virtual_drivers.c", "host-model data path"),
    ("RT3", "docs/status.md", "[r8169] RX via IRQ wake"),
    ("RT3", "docs/status.md", "r8169 → rtl8139"),
    ("RT3", "docs/driver_guide.md", "drivers/r8169/"),
    ("RT3", "docs/virtual_driver_matrix.md", "host-model data path"),
    ("RT3", "docs/metal_receipts.md", "RTL8169/8168 real-silicon data path"),
    ("RT3", "docs/residue_ledger.md", "RES-56"),
    ("RT3", "docs/residue_ledger.md", "PENDING-USER@RT3"),
    ("RT3", "Makefile", "check_realtek_claims.py"),
    ("RT3", ".github/workflows/integration.yml", "check_realtek_claims.py"),
    ("RT3", "tools/residue_baseline.txt", "REALTEK_PLAN.md"),
]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def check_plan(read_fn):
    """Return a list of failure strings.  read_fn is injected so
    --selftest can plant violations."""
    fails = []
    plan = read_fn("docs", "plans", "REALTEK_PLAN.md")

    if not plan:
        return ["REALTEK_PLAN.md missing or unreadable"]

    # All four phase sections exist.
    seen = set(re.findall(r"^### Phase (RT\d)\b", plan, re.M))
    for phase in PHASES:
        if phase not in seen:
            fails.append("phase table: missing section %s" % phase)

    # The plan is a closed series: COMPLETE once RT3 lands.
    status_line = ""
    for line in plan.splitlines():
        if line.startswith("## Status:"):
            status_line = line
            break
    if "COMPLETE" not in status_line:
        fails.append("status: plan Status line is not COMPLETE "
                     "(got: %r)" % status_line.strip())

    done = set()
    for m in re.finditer(r"^### Phase (RT\d)\b.*?\bDONE\b", plan, re.M):
        done.add(m.group(1))
    for phase in PHASES:
        if phase not in done:
            fails.append("%s: phase not marked done (the series is "
                         "COMPLETE; a pending phase contradicts it)" % phase)

    # Artefacts and receipts for every done phase.
    for phase, arts in ARTEFACTS.items():
        if phase not in done:
            continue
        for art in arts:
            if not read_fn(*art.split("/")):
                fails.append("%s: marked done but %s missing" % (phase, art))
    for phase, path, needle in RECEIPTS:
        if phase not in done:
            continue
        if needle not in read_fn(*path.split("/")):
            fails.append("%s: receipt %r missing from %s"
                         % (phase, needle, path))

    return fails


def main():
    if "--selftest" in sys.argv:
        # Planted violation 1: a done phase's artefact is missing.
        fails = check_plan(lambda *p: "" if "/".join(p) ==
                           "tests/unit/test_r8169_driver.c" else
                           read(*p))
        if not any("RT2: marked done but tests/unit/"
                   "test_r8169_driver.c missing" in f for f in fails):
            print("check_realtek_claims: SELFTEST FAILED -- planted "
                  "missing-artefact violation not caught")
            return 1
        # Planted violation 2: a receipt vanished from a present file.
        def no_receipt(*p):
            txt = read(*p)
            if "/".join(p) == "kernel/net/net.c":
                return txt.replace("r8169_init() == 0", "r8169_init() == -1")
            return txt
        fails = check_plan(no_receipt)
        if not any("RT3: receipt 'r8169_init() == 0' missing" in f
                   for f in fails):
            print("check_realtek_claims: SELFTEST FAILED -- planted "
                  "missing-receipt violation not caught")
            return 1
        # Planted violation 3: the plan's status is not COMPLETE.
        def pending_plan(*p):
            if p == ("docs", "plans", "REALTEK_PLAN.md"):
                return read(*p).replace(
                    "## Status: COMPLETE", "## Status: OPEN", 1)
            return read(*p)
        fails = check_plan(pending_plan)
        if not any("status: plan Status line is not COMPLETE" in f
                   for f in fails):
            print("check_realtek_claims: SELFTEST FAILED -- planted "
                  "not-COMPLETE violation not caught")
            return 1
        print("check_realtek_claims: SELFTEST OK (planted violations caught)")
        return 0

    fails = check_plan(read)
    if fails:
        for f in fails:
            print("check_realtek_claims: FAIL -- %s" % f)
        print("check_realtek_claims: %d claim(s) disagree with the tree"
              % len(fails))
        return 1
    print("check_realtek_claims: OK -- the Realtek plan matches the tree "
          "(%d phases, %d artefact + %d receipt pins)"
          % (len(PHASES), sum(len(v) for v in ARTEFACTS.values()),
             len(RECEIPTS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
