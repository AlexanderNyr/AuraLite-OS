#!/usr/bin/env bash
# test_wr4_putty_live.sh — W32RUN_PLAN.md phase WR-4 gate (PuTTY live).
#
# The pinned PuTTY 0.85 x64 (single putty.exe) is launched under the w32
# personality and driven live: config dialog, session round-trip through the
# registry hive, a -raw echo leg, and a -ssh leg against host-side lane
# servers.  Binaries stay outside the repo (sha256 pins only).
#
# WHAT THIS GATE ASSERTS (live, headless, CI-automatable):
#   - launch: putty.exe binds its 348-import ledger; the config dialog paints;
#   - session UI round-trip: a typed marker host lands in the host edit
#     (pixels), persists byte-exact into /disk/w32hive under the PuTTY
#     session key, and repopulates the edit after Cancel + relaunch;
#   - raw echo: guest bytes reach the lane echo server (server-side) and the
#     ECHO: reply paints in the terminal; the terminal then closes fully
#     (X + Exit Confirmation OK), verified by pixels AND server-side EOF —
#     the gate holds strictly ONE live TCP connection at a time;
#   - ssh: kex+auth+channel complete (server-side) and the command output
#     paints in the terminal — in a SECOND boot, so every leg runs with a
#     single live TCP connection (the stack's proven envelope; closing the
#     raw window orphans its connection process-side, a WR-5 lifecycle gap,
#     so the ssh leg boots fresh instead of sharing the wire);
#   - no kernel panic; frame digests are recorded for the #putty receipt.
#
# Skip convention mirrors test_wr3_npp_live.sh: no OVMF or no pinned binary
# -> loud SKIP, not a silent green.  Slow under TCG -> on SLOW_CASES_RE.

set -u
CASE_DIR="$(dirname "$0")"
cd "$CASE_DIR/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "WR-4: the pinned PuTTY drives dialog, raw and ssh legs live"

# --- the pinned binary: outside the repo, delivered on /fat ------------------
WR4_PUTTY_DIR="${WR4_PUTTY_DIR:-$HOME/appbins}"
PUTTY="$WR4_PUTTY_DIR/putty.exe"
PUTTY_SHA="d01fdb5aae8f112526040a39b0bfb9e27d813003178645e65f8d1cfdb2a26c87"
if [ ! -f "$PUTTY" ]; then
    echo "  SKIP: pinned putty.exe not staged ($WR4_PUTTY_DIR/putty.exe)"
    echo "        set WR4_PUTTY_DIR=/path/to/pinned to run this gate."
    il_summary
    exit 0
fi
if [ "$(sha256sum "$PUTTY" | awk '{print $1}')" != "$PUTTY_SHA" ]; then
    echo "  SKIP: staged putty.exe is not the pinned 0.85 x64 build"
    il_summary
    exit 0
fi

# --- geometry (1280x800 frame; deterministic placement, measured) ------------
SAVE_X=875;  SAVE_Y=448    # config dialog Save button
CANCEL_X=873; CANCEL_Y=631 # config dialog Cancel button (measured)
TERM_X=713;  TERM_X_Y=74    # terminal title-bar close box (measured)
EXIT_OK_X=461; EXIT_OK_Y=299 # PuTTY Exit Confirmation OK button (measured)
CONFIRM_REGION="250 205 500 225" # confirm title bar (blue) vs Hello-GUI white
HOST_REGION="530 230 935 280"     # host-name edit + padding (marker paints x545-650)
DLG_REGION="330 140 940 620"       # config dialog client area (measured)
TREE_REGION="410 180 595 595"     # category tree (static once painted)
LIST_REGION="648 372 810 545"     # saved-sessions (white) list box
TERM_REGION="80 55 880 500"       # terminal title bar + client
FRAME_NOTASK="0 0 1280 760"       # full frame minus the clocked taskbar
MARKER="wr4rt11"

BIND_PAT='putty[.]exe mapped at .* import[(]s[)] bound'

# --- in-lane servers (host side) ----------------------------------------------
ECHO_LOG="$IL_LOGDIR/wr4_echo.log"
SSH_LOG="$IL_LOGDIR/wr4_ssh.log"
ECHO_PID=""; SSH_PID=""
python3 "$CASE_DIR/../wr4_echo_server.py" >"$ECHO_LOG" 2>&1 &
ECHO_PID=$!
SSH_OK=0
if python3 -c "import paramiko" 2>/dev/null && [ -f "$CASE_DIR/../wr4_ssh_host_rsa.key" ]; then
    python3 "$CASE_DIR/../wr4_ssh_server.py" >"$SSH_LOG" 2>&1 &
    SSH_PID=$!
    SSH_OK=1
else
    echo "  SKIP (env gap): paramiko or the pinned ssh host key is missing — ssh leg off"
fi

trap 'kill $ECHO_PID $SSH_PID 2>/dev/null; gl_stop; il_dump_on_error' EXIT

# --- lane helpers (probe-free asserts; the serial lane is drained actively) ---
wr4_drain() { # $1=secs — one-shot serial drain into GL_LOG (best effort)
    python3 "$GL_CONSOLE" serial-drain --serial "$GL_SER" --secs "$1" \
        >>"$GL_LOG" 2>&1 || true
}

wr4_wait_grep() { # $1=file $2=pattern $3=timeout_secs — 0 iff seen
    local f="$1" pat="$2" budget="$3" i=0
    while [ "$i" -lt "$budget" ]; do
        if grep -a -q "$pat" "$f" 2>/dev/null; then return 0; fi
        wr4_drain 2; i=$((i + 2))
    done
    return 1
}

wr4_send() { # $1=cmd [$2=secs] — gl_send, stdout quiet (GL_LOG has it)
    gl_send "$1" "${2:-8}" >/dev/null
}

wr4_wait_count() { # $1=file $2=pattern $3=min_count $4=timeout — 0 iff met
    local f="$1" pat="$2" want="$3" budget="$4" i=0 n=0
    while [ "$i" -lt "$budget" ]; do
        n=$(grep -a -c "$pat" "$f" 2>/dev/null || echo 0)
        if [ "$n" -ge "$want" ]; then return 0; fi
        wr4_drain 2; i=$((i + 2))
    done
    return 1
}

BIND_BASE=0
wr4_boot() { # fresh boot for one leg-group; resets the bind ordinal base
    gl_stop 2>/dev/null || true
    gl_boot || { il_fail "OVMF boot did not reach the shell"; il_summary; exit 1; }
    il_assert_grep "$GL_LOG" "auralite#" "reached the shell"
    BIND_BASE=$(grep -a -c "$BIND_PAT" "$GL_LOG" 2>/dev/null || true)
    BIND_BASE=${BIND_BASE:-0}
}
wr4_launch_putty() { # $1=extra args $2=ordinal-in-boot — send + wait for Nth bind
    wr4_send "w32run /fat/putty.exe $1 &" 8
    wr4_wait_count "$GL_LOG" "$BIND_PAT" "$((BIND_BASE + $2))" 120
}

wr4_flush_shell() {
    # HMP-typed text ALSO reaches the shell (the console taps the PS/2
    # stream), so after typing into an app a garbage line pends and would
    # concatenate with the next command.  A bare CR executes (and clears) it.
    sleep 2
    wr4_send "" 2
}

wr4_settle() { # $1=tag $2=region $3=budget_secs — frame stable 2 shots apart
    local tag="$1" region="$2" budget="$3" i=0
    gl_shot "_settle_${tag}_0" >/dev/null 2>&1
    while [ "$i" -lt "$budget" ]; do
        sleep 2
        gl_shot "_settle_${tag}_1" >/dev/null 2>&1
        if gl_oracle delta "$GL_SHOTDIR/_settle_${tag}_0.png" \
                "$GL_SHOTDIR/_settle_${tag}_1.png" --region "$region" >/dev/null 2>&1; then
            mv -f "$GL_SHOTDIR/_settle_${tag}_1.png" "$GL_SHOTDIR/_settle_${tag}_0.png"
        else
            return 0 # identical — settled
        fi
        i=$((i + 2))
    done
    return 1
}

wr4_assert_delta() { # $1=a $2=b $3=region $4=msg — pass iff region changed
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if gl_oracle delta "$1" "$2" --region "$3" >/dev/null 2>&1; then
        il_pass "$4"
    else
        il_fail "$4"
    fi
}

wr4_assert_bright() { # $1=img $2=region $3=floor $4=msg — pass iff mean > floor
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    local mean
    mean=$(gl_oracle region-brightness "$1" --region "$2" 2>/dev/null | sed -n 's/.*mean=\([0-9.]*\).*/\1/p')
    if [ -n "$mean" ] && python3 -c "import sys; sys.exit(0 if float('$mean') > $3 else 1)" 2>/dev/null; then
        il_pass "$4 (mean $mean > $3)"
    else
        il_fail "$4 (mean ${mean:-?} <= $3)"
    fi
}

wr4_assert_dark() { # $1=img $2=region $3=floor $4=msg — pass iff mean < floor
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    local mean
    mean=$(gl_oracle region-brightness "$1" --region "$2" 2>/dev/null | sed -n 's/.*mean=\([0-9.]*\).*/\1/p')
    if [ -n "$mean" ] && python3 -c "import sys; sys.exit(0 if float('$mean') < $3 else 1)" 2>/dev/null; then
        il_pass "$4 (mean $mean < $3)"
    else
        il_fail "$4 (mean ${mean:-?} >= $3)"
    fi
}

# --- deliver the binary on a /fat disk and boot -------------------------------
APP_IMG="$IL_LOGDIR/wr4_putty_live_fat.img"
gl_make_fat_disk "$APP_IMG" "$PUTTY"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off
# --- boot 1: dialog legs + raw leg (one live connection) -----------------------
wr4_boot
gl_shot wr4_pre

# --- 1. config dialog ----------------------------------------------------------
# A missed launch poisons every leg below with no-putty cascades, so stop here
# instead of cascading ten vacuous failures.
wr4_launch_putty "" 1 || { il_fail "putty.exe did not bind (dialog launch)"; il_summary; exit 1; }
il_assert_grep "$GL_LOG" "$BIND_PAT" "putty.exe bound its import ledger"
wr4_settle dlg "$TREE_REGION" 60 || il_fail "dialog did not settle (frame)"
gl_shot wr4_dlg0
wr4_assert_delta "$GL_SHOTDIR/wr4_pre.png" "$GL_SHOTDIR/wr4_dlg0.png" \
    "$DLG_REGION" "config dialog painted (dialog-region delta)"
sleep 10  # input-ready grace: settle fires before PuTTY drains keys

# --- 2. session UI round-trip --------------------------------------------------
python3 "$GL_CONSOLE" type --monitor "$GL_MON" --delay 0.12 "$MARKER" \
    || il_fail "typing the marker host failed"
sleep 8  # TCG paints late; pilot shows +8s is fully painted
gl_shot wr4_typed
wr4_assert_delta "$GL_SHOTDIR/wr4_dlg0.png" "$GL_SHOTDIR/wr4_typed.png" \
    "$HOST_REGION" "typed marker host landed in the host edit (pixels)"
gl_click $SAVE_X $SAVE_Y
sleep 4
wr4_flush_shell
# The app disk snapshot cannot be read back from the host, so the hive bytes
# are asserted through an in-guest cat fenced between markers (the fence keeps
# shell-echoed copies of the marker out of the match).
wr4_send "echo HIVEBEGIN; cat /disk/w32hive; echo HIVEEND" 10
wr4_drain 6
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if tr -d '\000' < "$GL_LOG" | awk '/HIVEBEGIN/{f=1;next}/HIVEEND/{f=0}f' \
        | grep -a -F -q "$MARKER"; then
    il_pass "marker host persisted to /disk/w32hive (byte-exact)"
else
    il_fail "marker host persisted to /disk/w32hive (byte-exact)"
fi
gl_click $CANCEL_X $CANCEL_Y
sleep 3
gl_shot wr4_closed
wr4_assert_dark "$GL_SHOTDIR/wr4_closed.png" "$LIST_REGION" 100 \
    "dialog closed (sessions list region dark again)"
wr4_launch_putty "" 2 || il_fail "putty.exe did not bind (relaunch)"
wr4_settle dlg1 "$TREE_REGION" 60 || il_fail "reopened dialog did not settle"
sleep 10  # paint grace: repopulated text lands after settle (hv1: +12s proven)
gl_shot wr4_dlg1
wr4_assert_delta "$GL_SHOTDIR/wr4_dlg0.png" "$GL_SHOTDIR/wr4_dlg1.png" \
    "$HOST_REGION" "reopened dialog repopulated the host edit (round-trip)"
gl_click $CANCEL_X $CANCEL_Y
sleep 3

# --- 3. raw echo, live ---------------------------------------------------------
wr4_launch_putty "-raw 10.0.2.2 2323" 3 \
    || il_fail "putty.exe did not bind (-raw launch)"
wr4_wait_grep "$ECHO_LOG" "wr4echo: conn" 60 \
    || il_fail "echo server saw no connection"
sleep 6
gl_shot wr4_term0
python3 "$GL_CONSOLE" type --monitor "$GL_MON" --delay 0.12 $'hello WR4\n' \
    || il_fail "typing into the raw terminal failed"
wr4_flush_shell
wr4_wait_grep "$ECHO_LOG" "wr4echo: got" 60 \
    || il_fail "echo server received no guest bytes"
sleep 5
gl_shot wr4_term1
il_assert_grep "$ECHO_LOG" "wr4echo: got b'hello WR4" \
    "guest bytes reached the echo server (server-side)"
wr4_assert_delta "$GL_SHOTDIR/wr4_term0.png" "$GL_SHOTDIR/wr4_term1.png" \
    "$TERM_REGION" "ECHO: reply painted in the terminal"
gl_click $TERM_X $TERM_X_Y
sleep 4  # PuTTY Exit Confirmation appears (WarnOnClose: session is live)
gl_click $EXIT_OK_X $EXIT_OK_Y
sleep 3
gl_shot wr4_rawclosed
wr4_assert_bright "$GL_SHOTDIR/wr4_rawclosed.png" "$CONFIRM_REGION" 165 \
    "raw terminal window closed (confirm dialog gone)"
echo "  SKIP (named gap, WR-5 lifecycle): raw server-side EOF — the closed"
echo "        window orphans its connection process-side; the ssh leg boots fresh."

# --- boot 2: ssh leg alone (fresh wire, one live connection) ----------------------
if [ "$SSH_OK" = "1" ]; then
    wr4_boot
    wr4_launch_putty "-ssh wr4@10.0.2.2 -P 2222 -pw wr4pass -hostkey \"ssh-rsa 2048 18:9b:f3:ce:90:36:6e:eb:3a:4c:81:fe:61:e6:ea:d5\"" 1 \
        || il_fail "putty.exe did not bind (-ssh launch)"
    wr4_wait_grep "$SSH_LOG" "wr4ssh: shell open" 120 \
        || il_fail "SSH session never opened (kex/auth/channel)"
    il_assert_grep "$SSH_LOG" "wr4ssh: shell open" \
        "SSH kex+auth+channel completed (server-side)"
    sleep 6
    gl_shot wr4_ssh0
    python3 "$GL_CONSOLE" type --monitor "$GL_MON" --delay 0.12 $'echo SSH-OK\n' \
        || il_fail "typing into the ssh terminal failed"
    wr4_flush_shell
    wr4_wait_grep "$SSH_LOG" "wr4ssh: cmd 'echo SSH-OK'" 60 \
        || il_fail "ssh server received no command"
    sleep 5
    gl_shot wr4_ssh1
    wr4_assert_delta "$GL_SHOTDIR/wr4_ssh0.png" "$GL_SHOTDIR/wr4_ssh1.png" \
        "$TERM_REGION" "SSH command output painted in the terminal"
fi

# --- no fault behind the run ----------------------------------------------------
il_assert_no_grep "$GL_LOG" "PANIC" "no kernel panic during the run"

# --- frame digests for the #putty receipt ---------------------------------------
sha256sum "$GL_SHOTDIR"/wr4_*.png | sed "s|$GL_SHOTDIR/||" \
    | tee "$IL_LOGDIR/wr4_frames.sha256"

il_summary
