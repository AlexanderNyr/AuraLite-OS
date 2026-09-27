#!/usr/bin/env bash
# W32A-11: kernel pathname tokens stay owned, one-shot and generation-safe
# across destruction/recycling of the exact same compositor window slot.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "W32A-11 cross-process file-drop token lifecycle"
LOG="$IL_LOGDIR/w32a11_tokens.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send "run /tests/w32a11_token_sender"
il_send_wait "A11-TOKEN-SENDER-OK" 55
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 105

il_assert_grep "$LOG" "A11-TOKEN-REUSE-OK" \
    "recycled HWND rejects still-busy stale token without clobbering buffer"
il_assert_grep "$LOG" "A11-TOKEN-ACL-OK" \
    "different PID cannot consume current owner's live token"
il_assert_grep "$LOG" "A11-TOKEN-OK" \
    "owner consumes new token, reads bytes and refuses one-shot replay"
il_assert_grep "$LOG" "A11-TOKEN-SENDER-OK" "sender reaped receiver"
il_assert_grep "$LOG" "'/tests/w32a11_token_sender' \(tid [0-9]+\) exited \(code=78\)" \
    "native sender success status"
il_assert_no_grep "$LOG" "A11-TOKEN-FAIL|kernel panic|Page Fault" \
    "no guest fault or fixture failure"
il_assert_grep "$LOG" "Goodbye!" "shell survived"
il_summary
