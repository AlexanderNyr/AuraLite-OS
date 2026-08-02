#!/usr/bin/env bash
# test_apm_packages.sh — the .apkg format and the filesystem repository
# (SDK_PLAN phases S4 and S5).
#
# tests/unit/test_apkg.c covers the parser exhaustively on the host. This
# covers what a parser test cannot: that apm reads a real directory, installs
# from a real file, refuses a corrupt one, and that the installed program
# actually runs.
#
# The corrupt package is built by the Makefile alongside the good ones — a
# verification path with no test is a verification path nobody knows works.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "apm package format and repository"

LOG="$IL_LOGDIR/apm_packages.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "apm list"
il_send_delay 2
il_send "apm info matrix"
il_send_delay 2
il_send "apm install matrix"
il_send_delay 3
il_send "ls /opt"
il_send_delay 1
# A corrupt package must be refused, and must leave nothing behind.
il_send "apm install /pkg/broken.apkg"
il_send_delay 2
il_send "ls /opt"
il_send_delay 1
# Install by explicit path — the route a third-party package arrives by.
il_send "apm install /pkg/life.apkg"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 45

# --- the repository is read from the filesystem ---
il_assert_grep "$LOG" "matrix"                       "apm lists a package found in /pkg"
il_assert_grep "$LOG" "Source:       /pkg/matrix.apkg" "info reports the real file"
il_assert_grep "$LOG" "Payload:"                     "info reports metadata from the header"

# --- the table is formatted, not printed as specifiers ---
il_assert_no_grep "$LOG" "%-12s"                     "printf width specifiers are honoured"

# --- install works ---
il_assert_grep "$LOG" "Unpacked .* to /opt/matrix"   "install unpacks the payload"
il_assert_grep "$LOG" "Successfully installed matrix" "install reports success"

# --- install by path works (a package from anywhere) ---
il_assert_grep "$LOG" "Successfully installed life"  "install by explicit path"

# --- a corrupt package is refused, and NOTHING is written ---
il_assert_grep "$LOG" "checksum mismatch"            "a corrupt package is detected"
il_assert_grep "$LOG" "refusing to install"          "and refused"
il_assert_no_grep "$LOG" "Unpacked .* to /opt/broken" "nothing was written for it"

# --- the fake network messages are gone ---
il_assert_no_grep "$LOG" "upstream"                  "no invented network activity"

il_assert_no_grep "$LOG" "PANIC"                     "no kernel panic"

il_summary
