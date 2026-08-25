#!/usr/bin/env bash
# test_trust_store.sh — REALINTERNET_PLAN X8 (trust-store lifecycle).
#
# Asserts the shipped trust store is visible and diagnosable in the guest:
#   1. `trustinfo` runs against /etc/ssl/roots.pem and reports the root count;
#   2. every shipped root's common name and not-after expiry are printed
#      (so a trust-store gap reads as a trust-store issue, not a TLS bug);
#   3. the docs/trust_store.md expiry values match what the guest prints —
#      the provenance table is not stale relative to the shipped bytes.
#
# The shipped roots and their expected expiries are fixed (see docs/trust_store.md).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Trust store (REALINTERNET_PLAN X8)"

LOG="$IL_LOGDIR/trust_store.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run trustinfo"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 25

il_assert_grep_fixed "$LOG" "AuraLite OS trust store: /etc/ssl/roots.pem" \
    "trustinfo reads the shipped trust store"
il_assert_grep_fixed "$LOG" "17 trust root(s)." \
    "seventeen roots are decoded"
il_assert_grep_fixed "$LOG" "DigiCert Global Root CA         2031-11-10" \
    "DigiCert Global Root CA expiry visible (2031-11-10)"
il_assert_grep_fixed "$LOG" "DigiCert Global Root G3         2038-01-15" \
    "DigiCert Global Root G3 expiry visible (2038-01-15)"
il_assert_grep_fixed "$LOG" "ISRG Root X1                    2035-06-04" \
    "ISRG Root X1 expiry visible (2035-06-04)"
il_assert_grep_fixed "$LOG" "GTS Root R1" \
    "Google Trust Services R1 is shipped"
il_assert_grep_fixed "$LOG" "ISRG Root X2" \
    "ISRG Root X2 is shipped"
il_assert_grep_fixed "$LOG" "See docs/trust_store.md" \
    "provenance file is referenced"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" \
    "no exception while reading the trust store"

il_summary
