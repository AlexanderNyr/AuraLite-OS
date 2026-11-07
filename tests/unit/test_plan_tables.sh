#!/usr/bin/env bash
# test_plan_tables.sh — the plan-table / orphan-debt checker (RESIDUE3 C0).
#
# WHY THIS TEST EXISTS
#
# Three ways a status drifts away from the tree without any claim changing,
# all three measured on 26cb7fe:
#
#   1. a ledger row whose own phase says it closes, while the row still
#      reads HANDED-OFF (RES-38, RES-39, RES-47);
#   2. a plan's phase table disagreeing with that same plan's per-phase
#      status (W32APP_PLAN.md's five "⬜ planned" rows; RESIDUE2 T7);
#   3. a TODO box tagged with a phase that is already ✅ (all seven open
#      boxes carried RESIDUE2 tags for closed phases).
#
# None of the seven existing claim checkers reads tables, box tags or the
# Definition-of-done paragraph, so none of the three could ever fail a build.
# tools/check_plan_tables.py does, and its live violations are pinned in
# tools/plan_tables_baseline.txt the way the residue ratchet pins marker
# counts: a new violation is red, and so is a fixed one whose pin did not
# move in the same commit.
#
# Run by `make test-unit`.  Python only: no build, no QEMU.

set -u
cd "$(dirname "$0")/../.."

fail=0

echo "[unit] check_plan_tables --selftest (a doctored tree must fail)"
if ! python3 tools/check_plan_tables.py --selftest; then
    echo "  FAIL: the checker does not detect its own planted violations"
    fail=1
fi

echo "[unit] check_plan_tables --check (live tree against the pin)"
if ! python3 tools/check_plan_tables.py --check; then
    echo "  FAIL: the plan tables, TODO tags or ledger drifted out of the pin"
    echo "  fix the drift (a plan phase owns it) or, when the drift is the"
    echo "  point, move tools/plan_tables_baseline.txt in the SAME commit"
    fail=1
fi

echo "[unit] check_plan_tables --openers (quoted line counts re-derived)"
python3 tools/check_plan_tables.py --openers || fail=1

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "[unit] test_plan_tables: PASS"
