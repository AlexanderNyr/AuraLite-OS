#!/usr/bin/env bash
# test_execvpe_lanes.sh — RESIDUE2 T4 gate: execvpe()/fexecve() proven live.
#
# The prototypes existed since P10 but nothing in the tree ever CALLED
# them; userspace/tests/execvetest/execvetest.c is the four-lane proof
# program and this case drives every lane through the real shell:
#
#   fexec     fexecve(open("/tests/argv_echo")) replaces the image; the
#             successor's ARGV_ECHO markers prove THIS call marshalled the
#             argv/envp the successor actually saw (XEC_FEXEC / XEC=1).
#   vpe       execvpe("argv_echo", ...) with envp PATH=/tests:/bin — the
#             global environment does NOT carry /tests, so a resolve is
#             proof the search read the CHILD's environment.
#   vpe-cwd   PATH=":/bin": the empty first segment is the current
#             directory; execvetest chdir()s to /tests first, so finding
#             ./argv_echo is proof the POSIX empty-segment rule works (and
#             that the search loop did not spin forever on it).
#   vpe-miss  a name that is nowhere must RETURN -1/ENOENT to the caller
#             — the only lane whose success is a normal return.
#
# The first three lanes never return on success; their pass output comes
# from the successor image, so seeing XEC_* markers in the successor's
# dump is the whole point.  A lane that fails prints "XEC ... FAILED"
# with errno — asserted absent.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "execvpe/fexecve lanes (RESIDUE2 T4)"

LOG="$IL_LOGDIR/execvpe_lanes.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run execvetest vpe-miss"
il_send_delay 4
il_send "run execvetest fexec"
il_send_delay 6
il_send "run execvetest vpe"
il_send_delay 6
il_send "run execvetest vpe-cwd"
il_send_delay 6
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 90

# ---- lane 1: vpe-miss is the returning lane --------------------------------
il_assert_grep_fixed "$LOG" "XEC_VPE_MISS_OK ENOENT" \
    "execvpe of a name that is nowhere returned -1 with ENOENT"

# ---- lane 2: fexecve --------------------------------------------------------
il_assert_grep_fixed "$LOG" "XEC fexecve calling (fd=" \
    "the fexec lane reached the call"
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=XEC_FEXEC" \
    "fexecve: the successor saw THIS call's argv"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[0]=XEC=1" \
    "fexecve: the successor saw THIS call's envp"

# ---- lane 3: execvpe searches the CHILD's PATH ------------------------------
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=XEC_VPE" \
    "execvpe: resolved argv_echo and marshalled argv"
il_assert_grep_fixed "$LOG" "ARGV_ECHO env[0]=PATH=/tests:/bin" \
    "execvpe: the search used the child envp's PATH (not the global one)"

# ---- lane 4: the empty PATH segment is the current directory ----------------
il_assert_grep_fixed "$LOG" "ARGV_ECHO argv[1]=XEC_CWD" \
    "execvpe: PATH=\":/bin\" resolved ./argv_echo from /tests"

# ---- no lane failed, kernel survived ----------------------------------------
il_assert_no_grep "$LOG" "XEC .* FAILED" "no lane took its failure path"
il_assert_grep_fixed "$LOG" "gate-end" "shell survived all four lanes"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
