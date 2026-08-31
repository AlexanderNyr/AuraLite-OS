#!/usr/bin/env bash
# test_selfhost_shmake.sh — SELFHOST_PLAN.md SH6e: shmake honours a graph.
#
# SH6a–SH6d gave the shell a status spine, redirects, pipes, lists and
# control flow.  SH6e is the build driver those features exist to run:
# rules, prerequisites, `CC = ...` / `$(CC)`, `.PHONY` (host-tested) and
# timestamp comparison.
#
# The load-bearing assertion is the incremental rebuild: after touching
# a.in, GEN=2 rebuilds a.out and app and does NOT rebuild b.out.  That
# is what distinguishes a dependency graph from a shell script with
# comments.
#
# Needs no guest toolchain: /bin/shmake is a normal user ELF, so like
# SH6a–SH6d this case never skips.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host build driver (SH6e): shmake honours a graph"

LOG="$IL_LOGDIR/selfhost_shmake.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6e_probe.sh"
# two sleep 2 inside the probe, plus three shmake runs
il_send_delay 16
il_send "echo SH6E_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
# The receipt is `[selfhost] shmake PASS: 3 targets up to date`, but the
# kernel's `[thread] reaped ...` line can land on the same serial line and
# split the `[selfhost]` prefix from the rest, so match the receipt text
# alone (the exit-0 run and every behavioural assertion above already prove
# the probe completed).
il_assert_grep "$LOG" "shmake PASS: 3 targets up to date" \
    "the probe script ran to completion"

# ---- cold run (GEN=1): every recipe ran ----
il_assert_grep "$LOG" "\\[selfhost\\] sh6e: g1 rebuilt a.out" \
    "the cold run built a.out"
il_assert_grep "$LOG" "\\[selfhost\\] sh6e: g1 rebuilt b.out" \
    "the cold run built b.out"
il_assert_grep "$LOG" "\\[selfhost\\] sh6e: g1 rebuilt app" \
    "the cold run linked app"

# ---- incremental (GEN=2): only what depends on a.in ----
il_assert_grep "$LOG" "\\[selfhost\\] sh6e: g2 rebuilt a.out" \
    "touching a.in rebuilt a.out"
il_assert_grep "$LOG" "\\[selfhost\\] sh6e: g2 rebuilt app" \
    "touching a.in rebuilt app (depends on a.out)"
il_assert_no_grep "$LOG" "g2 rebuilt b.out" \
    "touching a.in did not rebuild b.out"
il_assert_grep "$LOG" "shmake: 'b.out' is up to date" \
    "b.out was reported up to date on the incremental run"

# ---- no-op run (GEN=3): the receipt's 3 targets ----
il_assert_grep "$LOG" "\\[shmake\\] rebuilt=0 uptodate=3" \
    "a third run with no changes rebuilt nothing"
il_assert_no_grep "$LOG" "g3 rebuilt" \
    "the no-op run invoked no recipe"

# ---- $(CC) actually expanded (recipe echo, not the stamp) ----
il_assert_grep "$LOG" "/tests/sh6e_stamp 1 a.out a.in" \
    "\$(CC) expanded to the stamp program"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH6E_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
