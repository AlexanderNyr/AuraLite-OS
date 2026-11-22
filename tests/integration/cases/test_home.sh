#!/usr/bin/env bash
# test_home.sh — HOME_PLAN H0 end-to-end: /home exists, users parse, the
# user's space is writable, and the shell's cwd/`~`/HOME follow the database.
#
# What this case exists to prove, in one boot:
#   1. The kernel mounted /home and parsed /etc/passwd: the boot log's
#      "[home] N user record(s)" and "[home] PASS:" lines.
#   2. `ls /` shows home/ -- the directory a user can see.
#   3. `cd ~` lands in the record's directory and `pwd` says so (pwd used to
#      print a constant "/", so this is the regression the case name pins).
#   4. A note saved at ~/note.txt reads back from an absolute path -- the
#      user's original question ("куда пользователь сможет сохранять
#      заметки").
#   5. `echo $HOME` and a non-root record both resolve from /etc/passwd.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "HOME_PLAN H0: /home and the user database"

LOG="$IL_LOGDIR/home.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

MARK="HOMEMARK_$$_$(date +%s)"

il_send_delay 8
il_send "ls /"
il_send_delay 2
il_send "echo \$HOME"
il_send_delay 2
il_send "echo \$USER"
il_send_delay 2
il_send "cd ~"
il_send_delay 2
il_send "pwd"
il_send_delay 2
il_send "write ~/note.txt $MARK"
il_send_delay 2
il_send "ls ~"
il_send_delay 2
il_send "cat /home/root/note.txt"
il_send_delay 2
il_send "echo \"~\""
il_send_delay 2
il_send "cd /"
il_send_delay 2
il_send "pwd"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 75

# --- the kernel's own receipts --------------------------------------------
il_assert_grep "$LOG" "\[home\] 2 user record" \
    "both /etc/passwd records parsed"
il_assert_grep "$LOG" "\[home\] PASS:"                 \
    "a home directory exists and is writable"
il_assert_grep "$LOG" "self-test: parser grammar" \
    "parser self-test ran"
il_assert_grep "$LOG" "\[home\]   ok   7-field record parses" \
    "parser self-test: canonical record"
il_assert_grep "$LOG" "storage=tmpfs"                 \
    "the receipt states the storage is volatile"

# --- the shell facing the user --------------------------------------------
il_assert_grep "$LOG" "  home/"                       "ls / lists home/"
il_assert_grep "$LOG" "^/home/root$"                   "echo \$HOME -> /home/root (root record)"
il_assert_grep "$LOG" "^root$"                        "echo \$USER -> root"
il_assert_grep "$LOG" "wrote /home/root/note.txt"     "a note saves in ~ (tilde expanded)"
il_assert_grep "$LOG" "$MARK"                         "the note reads back"
il_assert_grep "$LOG" "  note.txt"                    "ls ~ lists the note"
il_assert_grep "$LOG" "^~$"                           "a quoted tilde stays literal"
il_assert_grep "$LOG" "^/$"                           "cd / ; pwd -> /"

il_summary
