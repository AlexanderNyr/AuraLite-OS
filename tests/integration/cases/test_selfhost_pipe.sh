#!/usr/bin/env bash
# test_selfhost_pipe.sh — SELFHOST_PLAN.md SH6c: pipes and command lists.
#
# SH6a gave the shell an exit status, SH6b redirects and variables.  SH6c
# makes commands compose, so a build step can be one line instead of a
# temporary file:
#
#   - pipes.  `a | b` on the kernel's existing SYS_PIPE -- the plan's "no
#     kernel change needed" is right for pipes (the pipe exception in
#     SYS_WRITE predates SH6b; it is how gterm captures a child's stdout).
#     Stages run sequentially in the shell, each one's stdout wired to the
#     next one's stdin through a real kernel pipe.
#   - command lists.  `;`, `&&` and `||`, consuming the status spine: the
#     next element runs (or is skipped) on the previous element's status.
#
# The gate shape the plan calls out: a failing first element whose failure
# propagates through `&&` (L2 in the probe), and the POSIX pipeline-status
# rule that makes the failing-LAST-stage shape (P4) the one a build script
# uses.
#
# Needs no guest toolchain, so like SH6a's and SH6b's cases it never skips.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host scripting (SH6c): pipes and command lists"

LOG="$IL_LOGDIR/selfhost_pipe.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6c_probe.sh"
il_send_delay 4
# pipes and lists at the prompt, not only inside a script
il_send "echo prompt-pipe | cat > /tmp/sh6c_prompt.txt"
il_send_delay 1
il_send "cat /tmp/sh6c_prompt.txt"
il_send_delay 1
il_send "run sh6c-no-such-program && echo UNREACHABLE-PROMPT"
il_send_delay 1
il_send "run sh6c-no-such-program || echo prompt-or-ran"
il_send_delay 1
il_send "echo SH6C_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] pipe PASS: 4 pipelines ran" \
    "the probe script ran to completion"

# ---- P1/P2: data flowed through 2- and 3-stage pipes into a file ----
il_assert_grep "$LOG" "^\[selfhost\] sh6c: p1$" \
    "the 2-stage pipeline wrote its line (read back from the file)"
il_assert_grep "$LOG" "^\[selfhost\] sh6c: p2$" \
    "the 3-stage pipeline appended its line (read back from the file)"

# ---- P3: $? after successful pipelines is 0 ----
il_assert_grep "$LOG" "^\[selfhost\] sh6c: status-after-p2=0$" \
    "\$? carries the last pipeline's status"

# ---- L1: `;` continues after a failure ----
il_assert_grep "$LOG" "^\[selfhost\] sh6c: semicolon-continued$" \
    "; ran the next element despite the failure"

# ---- L2: a failing first element stops the && chain ----
il_assert_grep "$LOG" "sh6c-no-such-program: not found in" \
    "the failing element did run (and was reported)"
il_assert_no_grep "$LOG" "UNREACHABLE-AND" \
    "the failure propagated through && (the partner was skipped)"
il_assert_grep "$LOG" "^\[selfhost\] sh6c: and-survived$" \
    "the script continued after the skipped && partner"

# ---- L3: || runs after a failure ----
il_assert_grep "$LOG" "^\[selfhost\] sh6c: or-continued$" \
    "|| ran the next element after the failure"

# ---- P4: a pipeline whose last stage fails stops the && behind it ----
il_assert_no_grep "$LOG" "UNREACHABLE-AND2" \
    "a failing last stage propagates through the && too (POSIX status)"
il_assert_grep "$LOG" "^\[selfhost\] sh6c: pipe-and-survived$" \
    "the script continued after the failing pipeline"

# ---- P5: a pipeline wrote the file and a redirect read it back ----
il_assert_grep "$LOG" "^roundtrip$" \
    "a pipeline wrote the file and cat read it back"

# ---- pipes and lists at the prompt, not only inside a script ----
il_assert_grep "$LOG" "^prompt-pipe$" \
    "a pipe typed at the prompt wrote the file"
il_assert_grep "$LOG" "^prompt-or-ran$" \
    "a || list typed at the prompt ran its partner"
il_assert_no_grep "$LOG" "^UNREACHABLE-PROMPT$" \
    "a failing && element at the prompt skipped its partner"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH6C_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
