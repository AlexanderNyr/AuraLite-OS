#!/usr/bin/env bash
# test_selfhost_mkinitrd.sh — SELFHOST_PLAN.md SH7b: in-guest USTAR writer.
#
# SH7b is the second "image tooling in C" twin: a /bin/mkinitrd that writes
# the USTAR archive the kernel initrd parser already reads, replacing host
# `tar` in the in-guest image loop.  The host unit test (test_mkinitrd.c)
# proves GNU tar accepts the output and every member round-trips; this case
# proves the stripped ELF runs on AuraLite and the shell can branch on it.
#
# The guest probe (sh7b_probe.sh) covers: the built-in write->reparse round
# trip, packing a fresh tree, the tool reading its own archive back (--list),
# and a negative control (a missing archive is rejected).  Needs no guest
# toolchain: /bin/mkinitrd is a normal user ELF, so like SH7a this never
# skips on a plain `make iso`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host image tooling (SH7b): in-guest mkinitrd USTAR writer"

LOG="$IL_LOGDIR/selfhost_mkinitrd.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7b_probe.sh"
# the selftest packs /tests (dozens of members); give the TCG guest room.
il_send_delay 12
il_send "echo SH7B_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] mkinitrd PASS: members written in-guest" \
    "the probe ran to completion and printed the SH7b receipt"

# ---- S1: the built-in write->reparse round trip passed in-guest ----
il_assert_grep "$LOG" "\[selfhost\] mkinitrd round-trip OK \([0-9]+ members\)" \
    "the tool packed a tree and re-parsed its own archive"

# ---- S2: a freshly packed archive lists its members back ----
il_assert_grep "$LOG" "sh7b: s2-archive-listed" \
    "the guest-written archive is re-parseable via --list"

# ---- S3: the negative control fired (a missing archive is rejected) ----
il_assert_grep "$LOG" "sh7b: s3-missing-archive-rejected" \
    "listing a nonexistent archive fails rather than falsely succeeding"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH7B_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
