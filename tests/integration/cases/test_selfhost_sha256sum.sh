#!/usr/bin/env bash
# test_selfhost_sha256sum.sh — SELFHOST_PLAN.md SH7a: in-guest sha256sum.
#
# SH7 is "image tooling in C": the pieces Fact 5 lists as host-only (tar,
# mtools, the BPB-patching python3) get C twins the guest can build and run,
# so `sh build.sh iso` can assemble the ISO in-guest.  SH7a is the first
# twin and the foundation the later receipts lean on: a /bin/sha256sum that
# reuses the single libatls SHA-256 (no second implementation), verified
# against the published FIPS vectors.
#
# The guest probe (sh7a_probe.sh) covers four things: the known-answer
# selftest, the stdin path, the file path, and a negative control that a
# content mismatch is actually detected.  The host unit test
# (tests/unit/test_sha256sum.c) pins the same logic at dev speed; this case
# proves the stripped ELF runs on AuraLite and the shell can branch on it.
#
# Needs no guest toolchain: /bin/sha256sum is a normal user ELF, so like the
# SH6 cases this never skips on a plain `make iso`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host image tooling (SH7a): in-guest sha256sum"

LOG="$IL_LOGDIR/selfhost_sha256sum.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7a_probe.sh"
# selftest hashes 1M bytes + four short hashes; give the TCG guest room.
il_send_delay 12
il_send "echo SH7A_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# NOTE: il_assert_grep uses grep -E, so regex metachars in the receipt must
# be escaped: the literal '+' in "selftest + stdin" is '\+', the parentheses
# around "(3 vectors)" are '\(' '\)'.  Same gotcha test_selfhost_build.sh hit
# with "kernel+initrd".
# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] sha256 PASS: selftest \+ stdin \+ file parity verified in-guest" \
    "the probe ran to completion and printed the SH7a receipt"

# ---- S1: the published vectors passed inside the guest ----
il_assert_grep "$LOG" "\[selfhost\] sha256 SELFTEST OK \(3 vectors\)" \
    "the FIPS known-answer selftest passed in-guest"

# ---- S3: file and stdin hashes of identical bytes match ----
il_assert_grep "$LOG" "\[selfhost\] sha256 MATCH: stdin == /tmp/sh7a_data.txt" \
    "stdin and the file hash of the same content agree"

# ---- S4: the negative control fired (a mismatch is detectable) ----
il_assert_grep "$LOG" "\[selfhost\] sh7a: s4-mismatch-detected" \
    "different content is reported as a mismatch, not a false match"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

# ---- the stdin coreutils-shaped line actually appeared (hex  name) ----
il_assert_grep "$LOG" "^[0-9a-f]{64}  -$" \
    "stdin hashing printed a 64-hex digest line"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH7A_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
