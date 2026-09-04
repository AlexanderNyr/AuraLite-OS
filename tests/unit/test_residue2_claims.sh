#!/usr/bin/env bash
# tests/unit/test_residue2_claims.sh — RESIDUE2_PLAN.md cannot drift from
# TODO.md and the ledger (RESIDUE2 T0; the GL2 L0 convention: run the
# checker, then prove it can fail).
set -u
cd "$(dirname "$0")/../.."

python3 tools/check_residue2_claims.py || {
    echo "[residue2-claims] FAILED"
    exit 1
}

# Negative control: a checker that never fails is indistinguishable from
# a clean tree.
python3 tools/check_residue2_claims.py --selftest || {
    echo "[residue2-claims] SELFTEST FAILED"
    exit 1
}

echo "[residue2-claims] all T0 claim gates passed"
