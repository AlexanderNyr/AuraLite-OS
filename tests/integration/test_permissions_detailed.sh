#!/usr/bin/env bash
set -u
cd "$(dirname "$0")"
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "P7 UIDs, GIDs, File Permissions & Job Control (DETAILED)"

LOG="$IL_LOGDIR/permissions_detailed.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Increased delays for better reliability
il_send_delay 15
il_send "sleep 8 &"
il_send_delay 4
il_send "jobs"
il_send_delay 5
il_send "run /selftest"
il_send_delay 8
il_send "exit"

# Run with higher timeout (no hard limit)
il_run_qemu "$LOG" 120

echo ""
echo "=== FULL LOG (last 80 lines) ==="
tail -80 "$LOG"

il_assert_grep "$LOG" "\[1\]"                                         "background job started"
il_assert_grep "$LOG" "(Running|Running sleep)"                       "jobs builtin shows running job"
il_assert_grep "$LOG" "Done"                                          "background job reaped"
il_assert_grep "$LOG" "SELFTEST PASS: getuid is root initially"       "getuid returns 0 for root"
il_assert_grep "$LOG" "SELFTEST PASS: umask applied \(mode 0644\)"    "umask applied correctly"
il_assert_grep "$LOG" "SELFTEST PASS: chmod 0600"                     "chmod works"
il_assert_grep "$LOG" "SELFTEST PASS: open denied for non-owner"      "permission denial works"

il_summary
