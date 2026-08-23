#!/usr/bin/env bash
# test_dns_tcp.sh — RESIDUE_PLAN R9, ledger RES-25: DNS-over-TCP
# fallback for truncated answers (RFC 1035 s4.2.2).
#
# Shape: the guest arms the NAMED one-shot test knob (dnstc →
# DNSCTL_FORCE_TC) so the UDP leg comes back TC=1 without a
# root-bound port-53 fixture; the TCP retry that follows is REAL
# wire — QEMU guestfwd carries guest 10.0.2.4:53/tcp to a host
# python fixture that answers with a ~700-byte message (an answer
# that could never have arrived over the 512-byte UDP floor).
#
# Receipts pinned: the TC line, the TCP fallback byte count (>512),
# and the A record resolving through the fallback path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "R9: DNS TCP fallback (truncated UDP -> real TCP wire)"

LOG="$IL_LOGDIR/dns_tcp.log"
IL_LAST_LOG="$LOG"

FIXPORT=5533
FIX_PID=""
cleanup() {
    [ -n "$FIX_PID" ] && kill "$FIX_PID" 2>/dev/null
    wait 2>/dev/null
}
trap 'cleanup; il_dump_on_error' EXIT

# ---- the host fixture: TCP-only DNS answering ANY A question with
# 1.2.3.4 plus TXT padding to push the message far past 512 bytes.
python3 - "$FIXPORT" > "$IL_LOGDIR/dns_fixture.log" 2>&1 <<'PYEOF' &
import socket, struct, sys
port = int(sys.argv[1])
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(4)
while True:
    c, _ = srv.accept()
    print("accepted", flush=True)
    try:
        pfx = c.recv(2)
        if len(pfx) < 2:
            c.close(); continue
        need = struct.unpack(">H", pfx)[0]
        q = b""
        while len(q) < need:
            chunk = c.recv(need - len(q))
            if not chunk: break
            q += chunk
        if len(q) < 12:
            c.close(); continue
        qid = q[0:2]
        question = q[12:]          # name + qtype + qclass, echoed back
        # header: QR|RD|RA, QD=1, AN=1+pad, NS=0, AR=0
        npad = 22
        hdr = qid + b"\x81\x80" + struct.pack(">HHHH", 1, 1 + npad, 0, 0)
        # A answer: ptr to qname at 0x0c, type A, class IN, ttl 60, 1.2.3.4
        ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([1, 2, 3, 4])
        # TXT padding records (same owner) to push the message > 512B
        txt = b"AuraLite-R9-TCP" 
        rr = b"\xc0\x0c" + struct.pack(">HHIH", 16, 1, 60, len(txt) + 1) + bytes([len(txt)]) + txt
        msg = hdr + question + ans + rr * npad
        c.sendall(struct.pack(">H", len(msg)) + msg)
        print("answered", len(msg), "bytes", flush=True)
    finally:
        c.close()
PYEOF
FIX_PID=$!
sleep 1
if ! kill -0 "$FIX_PID" 2>/dev/null; then
    il_fail "dns fixture did not start"; il_summary; exit 1
fi

# guest 10.0.2.4:53/tcp -> host fixture (guestfwd is TCP-only, which
# is exactly the point: the UDP leg is the knob's synthetic TC).
export IL_NETDEV_OPTS=",guestfwd=tcp:10.0.2.4:53-tcp:127.0.0.1:$FIXPORT"

il_send_delay 8
il_send "dnsset 10.0.2.4"
il_send_delay 1
il_send "dnstc"
il_send_delay 1
il_send "nslookup biganswer.aura"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 60
unset IL_NETDEV_OPTS

il_assert_grep "$LOG" "auralite#"                       "shell reached"
il_assert_grep_fixed "$LOG" "[dns] FORCE_TC: synthesising a truncated UDP answer" \
    "the named knob armed the TC leg"
il_assert_grep_fixed "$LOG" "reply truncated (TC) — retrying over TCP" \
    "the resolver took the RFC 1035 s4.2.2 branch"
il_assert_grep "$LOG" "\[dns\] PASS: TCP fallback answer, [5-9][0-9][0-9] bytes" \
    "the TCP answer is >512 bytes (impossible over the UDP floor)"
il_assert_grep_fixed "$LOG" "-> 1.2.3.4" \
    "the A record resolved through the fallback"
il_assert_no_grep "$LOG" "TCP fallback carried no answer" "fallback did not fail"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"

il_summary
