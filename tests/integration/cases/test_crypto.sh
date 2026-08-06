#!/usr/bin/env bash
# test_crypto.sh — INTERNET_PLAN.md phase N1 in-guest gate.
#
# The RFC-vector batteries run host-side (tests/unit/test_atls_*).  This
# case proves the SAME sources also run inside the OS: /tests/cryptotest
# links libatls.a, runs one vector per primitive plus the refusal paths
# (tampered AEAD tag, low-order X25519 point, forged Ed25519 signature),
# and reports on the serial console.  It also exercises the 64 KiB guest
# user stack — the field code's scratch usage matters here, exactly as
# GL phase G11b discovered (WEBVIEW_PLAN §2 precedent).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N1: libatls in-guest smoke test"

LOG="$IL_LOGDIR/crypto.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run cryptotest"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 60

il_assert_grep "$LOG" "auralite#"                "shell reached"
il_assert_grep "$LOG" "\[cryptotest\] PASS: SHA-256"                     "SHA-256 vector in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: HMAC-SHA256"                 "HMAC vector in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: HKDF RFC 5869 TC1 expand"    "HKDF vector in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: AEAD RFC 8439"               "AEAD vector in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: AEAD tampered tag refused"   "AEAD refusal in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: X25519 RFC 7748"             "X25519 vector in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: X25519 low-order point refused" "X25519 refusal in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: Ed25519 RFC 8032"            "Ed25519 verify in guest"
il_assert_grep "$LOG" "\[cryptotest\] PASS: Ed25519 forged signature refused" "Ed25519 refusal in guest"
il_assert_grep "$LOG" "\[cryptotest\] ALL PASS"                          "cryptotest reports ALL PASS"

il_assert_no_grep "$LOG" "\[cryptotest\] FAIL"   "no in-guest crypto failure"
il_assert_no_grep "$LOG" "KERNEL EXCEPTION"      "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"

il_summary
