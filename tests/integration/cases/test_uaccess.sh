#!/bin/bash
# test_uaccess.sh — M3 integration gate: run /tests/usertest in QEMU and
# assert every hostile-pointer test returns -EFAULT with no kernel panic.
#
# MATURITY_PLAN.md phase M3.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../lib/lib.sh"

NAME="uaccess"
ISO="${ISO:-build/auralite.iso}"

il_run "$ISO" "run usertest" 30

# The guest must NOT have panicked or triple-faulted.
if echo "$IL_SERIAL" | grep -qi 'panic\|triple fault\|STACK CORRUPTION\|resetting'; then
    echo "FAIL: kernel panicked or triple-faulted on hostile pointer"
    exit 1
fi
il_assert "no kernel panic on hostile pointers"

# usertest reports its own pass/fail count.
if echo "$IL_SERIAL" | grep -q '== .* passed =='; then
    line=$(echo "$IL_SERIAL" | grep '== .* passed ==' | tail -1)
    passed=$(echo "$line" | sed 's/.*== \([0-9]*\)\/\([0-9]*\) passed ==.*/\1/')
    total=$(echo "$line" | sed 's/.*== \([0-9]*\)\/\([0-9]*\) passed ==.*/\2/')
    if [ "$passed" -eq "$total" ] && [ "$total" -ge 30 ]; then
        il_assert "usertest $passed/$total passed (>= 30)"
    else
        echo "FAIL: usertest $passed/$total"
        exit 1
    fi
else
    echo "FAIL: usertest did not report results"
    exit 1
fi

# The shell must have survived (no crash).
il_assert_shell_alive

il_pass
