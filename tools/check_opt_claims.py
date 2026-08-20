#!/usr/bin/env python3
"""Cross-check OPT_PLAN.md's status against the tree.

Why this exists
---------------
The same failure class AUDIT_A7 found in FIXES_PLAN.md and AUDIT_A3 in
MATURITY_PLAN.md: a plan's status lines are prose, and prose does not
fail a build.  OPT_PLAN.md itself documents two of its own phases whose
first drafts were wrong in ways only measurement caught (O1's rep-movsb
placebo, O6's phantom target) -- a plan with that history has no
business trusting its own checkboxes.

Ties every ✅ phase to the tree:
  - its deliverable patch exists in patches/;
  - its section says "Status: ✅ landed";
  - the O0 rig it all stands on is present AND registered (perfstat,
    /proc/perf, membench, the integration cases in run_all.sh, the
    host unit gates in the Makefile's UNIT_TESTS);
  - every perfstat counter the plan's §6 ledger quotes is still named
    in perfstat.c (a renamed counter silently orphans the ledger).

And checks the header itself, so the document cannot drift back.

Usage:
    tools/check_opt_claims.py [--check]
    tools/check_opt_claims.py --selftest   # prove the checker can fail
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def check_plan(plan, tree_has_patch):
    """Return a list of failure strings.  tree_has_patch(name) -> bool is
    injected so --selftest can plant a missing deliverable."""
    fails = []

    # Phase table rows: | O<n> — ... | <status> | `patches/<file>` |
    rows = re.findall(r"^\|\s*(O\d)\s+—[^|]*\|([^|]*)\|([^|]*)\|",
                      plan, re.M)
    if len(rows) < 10:
        fails.append("phase table: expected 10 rows O0..O9, found %d"
                     % len(rows))

    complete = set()
    for phase, status, deliverable in rows:
        if "✅" in status:
            complete.add(phase)
            m = re.search(r"patches/([A-Za-z0-9_.]+\.patch)", deliverable)
            if not m:
                fails.append("%s: marked ✅ but names no deliverable patch"
                             % phase)
                continue
            if not tree_has_patch(m.group(1)):
                fails.append("%s: deliverable patches/%s does not exist"
                             % (phase, m.group(1)))

    # Section status lines must agree with the table.
    for phase in complete:
        sec = re.search(r"^### %s —.*?(?=^### O|\Z)" % phase,
                        plan, re.M | re.S)
        if not sec:
            fails.append("%s: ✅ in the table but no section found" % phase)
            continue
        if "Status: ✅" not in sec.group(0):
            fails.append("%s: ✅ in the table but its section does not say "
                         "'Status: ✅'" % phase)

    # Header consistency: all ten complete <=> header says COMPLETE.
    header = plan.split("\n", 3)[2] if plan.count("\n") >= 3 else ""
    header_line = re.search(r"^## Status:.*$", plan, re.M)
    header = header_line.group(0) if header_line else ""
    if len(complete) == 10 and "COMPLETE" not in header:
        fails.append("all 10 phases are ✅ but the header does not say "
                     "COMPLETE: %r" % header)
    if len(complete) < 10 and "COMPLETE" in header:
        fails.append("header says COMPLETE but only %d/10 phases are ✅"
                     % len(complete))
    return fails


def check_rig():
    fails = []

    # The measuring rig O0 promised, present and wired.
    for path in ("kernel/lib/perfstat.c", "kernel/lib/perfstat.h",
                 "kernel/mm/sizeclass.h", "drivers/uart/uart_ring.h",
                 "kernel/arch/x86_64/tlb_policy.h",
                 "userspace/tests/membench/membench.c"):
        if not read(path):
            fails.append("rig file missing: %s" % path)

    if '"perf"' not in read("kernel/fs/procfs.c"):
        fails.append("/proc/perf is not served by procfs.c")

    run_all = read("tests/integration/run_all.sh")
    for case in ("test_perf_smoke", "test_selftest_modes",
                 "test_gui_dirty_uefi"):
        if not read("tests/integration/cases/%s.sh" % case):
            fails.append("integration gate missing: %s.sh" % case)
        if case not in run_all:
            fails.append("integration gate not REGISTERED (does not run): %s"
                         % case)

    makefile = read("Makefile")
    for gate in ("test_string_ops", "test_uart_ring", "test_tlb_policy",
                 "test_sizeclass"):
        if not read("tests/unit/%s.c" % gate):
            fails.append("host unit gate missing: tests/unit/%s.c" % gate)
        if ("$(BUILD_DIR)/%s" % gate) not in makefile:
            fails.append("host unit gate not in UNIT_TESTS: %s" % gate)

    # Every counter the §6 ledger stands on, still named in perfstat.c.
    perfstat = read("kernel/lib/perfstat.c")
    for counter in ("boot_ticks_to_shell", "compositor_frames_full",
                    "compositor_frames_partial",
                    "compositor_pixels_composited",
                    "compositor_pixels_flipped", "tlb_shootdowns_full",
                    "tlb_shootdowns_ranged", "tlb_ipis_skipped",
                    "kmalloc_walk_steps", "kmalloc_class_hits",
                    "uart_tx_sync_bytes", "uart_tx_ring_bytes"):
        if ('"%s"' % counter) not in perfstat:
            fails.append("perfstat counter renamed/gone: %s" % counter)
    return fails


def main():
    if "--selftest" in sys.argv:
        # Plant a violation: a ✅ phase whose patch does not exist.  The
        # checker must fail on it, or the checker itself is dead.
        plan = read("OPT_PLAN.md")
        fails = check_plan(plan, lambda name: name != "OPT_O1_stringops.patch")
        if any("OPT_O1_stringops.patch" in f for f in fails):
            print("check_opt_claims: SELFTEST OK (planted violation caught)")
            return 0
        print("check_opt_claims: SELFTEST FAILED -- the planted violation "
              "was not caught")
        return 1

    plan = read("OPT_PLAN.md")
    if not plan:
        print("check_opt_claims: FAIL -- OPT_PLAN.md missing")
        return 1

    def tree_has_patch(name):
        return bool(read("patches", name))

    fails = check_plan(plan, tree_has_patch) + check_rig()
    if fails:
        print("check_opt_claims: FAIL -- %d finding(s):" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("check_opt_claims: OK -- plan, patches, gates and counters agree "
          "with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
