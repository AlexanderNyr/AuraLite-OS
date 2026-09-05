#!/usr/bin/env bash
# test_deadkeys.sh — RESIDUE2 T4 gate: dead-key (accent) composition.
#
# One boot.  The serial queue drives `kbd de` and the phase markers; a
# background feeder polls the integration log and injects PS/2 scan codes
# through the QEMU HMP monitor (`sendkey`), exactly like test_keymaps.sh —
# serial input never touches the PS/2 keyboard path where the dead-key
# state machine lives.
#
# What each probe proves (German layout; the old tree's behaviour in
# parentheses):
#   ´ + t    no CP437 ´t exists -> spacing ' + t -> the shell line "'t"
#            -> "sh: unmatched quote".  (Old: ´ emitted NOTHING unshifted,
#            so the line was plain "t" -> "t: command not found" — the
#            quote error is the unambiguous witness that the accent armed
#            and its spacing fallback fired.)
#   ´ + e    composes é (CP437 0x82).  The legacy fd-0 line reader drops
#            bytes > 0x7E, so the command line is EMPTY: no "e: command
#            not found" may appear.  (Old: "e: command not found".)
#   ^ + o    composes ô (CP437 0x93) — likewise dropped, no "^o: ..."
#            line.  (Old: "^o: command not found".)
#   ^ + spc  the standard accent+space rule resolves to the spacing form
#            '^' — pure ASCII, so it MUST reach the shell:
#            "^: command not found".  (Old: same bytes via the direct
#            layer — this pins that the dead path did not lose them.)
# The composed bytes themselves (>0x7E) are pinned byte-exactly by the
# host-side tests/unit/test_deadkey.c against the same compose tables the
# kernel links; what only QEMU can prove — arm, consume, cancel, and the
# fd-0 drop — is what this case asserts.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "dead keys (RESIDUE2 T4)"

LOG="$IL_LOGDIR/deadkeys.log"
MON="$IL_LOGDIR/deadkeys.mon.sock"
MONLOG="$IL_LOGDIR/deadkeys.monitor.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
rm -f "$MON" "$MONLOG"

# ---- HMP sendkey helper: one short-lived monitor connection per batch ----
# The sc-0x29 key (^) is named "grave" up to QEMU 9 and "grave_accent" on
# QEMU 10+.  The special token CIRCUMFLEX is resolved by asking the
# monitor its VERSION (no keystroke injected — a sendkey probe would arm
# the very dead key under test), then every key fires blind with the
# original pacing, so the case runs on either QEMU generation.
kbd_sendkeys() {   # $1=sock $2..=QKeyCode names/combos (CIRCUMFLEX = auto)
    local sock="$1"; shift
    python3 - "$sock" "$@" >>"$MONLOG" 2>&1 <<'PYEOF'
import re, socket, sys, time
sock, keys = sys.argv[1], sys.argv[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.time() + 30
while True:
    try:
        s.connect(sock)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.time() > deadline:
            raise
        time.sleep(0.5)
s.settimeout(5)
try:
    s.recv(4096)                      # HMP greeting + (qemu) prompt
except socket.timeout:
    pass

# Resolve CIRCUMFLEX from the monitor's own version report.
if "CIRCUMFLEX" in keys:
    s.sendall(b"info version\n")
    time.sleep(0.4)
    s.settimeout(1.0)
    try:
        reply = s.recv(65536).decode("utf-8", "replace")
    except socket.timeout:
        reply = ""
    s.settimeout(5)
    m = re.search(r"version (\d+)", reply)
    major = int(m.group(1)) if m else 10
    name = "grave_accent" if major >= 10 else "grave"
    keys = [name if k == "CIRCUMFLEX" else k for k in keys]

for k in keys:
    s.sendall(("sendkey %s\n" % k).encode())
    time.sleep(0.6)                   # guest consumes press+release
time.sleep(0.3)
try:
    sys.stdout.write(s.recv(65536).decode("utf-8", "replace"))
except socket.timeout:
    pass
s.close()
PYEOF
}

feed_wait() {      # $1=fixed pattern; blocks (bounded) until it is in $LOG
    local pat="$1" i
    for i in $(seq 1 400); do
        [ -f "$LOG" ] && grep -qF -- "$pat" "$LOG" 2>/dev/null && return 0
        sleep 0.5
    done
    echo "FEEDER TIMEOUT waiting for: $pat" >> "$MONLOG"
    return 1
}

# ---- background feeder: fires key batches strictly after phase markers ----
# The marker carries the case's PID: a STALE log from a previous run can
# never satisfy feed_wait (il_run_qemu truncates the log only after the
# feeder has started, so an unqualified marker would release the batches
# against the previous run's text and type into a US-layout boot).
MARKER="ZZ-DEAD-DE-$$"
(
    feed_wait "$MARKER" || exit 1
    # ´ (sc 0x0D, HMP "equal") then t: spacing fallback "'t".
    kbd_sendkeys "$MON" equal t ret
    # ´ then e: composes é (0x82) — dropped by the ASCII fd-0 reader.
    kbd_sendkeys "$MON" equal e ret
    # ^ (sc 0x29, HMP "grave"/"grave_accent") then o: composes ô (0x93) — dropped.
    kbd_sendkeys "$MON" CIRCUMFLEX o ret
    # ^ then space: spacing form '^' is ASCII — must reach the shell.
    kbd_sendkeys "$MON" CIRCUMFLEX spc ret
    echo "MONITOR FEED DONE" >>"$MONLOG"
) &

# ---- serial script: layout switch + phase markers, generous gaps ---------
il_send_delay 7
il_send "echo deadkeys-boot-phase"
il_send "kbd de"
il_send_delay 2
il_send "echo $MARKER"
il_send_delay 40
il_send "kbd us"
il_send_delay 2
il_send "echo gate-end"
il_send "exit"

# HMP monitor on a unix socket; everything else stays lib.sh-standard.
il_run_qemu "$LOG" 130 \
    -monitor "unix:$MON,server,nowait"

# ---- assertions ------------------------------------------------------------
il_assert_grep_fixed "$LOG" "[kbd] layout set to 'de'" \
    "kbd de switched the kernel to the German layout"
il_assert_grep_fixed "$LOG" "sh: unmatched quote" \
        "´+t: the armed accent emitted its spacing form before t ('t line)"
il_assert_no_grep_fixed "$LOG" "t: command not found" \
    "´+t did NOT collapse to a bare t (the old behaviour)"
il_assert_no_grep_fixed "$LOG" "e: command not found" \
    "´+e composed to é (0x82), which the ASCII fd-0 reader drops"
il_assert_no_grep_fixed "$LOG" "o: command not found" \
    "^+o composed to ô (0x93), which the ASCII fd-0 reader drops"
il_assert_grep_fixed "$LOG" "^: command not found" \
    "^+space resolved to the ASCII spacing form '^' end to end"

il_assert_grep_fixed "$MONLOG" "MONITOR FEED DONE" "monitor feeder ran to completion"
il_assert_no_grep "$MONLOG" "Error|invalid|unknown|rejected" "HMP accepted every sendkey"

il_assert_grep_fixed "$LOG" "gate-end" "shell survives the gate"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
