#!/usr/bin/env bash
# test_tcp_wscale.sh — RESIDUE3 C7: TCP window scaling (RES-52).
#
# The stack could not scale its window: `TCP_WINDOW` was 64240, the window
# field was a constant, and `wscale`/`shift_cnt` had zero hits in tcp.c+tcp.h.
# A 16-bit window caps a connection at 65535 bytes in flight, whatever the
# link speed is.
#
# TWO LANES, because one of them cannot exist on SLIRP.
#
#   Case 1 (TAP, needs privileges): the guest talks to a REAL Linux peer over
#   a TAP link.  Linux negotiates a window scale, so the peer's 16-bit field
#   really means megabytes — and the measurement is the peak number of bytes
#   in flight: above 65535 is impossible with an unscaled window, so it can
#   only be scaling.  The same boot also runs the negative control (a peer
#   whose window is bounded to 16 KiB), where the identical megabyte transfer
#   must stay inside 16 bits.
#
#   Case 2 (SLIRP, no privileges): SLIRP terminates TCP itself and offers no
#   window scale at all, which makes it the perfect "peer that omits the
#   option" — the graceful path C7 must not confuse with a granted scale.
#
# THE 200 ms RTT THE PLAN ASKED FOR IS NOT HERE.  QEMU 10 has no netem and
# SLIRP has no shaper; a userspace peer cannot delay ACKs without also
# shrinking the window it grants (delaying reads does nothing until the
# receive buffer fills, and filling it is the opposite of granting a large
# window). "200 ms" and "window bigger than 64 KiB" cannot both hold through a
# userspace peer, so the lane measures bytes in flight and REPORTS the elapsed
# time instead. The substitution is written down in the plan, not hidden.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

BIG_PORT="${BIG_PORT:-18499}"
CLAMP_PORT="${CLAMP_PORT:-18500}"
TAP="alws$RANDOM"
LANE_RAN=0

cleanup() {
    kill "$PEER_PID" 2>/dev/null || true
    wait "$PEER_PID" 2>/dev/null || true
    if [ "${TAP_CREATED:-0}" = "1" ]; then
        sudo -n ip link del "$TAP" 2>/dev/null || true
    fi
    il_dump_on_error
}

# --- Case 1: the real peer over a TAP link --------------------------------
il_section "TCP window scaling (C7): a megabyte past the 16-bit ceiling"

if sudo -n true 2>/dev/null && sudo -n ip tuntap add dev "$TAP" mode tap 2>/dev/null; then
    TAP_CREATED=1
    sudo -n ip addr add 10.0.2.2/24 dev "$TAP" 2>/dev/null || true
    sudo -n ip link set "$TAP" up 2>/dev/null || true

    LOG="$IL_LOGDIR/tcp_wscale.log"
    PEERLOG="$IL_LOGDIR/tcp_wscale_peer.log"
    IL_LAST_LOG="$LOG"
    IL_LAST_PEERLOG="$PEERLOG"

    python3 lib/paced_peer.py "$BIG_PORT" "$CLAMP_PORT" >"$PEERLOG" 2>&1 &
    PEER_PID=$!
    trap cleanup EXIT
    sleep 1

    # $IL_QEMU directly, NOT il_run_qemu: the library always appends
    # `-netdev user,id=net0 -device $IL_NIC,netdev=net0`, and with two NICs
    # the SLIRP one wins registration — measured: the lane then ran over
    # SLIRP (which offers no window scale) while the TAP peer still received
    # its megabyte, i.e. exactly the kind of half-truth this suite exists to
    # avoid.
    set +e
    timeout --foreground 120 "$IL_QEMU" \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
        -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64 \
        -serial "file:$LOG" \
        -netdev "tap,ifname=$TAP,script=no,downscript=no,id=n0" \
        -device e1000,netdev=n0 \
        -fw_cfg "name=opt/auralite.tcp_ws,string=$BIG_PORT:$CLAMP_PORT" \
        >/dev/null 2>&1
    set -e
    LANE_RAN=1

    # 1. What we put on the wire.  Before C7 the SYN's option block was MSS +
    #    SACK-permitted only: 8 bytes, header 28.  A scale makes it 12/32.
    il_assert_grep_fixed "$LOG" "[tcp] SYN options: 12 bytes, hdr=32, mss=1460 sack-perm wscale=0" \
        "our SYN carries MSS + SACK-permitted + a window scale (32-byte header)"

    # 2. What the peer answered, and what we made of it.  `field 65535` with
    #    shift 8 is the same 16-bit field meaning 16 MiB — the arithmetic in
    #    one line.
    il_assert_grep "$LOG" "\\[tcp\\] wscale: peer shift=[1-9][0-9]* -> snd_wnd=[0-9]+ \\(field [0-9]+\\)" \
        "the peer offered a shift and its window was read through it"
    il_assert_grep "$LOG" "our shift 0 \\(64240-byte receive buffer\\)" \
        "our own shift is 0, derived from the receive buffer we actually hold"

    # 3. The measurement.  The number is compared AS A NUMBER: a grep for
    #    "PASS" would accept a lane that reported six bytes in flight.
    il_assert_grep "$LOG" "\\[tcp\\] wscale lane \\(big\\): ESTABLISHED, peer shift [1-9][0-9]*, snd_wnd [0-9]+" \
        "the megabyte lane negotiated a scale with its own peer"
    il_assert_grep "$LOG" "\\[tcp\\] wscale lane \\(big\\): sent 1048576 bytes in [0-9]+ ms, peak in-flight [0-9]+ bytes" \
        "a megabyte crossed the connection"

    PEAK="$(grep -oE 'wscale lane \(big\): sent 1048576 bytes in [0-9]+ ms, peak in-flight [0-9]+' "$LOG" \
            | tail -1 | grep -oE 'peak in-flight [0-9]+' | grep -oE '[0-9]+$')"
    IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
    if [ -n "${PEAK:-}" ] && [ "$PEAK" -gt 65535 ]; then
        il_pass "peak in-flight was $PEAK bytes — above the 65535 the 16-bit field can express"
    else
        il_fail "peak in-flight was '${PEAK:-none}' bytes — not above 65535"
    fi
    il_assert_grep "$LOG" "wscale lane \\(big\\): PASS [0-9]+ bytes were in flight at once" \
        "the lane itself agrees the ceiling was passed"

    # 4. The negative control: same payload, peer window bounded to 16 KiB.
    il_assert_grep "$LOG" "\\[tcp\\] wscale lane \\(clamped\\): ESTABLISHED, peer shift 0, snd_wnd [0-9]+" \
        "the control peer granted no scale"
    il_assert_grep "$LOG" "wscale lane \\(clamped\\): PASS peak in-flight [0-9]+ stayed inside the 65535-byte window" \
        "the same transfer stayed inside 16 bits when the peer allowed no more"
    il_assert_grep_fixed "$LOG" "[tcp] wscale lane: PASS 1 MiB over a scaled window, and a clamped peer held the same transfer inside 16 bits" \
        "both lanes agreed"

    # 5. The peer's own receipt — guest bytes are not evidence they arrived.
    il_assert_grep_fixed "$PEERLOG" "peer[big]: received 1048576 of 1048576 bytes (window free to grow)" \
        "the big peer read all 1 MiB"
    il_assert_grep_fixed "$PEERLOG" "peer[clamped]: received 1048576 of 1048576 bytes (window bounded to 16 KiB)" \
        "the clamped peer read all 1 MiB"

    il_assert_no_grep "$LOG" "PANIC|page fault in kernel" "no kernel fault"
    il_assert_no_grep "$LOG" "\[tcp\] wscale lane: FAIL" "no lane failure"
else
    IL_LAST_LOG="$IL_LOGDIR/tcp_wscale_skipped.log"
    : > "$IL_LAST_LOG"
    il_skip "cannot create a TAP link (needs sudo + /dev/net/tun): the scaled-window lane did not run"
    il_skip "and therefore NO claim is made about a window above 65535 bytes in this environment"
fi

# --- Case 2: a peer that omits the option entirely (SLIRP) ----------------
il_section "TCP window scaling (C7): a peer that never offers a scale"

SLOG="$IL_LOGDIR/tcp_wscale_slirp.log"
IL_LAST_LOG="$SLOG"
# SLIRP lane: the library's own invocation (one NIC, user-mode net).
il_run_qemu "$SLOG" 90 \
    -drive file="$IL_BUILD/auralite.iso",format=raw,if=ide,snapshot=on \
    -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64

il_assert_grep_fixed "$SLOG" "[tcp] SYN options: 12 bytes, hdr=32, mss=1460 sack-perm wscale=0" \
    "we offer a scale even to a peer that will not take it"
il_assert_grep "$SLOG" "\\[tcp\\] wscale: peer did not offer a window scale; snd_wnd=[0-9]+ stays 16-bit" \
    "a peer that omits the option is told apart from one that grants shift 0"
il_assert_grep "$SLOG" "ESTABLISHED" \
    "the handshake still completes — refusing to scale is not a failure"
il_assert_no_grep "$SLOG" "PANIC|page fault in kernel" "no kernel fault"

if [ "$LANE_RAN" = "1" ]; then
    il_assert_grep_fixed "$SLOG" "[tcp] wscale: peer did not offer a window scale" \
        "the SLIRP peer (which terminates TCP itself) is the omitting-peer case"
fi

il_summary
