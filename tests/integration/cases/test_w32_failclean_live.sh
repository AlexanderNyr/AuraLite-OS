#!/usr/bin/env bash
# test_w32_failclean_live.sh — RESIDUE3_PLAN.md phase C8, box 3.
#
# docs/win32.md generates 35 functions across nine modules as FAIL-CLEAN: they
# bind, and then report an honest failure rather than a success-shaped lie.
# Until this case, that was a claim about SOURCE CODE — the personality's own
# host tests call each function, but nothing proved that a real PE, loaded
# through w32run's binder and calling by NAME, gets the same answer on the
# machine.
#
# w32c8_failclean.exe imports all 35 by name, calls each with the smallest
# arguments that reach the refusal, checks the documented answer and prints:
#
#     [w32] FAIL-CLEAN <fn> -> <code> (as documented)
#
# Three of the 35 are documented degradations rather than refusals, and the
# receipt says so in the tail instead of flattening them into "failed":
# AdjustTokenPrivileges (TRUE + ERROR_NOT_ALL_ASSIGNED), DwmGetColorizationColor
# (S_OK + opaque black), and the SENSAPI pair (a real reachability probe).
# Those are asserted for the behaviour they actually have.
#
# Gate shape: the fixture exits 78 on a clean run, 1 if any function answered
# something other than its documented answer, and the case asserts every one of
# the 35 receipts by name — a silently missing receiver is a failure, not a
# smaller count.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C8: live FAIL-CLEAN receipts (35 functions, 9 modules)"

LOG="$IL_LOGDIR/w32_c8_failclean.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32c8_failclean; then
    echo "  SKIP: the C8 FAIL-CLEAN fixture is not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32c8_failclean.exe"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture ran and passed its own table ------------------------------
il_assert_grep "$LOG" "C8-FAILCLEAN-OK" \
    "the fixture checked all 35 documented answers in-guest"
il_assert_grep "$LOG" "'/tests/w32c8_failclean\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32c8_failclean.exe exited 78"
il_assert_no_grep "$LOG" "C8-FAILCLEAN-FAIL" \
    "no function answered something other than its documented answer"
il_assert_no_grep "$LOG" "FAIL-[A-Za-z_]+:" \
    "no individual FAIL-CLEAN check failed"
il_assert_grep "$LOG" "imports Win32 DLLs; running via /apps/w32run" \
    "the fixture went through the real binder, by name"

# --- ADVAPI32: 9 (one of them a documented degradation) --------------------
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN AdjustTokenPrivileges -> TRUE \\(degraded: ERROR_NOT_ALL_ASSIGNED" \
    "AdjustTokenPrivileges degrades: TRUE + ERROR_NOT_ALL_ASSIGNED"
for f in GetFileSecurityW LookupAccountNameW LookupPrivilegeValueW \
         LsaAddAccountRights LsaClose LsaOpenPolicy OpenProcessToken \
         SetFileSecurityW; do
    il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN $f -> " \
        "advapi32!$f reported its documented refusal"
done

# --- COMCTL32/COMdlg32: PrintDlgW ------------------------------------------
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN PrintDlgW -> FALSE \\(as documented: PDERR_NODEFAULTPRN" \
    "PrintDlgW refuses with PDERR_NODEFAULTPRN"

# --- CRYPT32: 8 ------------------------------------------------------------
for f in CertCloseStore CertFindCertificateInStore \
         CertGetCertificateContextProperty CertGetNameStringW CertNameToStrW \
         CryptMsgClose CryptMsgGetParam CryptQueryObject; do
    il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN $f -> " \
        "crypt32!$f reported its documented refusal"
done

# --- DWMAPI: one refusal, one documented constant --------------------------
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN DwmSetWindowAttribute -> 0x80004001 \\(as documented: E_NOTIMPL" \
    "DwmSetWindowAttribute refuses with E_NOTIMPL"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN DwmGetColorizationColor -> S_OK 0xFF000000 \\(constant:" \
    "DwmGetColorizationColor reports the documented opaque-black constant"

# --- IMM32: 10 -------------------------------------------------------------
for f in ImmEscapeW ImmGetCompositionStringW ImmGetContext ImmNotifyIME \
         ImmReleaseContext ImmSetCandidateWindow ImmSetCompositionFontA \
         ImmSetCompositionFontW ImmSetCompositionStringW ImmSetCompositionWindow; do
    il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN $f -> " \
        "imm32!$f reported its documented refusal"
done

# --- KERNEL32 / SENSAPI / USER32 / WINTRUST --------------------------------
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN ReadDirectoryChangesW -> FALSE \\(as documented: ERROR_NOT_SUPPORTED" \
    "ReadDirectoryChangesW refuses with ERROR_NOT_SUPPORTED"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN IsNetworkAlive -> (TRUE|FALSE) \\(probe:" \
    "IsNetworkAlive reports a probe result rather than a lie"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN IsDestinationReachableW -> (TRUE|FALSE) \\(probe:" \
    "IsDestinationReachableW reports a probe result"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN IsDestinationReachableW\\(empty\\) -> FALSE" \
    "an empty destination is refused (the probe's refusal half)"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN GetComboBoxInfo -> FALSE \\(as documented: ERROR_INVALID_HANDLE" \
    "GetComboBoxInfo refuses a non-combobox handle"
il_assert_grep "$LOG" "\\[w32\\] FAIL-CLEAN WinVerifyTrust -> 0x800B0100 \\(as documented: TRUST_E_NOSIGNATURE" \
    "WinVerifyTrust makes no trust decision (TRUST_E_NOSIGNATURE)"

# --- no fault --------------------------------------------------------------
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|PANIC" \
    "no fault while calling any of the 35"

il_summary
