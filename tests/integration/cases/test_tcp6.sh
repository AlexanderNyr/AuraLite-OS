#!/usr/bin/env bash
# test_tcp6.sh — REALINTERNET2 Y3 guest gate.
#
# Kernel tcp6_self_test() connects to fec0::2:8036 during boot.
# fec0::2 is SLIRP's ipv6-host (the 10.0.2.2 analogue).  QEMU 10's
# guestfwd parser is IPv4-only — `tcp:[fec0::2]:8036-...` is rejected
# as "Invalid guest forwarding rule" — so the fixture is the tcp32
# pattern: a host listener on the SAME port, no guestfwd.  Receipt:
#   [tcp6] PASS: round-trip N byte(s)

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "TCP-over-IPv6 (fec0::2:8036 round-trip)"

LOG="$IL_LOGDIR/tcp6.log"
IL_LAST_LOG="$LOG"
# Same port the kernel probes.  SLIRP NATs vhost:8036 onto the host.
TCP6_PORT="${TCP6_PORT:-8036}"

python3 - "$TCP6_PORT" >"$IL_LOGDIR/tcp6_server.log" 2>&1 <<'PY' &
import socket, sys
port = int(sys.argv[1])
# Dual-stack: slirp may open AF_INET to 127.0.0.1 or AF_INET6 to ::1.
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
except OSError:
    pass
s.bind(("::", port))
s.listen(1)
s.settimeout(90)
try:
    c, _ = s.accept()
    data = b""
    while b"\n" not in data:
        chunk = c.recv(64)
        if not chunk:
            break
        data += chunk
    c.sendall(b"PONG-FROM-HOST\n")
    c.close()
except Exception as e:
    sys.stderr.write("tcp6_server: %s\n" % e)
finally:
    s.close()
PY
SRV_PID=$!

cleanup() {
    kill "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    il_dump_on_error
}
trap cleanup EXIT

# No IL_NETDEV_OPTS: guestfwd cannot name an IPv6 vhost on QEMU 10.

il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 50

il_assert_grep_fixed "$LOG" "[tcp6] probing fec0::2:8036..." \
    "Y3 self-test ran"
il_assert_grep "$LOG" "\\[tcp6\\] PASS: round-trip [1-9][0-9]* byte" \
    "TCP-over-IPv6 round-trip receipt"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no unhandled exception"

il_summary
