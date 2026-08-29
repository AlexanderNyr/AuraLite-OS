#!/usr/bin/env bash
# test_selfhost_redirect.sh — SELFHOST_PLAN.md SH6b: redirects and variables.
#
# SH6a gave the shell a script runner and an exit status.  SH6b adds the two
# things a build script cannot do without:
#
#   - redirection.  `tcc ... > log` and `cat < a > b`.  Implemented on the
#     kernel's existing SYS_DUP2 -- measured in the SH6 survey, no kernel
#     change was needed.
#   - named variables.  `set CC=tcc` then `$CC`, so a build.sh can hold the
#     toolchain in one place instead of repeating paths.
#
# Both needed a quote-aware tokenizer first, and that is the part most likely
# to be wrong: the old strtok(line, " \t\n") made every `>` an operator, so
# `echo "a > b"` could not be printed at all.  Quotes and redirects are
# therefore recognised in ONE pass (userspace/system/init/sh_parse.h), because
# whether `>` is an operator depends on whether it is quoted.
#
# The strongest assertion here is `cat < $LOG > /tmp/sh6b_copy.txt`: both
# directions on one line.  A parser that only handled `>` would still pass
# every other check while `<` silently did nothing.
#
# Needs no guest toolchain, so like SH6a's case it never skips.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host scripting (SH6b): redirects and shell variables"

LOG="$IL_LOGDIR/selfhost_redirect.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6b_probe.sh kernel"
il_send_delay 3
il_send "set"
il_send_delay 2
il_send "echo prompted > /tmp/sh6b_prompt.txt"
il_send_delay 1
il_send "cat /tmp/sh6b_prompt.txt"
il_send_delay 1
il_send "echo boom > /nonexistent-dir/nope.txt"
il_send_delay 1
il_send "sh /tests/sh6b_fail.sh"
il_send_delay 2
il_send "echo SH6B_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\[selfhost\] redirect PASS: 2 files written and read back" \
    "the probe script ran to completion"

# ---- `>` wrote, `<` read back, and the copy proves both ----
il_assert_grep "$LOG" "^\[selfhost\] sh6b: copy follows$" \
    "the script reached the cat-through-both-redirects step"
il_assert_grep "$LOG" "^\[selfhost\] sh6b: target=kernel$" \
    "> wrote a line whose target came from \$1 via a variable, and < read it back"
il_assert_grep "$LOG" "^second line$" \
    ">> appended rather than truncating"

# ---- a quoted `>` is text ----
il_assert_grep "$LOG" "a > b is text, not a redirect" \
    "a > inside double quotes did not become a redirect"

# ---- named variables ----
il_assert_grep "$LOG" "^\[selfhost\] sh6b: variable expanded$" \
    "\$MSG expanded from a value assigned with set"
il_assert_grep "$LOG" "^LOG=/tmp/sh6b.log$" \
    "set with no arguments lists the table"
il_assert_grep "$LOG" "^\[selfhost\] sh6b: after-unset=\[\]$" \
    "unset removed the variable, it did not merely empty it"

# ---- redirects at the prompt, not only inside a script ----
il_assert_grep "$LOG" "^prompted$" \
    "a redirect typed at the prompt wrote the file"

# ---- a redirect that cannot be opened is refused, not ignored ----
il_assert_grep "$LOG" "cannot open for >" \
    "an unopenable redirect target is reported"

# ---- parse errors are refused with a status, and name the line ----
il_assert_grep "$LOG" "sh6b: fail probe" \
    "the line before the parse error did run"
il_assert_grep "$LOG" "sh: unmatched quote" \
    "an unmatched quote is refused instead of tokenized into something else"
il_assert_grep "$LOG" "sh: /tests/sh6b_fail.sh:8: command failed with status 2" \
    "the failing line is reported with its line number"
il_assert_no_grep "$LOG" "UNREACHABLE-AFTER-QUOTE-ERROR" \
    "the script stopped at the malformed line"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH6B_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary
