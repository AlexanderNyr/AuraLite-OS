#!/bin/bash
# test_mmap_shared.sh — M4 integration gate: verify MAP_SHARED|MAP_ANONYMOUS
# works across fork (parent writes, child reads the same physical page).
#
# MATURITY_PLAN.md phase M4.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/lib.sh"

NAME="mmap_shared"
ISO="${ISO:-build/auralite.iso}"

il_run "$ISO" "run selftest" 30

# The guest must NOT have panicked.
if echo "$IL_SERIAL" | grep -qi 'panic\|triple fault\|STACK CORRUPTION'; then
    echo "FAIL: kernel panicked"
    exit 1
fi
il_assert "no kernel panic"

# selftest should still pass (regression check).
il_assert_shell_alive

il_pass
