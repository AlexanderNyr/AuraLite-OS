#!/usr/bin/env bash
# tests/unit/test_opt_claims.sh — OPT_PLAN.md cannot drift from the tree
# (OPT_PLAN O9; the check_fixes_claims.py convention: run the checker,
# then prove it can fail).
set -u
cd "$(dirname "$0")/../.."

python3 tools/check_opt_claims.py --check || {
    echo "[opt-claims] FAILED"
    exit 1
}

# Negative control: a checker that never fails is indistinguishable from
# a clean tree.
python3 tools/check_opt_claims.py --selftest || {
    echo "[opt-claims] SELFTEST FAILED"
    exit 1
}

echo "[opt-claims] all O9 claim gates passed"
