#!/usr/bin/env bash
# test_w32_a1_loader.sh — W32APP_PLAN.md phase W32A-1 gate.
#
# Proves, in a booted system, that the loader binds the six W32A-1 shapes:
# ordinal imports (by number and by name, plus both refusals), delay-load
# (hit and miss), recursive loads (chain order, cycle refusal), data
# exports, forwarded exports (dynamic and static), and SxS manifests
# (v6/v5 select, silent none, admin + malformed refusals).
#
# Fifteen runs in one boot, each announced by an A1-Rnn sentinel.  Exit
# codes discriminate: 0 the fixture proved its claim, 1 the loader or the
# gate refused, 77 the delay helper named its missing DLL, 3 a fixture
# failed its own assertions (never expected).  Markers discriminate too:
# every fixture prints NAME-OK on success and NAME-FAIL only on failure,
# so the absence of every *-FAIL plus the presence of every *-OK is the
# proof, not just the exit codes.
#
# All fixtures are built in-tree by nasm -f win64 + lld-link + llvm-rc from
# committed sources (w32/tests/); no Microsoft file is involved.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-1 loader: ordinals, delay, recursion, data, forwarders, manifests"

LOG="$IL_LOGDIR/w32_a1_loader.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# lib.sh counts are "at least"; exit codes and repeated lines need EXACT.
a1_assert_exact() {
    local log="$1" pat="$2" want="$3" desc="$4" n
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    n="$(grep -cE "$pat" "$log" || true)"
    if [ "$n" -eq "$want" ]; then
        il_pass "$desc (got $n)"
    else
        il_fail "$desc (got $n, want $want)"
    fi
}

# First occurrence of $2 must precede the first of $3 (attach/detach order).
a1_assert_order() {
    local log="$1" first="$2" second="$3" desc="$4" a b
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    a="$(grep -nE "$first" "$log" | head -1 | cut -d: -f1)"
    b="$(grep -nE "$second" "$log" | head -1 | cut -d: -f1)"
    if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then
        il_pass "$desc (lines $a < $b)"
    else
        il_fail "$desc (lines '$a' vs '$b')"
    fi
}

il_send_delay 10
il_send "echo A1-R01-ordtest"
il_send "run /apps/w32run /tests/ordtest.exe"
il_send_delay 4
il_send "echo A1-R02-ordbadnum"
il_send "run /apps/w32run /tests/ordbadnum.exe"
il_send_delay 4
il_send "echo A1-R03-ordbadname"
il_send "run /apps/w32run /tests/ordbadname.exe"
il_send_delay 4
il_send "echo A1-R04-delaytest-present"
il_send "run /apps/w32run /tests/delaytest_present.exe"
il_send_delay 4
il_send "echo A1-R05-delaytest-absent"
il_send "run /apps/w32run /tests/delaytest_absent.exe"
il_send_delay 4
il_send "echo A1-R06-chainmain"
il_send "run /apps/w32run /tests/chainmain.exe"
il_send_delay 4
il_send "echo A1-R07-cycmain"
il_send "run /apps/w32run /tests/cycmain.exe"
il_send_delay 4
il_send "echo A1-R08-datamain"
il_send "run /apps/w32run /tests/datamain.exe"
il_send_delay 4
il_send "echo A1-R09-fwdmain"
il_send "run /apps/w32run /tests/fwdmain.exe"
il_send_delay 4
il_send "echo A1-R10-fwdstatic"
il_send "run /apps/w32run /tests/fwdstatic.exe"
il_send_delay 4
il_send "echo A1-R11-mantest-v6"
il_send "run /apps/w32run /tests/mantest_v6.exe"
il_send_delay 4
il_send "echo A1-R12-mantest-v5"
il_send "run /apps/w32run /tests/mantest_v5.exe"
il_send_delay 4
il_send "echo A1-R13-mantest-none"
il_send "run /apps/w32run /tests/mantest_none.exe"
il_send_delay 4
il_send "echo A1-R14-mantest-admin"
il_send "run /apps/w32run /tests/mantest_admin.exe"
il_send_delay 4
il_send "echo A1-R15-mantest-bad"
il_send "run /apps/w32run /tests/mantest_bad.exe"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 150

# --- ordinals ------------------------------------------------------------
il_assert_grep "$LOG" "ORD-OK" \
    "ordinal imports bound: TODO stubs, BSTR round-trip, name/number alias"
il_assert_grep "$LOG" "w32run: unresolved import COMCTL32\\.dll!#9999" \
    "an unmapped ordinal refuses BY NUMBER"
il_assert_grep "$LOG" "w32run: unresolved import COMCTL32\\.dll!NoSuchFunction" \
    "an unknown name refuses BY NAME"
il_assert_no_grep "$LOG" "ORD-FAIL" \
    "no ordinal fixture failed its own assertions"

# --- delay-load -----------------------------------------------------------
il_assert_grep "$LOG" "DELAY-LAZY-OK" \
    "the delay target stays unloaded until first call"
il_assert_grep "$LOG" "DELAY-PRESENT-OK" \
    "the delay call resolves and returns 42"
il_assert_grep_fixed "$LOG" "DELAY-LOADFAIL nosuchdll.dll" \
    "the miss path exits 77 naming the DLL"
il_assert_no_grep "$LOG" "DELAY-FAIL" \
    "no delay fixture failed its own assertions"

# --- recursion ------------------------------------------------------------
il_assert_grep "$LOG" "CHAIN-OK" \
    "the recursive chain loaded, ran (8), and saw both modules"
a1_assert_order "$LOG" "CHAIN-B-ATTACH" "CHAIN-A-ATTACH" \
    "attach runs dependencies-first"
a1_assert_order "$LOG" "CHAIN-A-ATTACH" "CHAIN-A-DETACH" \
    "attach precedes detach"
a1_assert_order "$LOG" "CHAIN-A-DETACH" "CHAIN-B-DETACH" \
    "teardown runs in reverse"
il_assert_grep_fixed "$LOG" "dependency cycle: cyc_c.dll -> cyc_d.dll -> cyc_c.dll" \
    "the import cycle refuses with the cycle named"
il_assert_no_grep "$LOG" "CHAIN-FAIL" \
    "the chain fixture never failed"
il_assert_no_grep "$LOG" "CYC-FAIL" \
    "no instruction of the cyclic program ran"

# --- data exports ---------------------------------------------------------
il_assert_grep "$LOG" "DATA-OK" \
    "the imported DWORD read 42 and the neighbouring function ran"
il_assert_no_grep "$LOG" "DATA-FAIL" \
    "no data fixture failed"

# --- forwarders -----------------------------------------------------------
il_assert_grep "$LOG" "FWD-OK" \
    "dynamic GetProcAddress of a forwarder: NULL + 127 + FreeLibrary"
a1_assert_exact "$LOG" "forwarder exports are not supported \\(target kernel32\\.GetTickCount64\\)" 2 \
    "both forwarder refusals name the target"
il_assert_grep "$LOG" "w32run: unresolved import fwdtest\\.dll!FwdFunc" \
    "the static forwarder refuses at bind"
il_assert_no_grep "$LOG" "FWD-FAIL" \
    "no forwarder fixture failed"

# --- manifests ------------------------------------------------------------
a1_assert_exact "$LOG" "MAN-OK" 3 \
    "v6, v5 and none ran; admin and bad never reached start"
il_assert_grep "$LOG" "w32run: manifest: comctl v6, exec=asInvoker, dpi-aware" \
    "the v6 manifest selects v6 + asInvoker + dpi"
il_assert_grep "$LOG" "w32run: manifest: comctl v5, exec=asInvoker" \
    "the 5.82 manifest selects v5 (the version discriminates)"
a1_assert_exact "$LOG" "w32run: manifest: comctl" 2 \
    "only the runnable manifests print a gate line"
a1_assert_exact "$LOG" "supportedOS stanza ignored" 2 \
    "both supportedOS stanzas are loud"
il_assert_grep "$LOG" "w32run: refused: manifest requests requireAdministrator" \
    "requireAdministrator refuses before mapping"
il_assert_grep "$LOG" "w32run: refused: malformed application manifest" \
    "the malformed blob refuses as malformed"

# --- the exit-code triad --------------------------------------------------
# Eight fixtures prove their claims (0), six loader/gate refusals (1), one
# delay miss (77).  Attributed to w32run so the shell's own exit cannot
# miscount; 3 must never appear anywhere.
a1_assert_exact "$LOG" "'/apps/w32run' \\(tid [0-9]+\\) exited \\(code=0\\)" 8 \
    "eight runs proved their claims"
a1_assert_exact "$LOG" "'/apps/w32run' \\(tid [0-9]+\\) exited \\(code=1\\)" 6 \
    "six runs refused cleanly"
a1_assert_exact "$LOG" "'/apps/w32run' \\(tid [0-9]+\\) exited \\(code=77\\)" 1 \
    "one delay miss exited 77"
il_assert_no_grep "$LOG" "exited \\(code=3\\)" \
    "no fixture failed its own assertions"

# --- nothing broke ---------------------------------------------------------
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault across fifteen runs"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived all fifteen runs and exited cleanly"

il_summary
