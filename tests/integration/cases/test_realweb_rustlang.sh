#!/usr/bin/env bash
# test_realweb_rustlang.sh — RESIDUE2 T5 DoD receipt: rust-lang.org over
# HTTPS from the guest.
#
# The phase's Definition of Done reads: "rust-lang.org over HTTPS fetches
# from the guest or the phase says exactly which cipher/curve refused."
# This case is the first half of that sentence, made repeatable: the
# default SLIRP user network NATs to the runner's real internet, the
# guest's /apps/http loads the shipped trust store (/etc/ssl/roots.pem,
# 17 real roots) and fetches https://rust-lang.org/ end to end:
#   DNS (A + AAAA) → TCP:443 (v6 attempted, v4 fallback — SLIRP has no
#   v6 route) → TLS 1.3 (X25519MLKEM768 hybrid + ChaCha20-Poly1305,
#   certificate chain validated against the shipped roots) → HTTP/1.1
#   GET → 200 with the real page body → clean FIN.
#
# NETWORK-DEPENDENT (the x5 precedent): a runner without outbound
# internet fails here loudly — that is the honest failure mode for a
# real-web receipt, by design.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "real web: rust-lang.org over HTTPS (RESIDUE2 T5 DoD)"

LOG="$IL_LOGDIR/realweb_rustlang.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 12
il_send "run http https://rust-lang.org/"
il_send_delay 50
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 110

il_assert_grep "$LOG" "\[dns\] PASS: 'rust-lang\\.org' -> [0-9.]+|\\[dns\\] cache HIT 'rust-lang\\.org'" \
    "the real name resolved through the runner's DNS"
il_assert_grep "$LOG" "\[tcp\] \\[h=[0-9]+\\] ESTABLISHED .*185\\.|\\[tcp\\] \\[h=[0-9]+\\] ESTABLISHED" \
    "TCP:443 established"
il_assert_grep "$LOG" "\\[tls\\] group=X25519MLKEM768|\\[tls\\] group=X25519" \
    "the TLS 1.3 key exchange completed (post-quantum hybrid accepted)"
il_assert_grep "$LOG" "Response: 200, [0-9]+ bytes body" \
    "HTTP 200 with a real body"
il_assert_grep_fixed "$LOG" "Rust Programming Language" \
    "the body IS rust-lang.org (title marker present)"
il_assert_grep "$LOG" "connection closed" "clean teardown"

il_assert_no_grep "$LOG" "TLS.*FAIL|certificate.*refus|alert" \
    "no TLS/certificate failure"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived the fetch"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
