#!/usr/bin/env bash
# test_keymaps.sh — FIX_R8 gate: selectable keyboard layouts.
#
# One boot.  The serial queue drives `kbd`; a background feeder polls the
# integration log for phase markers and injects PS/2 scan codes through the
# QEMU HMP monitor (`sendkey`), because serial input bypasses the PS/2
# keyboard entirely — the plan's gate ("switching layouts changes what the
# shell receives") can only be observed through real key events.
#
# Asserted, in order of appearance:
#   us phase (default):   Shift+2      -> '@'  -> "@: command not found"
#   de phase (`kbd de`):  Shift+2      -> '"'  -> "sh: unmatched quote"
#   (the init shell's quote parser rejects a bare '"' command line, which
#   is an equally unambiguous witness that the '"' byte reached it)
#                         key y        -> 'z'  (the y/z swap)
#                         AltGr+8      -> '['  (the AltGr third layer, PS/2)
#                         AltGr+q      -> '@'  (second '@' line in the log)
# Each probe ends with Enter so the shell itself reports the received byte —
# "<char>: command not found" is an unambiguous, greppable witness that the
# decoded character travelled key -> IRQ1 -> keymap_lookup -> kbd buffer ->
# fd-0 read -> shell command line, all inside the guest.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "selectable keyboard layouts (FIX_R8)"

LOG="$IL_LOGDIR/keymaps.log"
MON="$IL_LOGDIR/keymaps.mon.sock"
MONLOG="$IL_LOGDIR/keymaps.monitor.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
rm -f "$MON" "$MONLOG"

# ---- HMP sendkey helper: one short-lived monitor connection per batch ----
kbd_sendkeys() {   # $1=sock $2..=QKeyCode names/combos
    local sock="$1"; shift
    python3 - "$sock" "$@" >>"$MONLOG" 2>&1 <<'PYEOF'
import socket, sys, time
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
    echo "FEEDER TIMEOUT waiting for: $pat" >>"$MONLOG"
    return 1
}

# ---- background feeder: fires key batches strictly after phase markers ----
(
    feed_wait "ZZ-US-PHASE" || exit 1
    kbd_sendkeys "$MON" shift-2 ret

    feed_wait "ZZ-DE-PHASE" || exit 1
    kbd_sendkeys "$MON" shift-2 ret y ret
    kbd_sendkeys "$MON" alt_r-8 ret alt_r-q ret
    echo "MONITOR FEED DONE" >>"$MONLOG"
) &

# ---- serial script: layout query, switch, phase markers, generous gaps ----
# The il_send_delay gaps are what keep the two input sources (serial queue
# and monitor-injected keys) from racing each other into the same stdin ring.
il_send_delay 7
il_send "kbd"
il_send "echo ZZ-US-PHASE"
il_send_delay 14
il_send "kbd de"
il_send_delay 2
il_send "echo ZZ-DE-PHASE"
il_send_delay 40
il_send "echo gate-end"

# HMP monitor on a unix socket; everything else stays lib.sh-standard.
il_run_qemu "$LOG" 130 \
    -monitor "unix:$MON,server,nowait"

# ---- assertions ------------------------------------------------------------
il_assert_grep_fixed "$LOG" "[kbd] default layout 'us'" \
    "boot reports the compiled-in default layout"
il_assert_grep_fixed "$LOG" "kbd: current layout 'us'" \
    "kbd with no argument reports the active layout"
il_assert_grep_fixed "$LOG" "kbd: available: us de" \
    "kbd enumerates exactly the shipped layouts"
il_assert_grep_fixed "$LOG" "kbd: layout set to 'de'" \
    "kbd de switches at runtime (shell side)"
il_assert_grep_fixed "$LOG" "[kbd] layout set to 'de'" \
    "kbd de switches at runtime (kernel side)"

il_assert_grep_fixed "$LOG" "@: command not found" \
    "US phase: PS/2 Shift+2 produced '@'"
il_assert_grep_fixed "$LOG" "sh: unmatched quote" \
    "DE phase: the same PS/2 Shift+2 now produces '\"' — the shell's quote parser rejects the bare quote"
il_assert_grep_fixed "$LOG" "z: command not found" \
    "DE phase: the y key produces 'z' (QWERTZ y/z swap)"
il_assert_grep_fixed "$LOG" "[: command not found" \
    "DE phase: PS/2 AltGr+8 produces '[' (third layer works end to end)"
il_assert_count "$LOG" "@: command not found" 2 \
    "US Shift+2 and DE AltGr+q both reached the shell as '@'"

il_assert_grep_fixed "$MONLOG" "MONITOR FEED DONE" "monitor feeder ran to completion"
il_assert_no_grep "$MONLOG" "Error|invalid|unknown" "HMP accepted every sendkey"

il_assert_grep_fixed "$LOG" "gate-end" "shell survives the gate"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
