#!/usr/bin/env bash
# test_selftest.sh — userspace regression app for usercopy, FD and socket API.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Userspace selftest app (usercopy + FD + socket API)"

LOG="$IL_LOGDIR/selftest.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "SELFTEST PASS: write rejects kernel pointer"      "write rejects kernel pointer"
il_assert_grep "$LOG" "SELFTEST PASS: write rejects null pointer"        "write rejects null pointer"
il_assert_grep "$LOG" "SELFTEST PASS: open rejects kernel path pointer"  "open rejects bad path pointer"
il_assert_grep "$LOG" "SELFTEST PASS: stat rejects kernel output pointer" "stat rejects bad output pointer"
il_assert_grep "$LOG" "SELFTEST PASS: stat accepts valid output pointer" "stat accepts valid output"
il_assert_grep "$LOG" "SELFTEST PASS: open valid file returns process fd" "per-process fd allocation works"
il_assert_grep "$LOG" "SELFTEST PASS: read valid file into user buffer"  "read copies to userspace"
il_assert_grep "$LOG" "SELFTEST PASS: socket create AF_INET/SOCK_STREAM" "socket creation works"
il_assert_grep "$LOG" "SELFTEST PASS: closesocket succeeds"             "socket close works"
# ---- dup / pipe / fcntl ----
il_assert_grep "$LOG" "SELFTEST PASS: dup returns >=3"                  "dup works"
il_assert_grep "$LOG" "SELFTEST PASS: dup2 to higher fd"                "dup2 works"
il_assert_grep "$LOG" "SELFTEST PASS: fcntl F_GETFD initial 0"          "fcntl GETFD baseline"
il_assert_grep "$LOG" "SELFTEST PASS: fcntl F_SETFD CLOEXEC"            "fcntl SETFD CLOEXEC"
il_assert_grep "$LOG" "SELFTEST PASS: fcntl F_GETFD == CLOEXEC"         "fcntl SETFD/GETFD round-trip"
il_assert_grep "$LOG" "SELFTEST PASS: pipe returns 0"                   "pipe creates two fds"
il_assert_grep "$LOG" "SELFTEST PASS: pipe write 5 bytes"               "pipe write works"
il_assert_grep "$LOG" "SELFTEST PASS: pipe read 5 bytes \(possibly across calls\)" "pipe read returns same bytes"
il_assert_grep "$LOG" "SELFTEST PASS: pipe rejects kernel out buffer"   "pipe rejects bad userspace buffer"
# ---- GUI ownership / bad pointers ----
il_assert_grep "$LOG" "SELFTEST PASS: ag_window_create returns >=0"     "GUI window created from selftest"
il_assert_grep "$LOG" "SELFTEST PASS: gui_clear on owned window"        "GUI clear works on owned window"
il_assert_grep "$LOG" "SELFTEST PASS: gui_clear rejects wid=999"        "GUI rejects out-of-range wid"
il_assert_grep "$LOG" "SELFTEST PASS: gui_clear rejects wid=-1"         "GUI rejects negative wid"
il_assert_grep "$LOG" "SELFTEST PASS: gui_draw_text rejects kernel string" "GUI draw_text validates text pointer"
il_assert_grep "$LOG" "SELFTEST PASS: gui_event rejects kernel out pointer" "GUI event validates user pointer"
il_assert_grep "$LOG" "SELFTEST PASS: gui_get_size valid pointer"       "GUI get_size accepts user pointer"
il_assert_grep "$LOG" "SELFTEST PASS: gui_get_size rejects bad wid"     "GUI get_size rejects bad wid"
il_assert_grep "$LOG" "SELFTEST PASS: gui op on destroyed wid fails"    "GUI rejects ops after destroy"
# (spawn/waitpid is covered separately by test_process_cleanup.sh; see the
# note inside selftest.c on why we skip it inside this binary.)
il_assert_grep "$LOG" "SELFTEST ALL PASS"                               "all selftests passed"
il_assert_no_grep "$LOG" "SELFTEST FAIL"                                "no selftest failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                          "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                                        "no panic"

il_summary
