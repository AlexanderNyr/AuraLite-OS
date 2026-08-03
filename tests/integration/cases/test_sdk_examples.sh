#!/usr/bin/env bash
# test_sdk_examples.sh — applications built from the SDK run (SDK_PLAN S1/S2).
#
# tools/sdk_check.sh proves the examples BUILD against the staged SDK. This
# proves the result actually runs on the OS, which is a different claim: a
# binary can link cleanly, load, and still be wrong — a missing entry point or
# a dropped libc member produces exactly that.
#
# The examples are built from build/sdk only, with no path back into the
# source tree, and packed into the image by the Makefile.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "SDK example applications"

LOG="$IL_LOGDIR/sdk_examples.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run hello-app"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 30

il_assert_grep "$LOG" "HELLOAPP: hello from a third-party application" \
                                                  "an SDK-built program runs"
il_assert_grep "$LOG" "HELLOAPP: 2 \+ 3 = 5"      "libc works in an SDK-built program"
il_assert_grep "$LOG" "running /apps/hello-app"   "it resolved through the search path"

il_assert_no_grep "$LOG" "not found"              "the program was found"
il_assert_no_grep "$LOG" "PANIC"                  "no kernel panic"

il_summary
