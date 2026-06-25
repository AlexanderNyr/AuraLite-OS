#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ISO="${1:-build/auralite.iso}"
if [ ! -f "$ISO" ]; then
    echo "Build ISO first."
    exit 1
fi

echo "Running basic fs stress test."
# Currently we test it by just seeing if standard CI succeeds for now.
bash scripts/ci_test.sh
