#!/usr/bin/env bash
# tests/unit/test_gpu_claims.sh — GPU_ACCEL_PLAN.md cannot drift from the tree
# (GPU_ACCEL_PLAN H0; the check_gl2_claims.py convention: run the checker,
# then prove it can fail).
set -u
cd "$(dirname "$0")/../.."

python3 tools/check_gpu_claims.py || {
    echo "[gpu-claims] FAILED"
    exit 1
}

# Negative control: a checker that never fails is indistinguishable from
# a clean tree.
python3 tools/check_gpu_claims.py --selftest || {
    echo "[gpu-claims] SELFTEST FAILED"
    exit 1
}

echo "[gpu-claims] all H0 claim gates passed"
