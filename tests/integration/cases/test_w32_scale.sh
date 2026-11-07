#!/usr/bin/env bash
# test_w32_scale.sh — RESIDUE3_PLAN.md phase C8 gate (the loader at scale).
#
# Audacity 3.7.5's gap ledger (w32/app_ledger/audacity-3.7.5.gap) records three
# walls, each fatal on its own:
#
#   1. SCALE  90932 base relocations against a `static pe_reloc_t relocs[16384]`
#             buffer in BOTH loaders -> "w32run: too many relocations" before a
#             single import was bound.
#   2. SHAPE  a 117-DLL bundle graph against a 64-slot module table and a
#             dependency depth of 8.
#   3. GAP    19 modules / 419 symbols with no personality.
#
# The real Audacity bundle is USER-SUPPLIED and never committed, so this case
# runs generated images carrying the same measured shape (tools/
# w32_scale_fixture.py, verified by the tree's own parser):
#
#   w32scale.exe   90932 DIR64 relocations exactly -> the loader prints the
#                  count and says "no fixed cap", then the image runs and exits
#                  0.  This is wall 1, gone.
#   w32gap.exe     3 missing symbols across 2 modules -> the refusal is a LEDGER
#                  (every module named + the total), not a sample of one.  This
#                  is wall 3, reported instead of hidden.
#   w32type3.exe   16 HIGHLOW entries -> refused BY TYPE ("type 3"), because a
#                  refusal that does not name the type is a bug report with the
#                  bug removed.
#
# The negative controls matter as much as the positives: the old loader's
# message "too many relocations" must be ABSENT from the whole log, and the gap
# run must not print a bare "unresolved import" and nothing else.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C8: the loader at Audacity's measured scale"

LOG="$IL_LOGDIR/w32_scale.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32scale\\.exe; then
    echo "  SKIP: the generated scale fixtures are not in the image"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32scale.exe"
il_send_delay 10
il_send "run /tests/w32gap.exe"
il_send_delay 6
il_send "run /tests/w32type3.exe"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 120

# --- wall 1: 90932 relocations, applied, no cap ----------------------------
il_assert_grep "$LOG" "w32run: \\[w32\\] relocations: 90932 applied \\(DIR64\\)" \
    "the 90932-relocation image relocated (Audacity's measured count)"
il_assert_grep "$LOG" "w32run: \\[w32\\] relocations: 90932 applied .*no fixed cap" \
    "the receipt says the cap is gone, not merely larger"
il_assert_no_grep "$LOG" "too many relocations" \
    "the old fixed-buffer refusal never fires (the whole point of the phase)"

# --- and the image actually RAN -------------------------------------------
il_assert_grep "$LOG" "w32run: calling entry at" \
    "an image past its 90932 fixups reaches its entry point"
il_assert_grep "$LOG" "w32run: entry point returned" \
    "the scale image returns cleanly (relocation did not corrupt the entry)"
il_assert_grep "$LOG" "'/tests/w32scale\\.exe' \\(tid [0-9]+\\) exited \\(code=0\\)" \
    "w32scale.exe exited 0"

# --- wall 3: the gap is a ledger, not a sample -----------------------------
il_assert_grep "$LOG" "w32run: unresolved import winmm\\.dll!waveOutOpen" \
    "the first missing symbol is still named"
il_assert_grep "$LOG" "w32run: gap  winmm\\.dll +2 symbol\\(s\\)" \
    "the ledger groups symbols by module (winmm: 2)"
il_assert_grep "$LOG" "w32run: gap  wsock32\\.dll +1 symbol\\(s\\)" \
    "the ledger names the SECOND module too (wsock32: 1)"
il_assert_grep "$LOG" "w32run: \\[w32\\] gap: 2 module\\(s\\), 3 symbol\\(s\\) missing" \
    "the summary counts modules and symbols, not just the first miss"
il_assert_grep "$LOG" "'/tests/w32gap\\.exe' \\(tid [0-9]+\\) exited \\(code=1\\)" \
    "an image with a gap still refuses to run (exit 1)"

# --- the shell routed all three through the user-space binder -------------
il_assert_grep "$LOG" "\[shell\] /tests/w32scale\.exe imports Win32 DLLs; running via /apps/w32run" \
    "w32scale.exe went through /apps/w32run, not the kernel PE loader"
il_assert_grep "$LOG" "\[shell\] /tests/w32type3\.exe imports Win32 DLLs; running via /apps/w32run" \
    "w32type3.exe went through /apps/w32run"

# --- refusals name what they refused --------------------------------------
il_assert_grep "$LOG" "w32run: \\[w32\\] relocation refused: unsupported relocation type \\(type 3 at RVA 0x2000\\)" \
    "a HIGHLOW image is refused BY TYPE and by RVA"
il_assert_grep "$LOG" "'/tests/w32type3\\.exe' \\(tid [0-9]+\\) exited \\(code=1\\)" \
    "w32type3.exe refused before running any of its code"

# --- no fault, no fallback ------------------------------------------------
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|PANIC" \
    "no fault across the three runs"
il_assert_no_grep "$LOG" "w32run: import binding failed \\(-1\\)" \
    "the gap image parsed (a parse failure would be a different, blunter message)"

il_summary
