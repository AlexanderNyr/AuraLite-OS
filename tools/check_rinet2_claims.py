#!/usr/bin/env python3
"""Cross-check REALINTERNET2_PLAN.md against the tree.

Tenth checker of the D8 family.  The Y0 speciality: the plan's opener
facts are PINNED here as live greps — when a phase moves one (Y2 takes
inline-IPv4 7→0, Y3 lands AAAA/AF_INET6, Y6 adds the hybrid group),
the pin moves in the same commit or CI is red.  The Y0 rig also pinned
a plan CORRECTION: the plan's §1/§4 were written before reading the M6
layer (the A3 precedent — "found it understated"), and Y0's result
names what M6 already carried.

Usage:
    tools/check_rinet2_claims.py [--selftest]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PHASE_ORDER = ["Y0", "Y1", "Y2", "Y3", "Y4", "Y5", "Y6", "Y7"]


def read(*parts):
    try:
        with open(os.path.join(ROOT, *parts), "r", encoding="utf-8",
                  errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def claims():
    plan = read("REALINTERNET2_PLAN.md")
    tcp = read("kernel", "net", "tcp.c")
    dns = read("kernel", "net", "dns.c")
    tls = read("lib", "libatls", "src", "atls_tls.c")
    checks = []

    # --- the opener pins (moved by later phases, in the same commit) --
    y1_done = bool(re.search(r"^### Y1[^\n]*✅ COMPLETE", plan, re.M))
    y2_done = bool(re.search(r"^### Y2[^\n]*✅ COMPLETE", plan, re.M))
    y3_done = bool(re.search(r"^### Y3[^\n]*✅ COMPLETE", plan, re.M))
    y6_done = bool(re.search(r"^### Y6[^\n]*✅ COMPLETE", plan, re.M))

    if not y1_done:
        checks.append((
            "opener: cwnd initialises wide open (TCP_WINDOW) and the "
            "'until N7' comment stands — Y1 moves this pin",
            tcp.count("cwnd    = TCP_WINDOW") == 2 and
            "until N7" in tcp))
    if not y2_done:
        checks.append((
            "opener: the transport still spells IPv4 inline — Y2 moves "
            "this pin to zero",
            tcp.count("ip4") + tcp.count("ipv4") >= 5))
    if not y3_done:
        checks.append((
            "opener: DNS has no AAAA path and libc has no AF_INET6 — "
            "Y3 moves both pins",
            "AAAA" not in dns and
            "sockaddr_in6" not in read("lib", "libc", "include",
                                       "sys", "socket.h")))
    if not y6_done:
        checks.append((
            "opener: the ClientHello negotiates X25519 and nothing "
            "else — Y6 moves this pin",
            "MLKEM" not in tls and "0x11EC" not in tls and
            "ATLS_GROUP_X25519" in tls))

    # --- Y0: the rig (when the plan says it landed) --------------------
    if re.search(r"^### Y0[^\n]*✅ COMPLETE", plan, re.M):
        perf_h = read("kernel", "lib", "perfstat.h")
        perf_c = read("kernel", "lib", "perfstat.c")
        checks.append((
            "Y0: the four transport counters exist end to end "
            "(enum + wire names + tcp.c increments)",
            "PERF_TCP_RETRANSMITS" in perf_h and
            "tcp_cwnd_limited_sends" in perf_c and
            "PERF_TCP_RETRANSMITS" in tcp and
            "PERF_TCP_FAST_RETRANSMITS" in tcp and
            "PERF_TCP_RTO_EVENTS" in tcp and
            "PERF_TCP_CWND_LIMITED_SENDS" in tcp))
        checks.append((
            "Y0: perfstat is a KERNEL32_SHARED row (tcp.c counts on "
            "i386 too — the D3 constraint is structural)",
            "kernel/lib/perfstat.c" in read("Makefile") and
            read("Makefile").count("kernel/lib/perfstat.c") >= 1 and
            "perfstat.c" in
            read("Makefile").split("KERNEL32_SHARED")[1][:600]))
        checks.append((
            "Y0: the plan correction is RECORDED (M6 already carried "
            "fast-retx/SACK/Nagle/TIME_WAIT; the no-SACK non-goal was "
            "moot on arrival)",
            "struck at Y0" in plan and "M6" in plan))
        checks.append((
            "Y0: the rng band rider is in (FULL bound widened with the "
            "arithmetic at the site, the CI run named)",
            "32659367782" in read("kernel", "rng.c") and
            "expected / 4" in read("kernel", "rng.c")))
        checks.append((
            "Y0: the existing host gates cover the decision cores "
            "(test_tcp_x5 + the M6 quartet run in test-unit)",
            "test_tcp_x5" in read("Makefile") and
            "test_tcp_m6" in read("Makefile")))

    # --- Y1: congestion control (when the plan says it landed) ---------
    if y1_done:
        cc_h = read("kernel", "net", "tcp_cc.h")
        checks.append((
            "Y1: the growth arithmetic is a pure-C core with a host "
            "gate, and tcp.c drives cwnd from it",
            "tcpcc_iw" in cc_h and "tcpcc_ack_grow" in cc_h and
            "tcpcc_iw(TCP_MSS)" in tcp and
            "tcpcc_ack_grow(" in tcp and
            "tcpcc_rto_cwnd(" in tcp and
            "test_tcp_cc" in read("Makefile")))
        checks.append((
            "Y1: the wide-open init and the '+= 1460' growth are gone "
            "(the 'until N7' placeholder is retired)",
            tcp.count("cwnd    = TCP_WINDOW") == 0 and
            "c->cwnd += 1460" not in tcp and
            "until N7 brings" not in tcp))
        checks.append((
            "Y1: one ssthresh formula for both loss signals (the RTO "
            "path uses the m6 helper too)",
            tcp.count("tcpm6_recovery_ssthresh") >= 2))
        checks.append((
            "Y1: the recovery-deflate has an EDGE (the latent "
            "every-ACK clamp is fixed and named)",
            "was_recovery && !c->dupack.in_recovery" in tcp and
            "LATENT" in tcp))
        checks.append((
            "Y1: the guest receipt is pinned — the x5 1 MiB lane "
            "demands tcp_cwnd_limited_sends > 0",
            "tcp_cwnd_limited_sends [1-9]"
            in read("tests", "integration", "cases", "test_tcp_x5.sh")))

    # --- structural: status header vs table -----------------------------
    done_rows = len(re.findall(r"^\| Y\d+ [^|]*\| ✅ complete", plan, re.M))
    done_heads = len(re.findall(r"^### Y\d+[^\n]*✅ COMPLETE", plan, re.M))
    checks.append(("plan: every complete row has a COMPLETE heading",
                   done_rows == done_heads and plan != ""))
    status_ok = False
    if re.search(r"^## Status: PLANNED", plan, re.M):
        status_ok = (done_rows == 0)
    elif re.search(r"^## Status: IN PROGRESS", plan, re.M):
        m = re.search(r"Y0(?:–(Y\d+))? complete", plan)
        if m:
            label = m.group(1) if m.group(1) else "Y0"
            claimed = (PHASE_ORDER.index(label) + 1
                       if label in PHASE_ORDER else 0)
            status_ok = claimed == done_rows
    elif re.search(r"^## Status: COMPLETE", plan, re.M):
        status_ok = (done_rows == len(PHASE_ORDER))
    checks.append(("plan: the Status header agrees with the table",
                   status_ok))
    return checks


def main():
    if "--selftest" in sys.argv:
        results = claims()
        if not all(ok for _, ok in results):
            print("check_rinet2_claims: SELFTEST inconclusive (tree "
                  "already red)", file=sys.stderr)
            return 1
        global ROOT
        real_root = ROOT
        ROOT = os.path.join(real_root, "build")
        doctored = claims()
        ROOT = real_root
        if all(ok for _, ok in doctored):
            print("check_rinet2_claims: SELFTEST FAIL -- passes against "
                  "an empty tree", file=sys.stderr)
            return 1
        print("check_rinet2_claims: selftest PASS (doctored tree "
              "detected)")
        return 0

    results = claims()
    bad = [name for name, ok in results if not ok]
    for name in bad:
        print(f"check_rinet2_claims: FAIL -- {name}", file=sys.stderr)
    if bad:
        print(f"check_rinet2_claims: {len(bad)} claim(s) disagree "
              "with the tree", file=sys.stderr)
        return 1
    print(f"check_rinet2_claims: OK -- {len(results)} claims verified "
          "against the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
