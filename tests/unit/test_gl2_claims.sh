#!/usr/bin/env bash
# tests/unit/test_gl2_claims.sh — GL2_PLAN.md cannot drift from the tree
# (GL2_PLAN L0; the check_rinet2_claims.py convention: run the checker,
# then prove it can fail).
set -u
cd "$(dirname "$0")/../.."

python3 tools/check_gl2_claims.py || {
    echo "[gl2-claims] FAILED"
    exit 1
}

# Negative control: a checker that never fails is indistinguishable from
# a clean tree.
python3 tools/check_gl2_claims.py --selftest || {
    echo "[gl2-claims] SELFTEST FAILED"
    exit 1
}

echo "[gl2-claims] all L0 claim gates passed"
