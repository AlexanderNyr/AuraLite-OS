#!/usr/bin/env bash
# test_selfhost_bootoffsets.sh — SELFHOST_PLAN.md SH7c: in-guest boot-offset
# generator / verifier.
#
# SH7c proves the guest computes the same boot_info_t layout the host does,
# so the boot-offset header needs no host step in the self-host loop.  The
# host unit test (test_bootoffsets_twin.c) is compiled with the real
# host-generated build/boot_offsets.h and asserts every offsetof() matches
# byte-for-byte; this case proves the stripped in-guest ELF (/bin/bootoffsets)
# runs on AuraLite and emits/verifies the layout.  No guest toolchain needed.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host image tooling (SH7c): in-guest boot_offset generator"

LOG="$IL_LOGDIR/selfhost_bootoffsets.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh7c_probe.sh"
il_send_delay 8
il_send "echo SH7C_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] boot-offset header PASS: generated in-guest" \
    "the probe ran to completion and printed the SH7c receipt"

# ---- S1: the in-guest --check reports a consistent layout ----
il_assert_grep "$LOG" "sh7c: s1-layout-verified" \
    "bootoffsets --check ran and verified the guest boot_info_t layout"
il_assert_grep "$LOG" "sizeof\(boot_info_t\) = 7776" \
    "the guest struct sizeof matches the host (7776)"

# ---- S2/S3: both the C header and NASM inc forms regenerate ----
# The receipts are `[selfhost] sh7c: s2-c-header-generated` / `s3-asm-inc-generated`,
# but an SMP kernel `[thread] reaped ...` line can land on the same serial line
# and split the `[selfhost] sh7c: ` prefix off the tail, so match the tail
# text alone (the exit-status branches above already prove the tool ran).
il_assert_grep "$LOG" "s2-c-header-generated" \
    "the C boot_offsets.h form regenerated in-guest"
il_assert_grep "$LOG" "s3-asm-inc-generated" \
    "the NASM boot_offsets.inc form regenerated in-guest"

# ---- S4: negative control (unknown mode rejected) ----
il_assert_grep "$LOG" "sh7c: s4-bad-usage-rejected" \
    "an unknown mode fails rather than falsely succeeding"
il_assert_no_grep "$LOG" "UNREACHABLE" \
    "no probe branch that should be unreachable was taken"

il_assert_grep "$LOG" "^SH7C_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
