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

    # --- Y2: the TCP/IP seam (when the plan says it landed) ------------
    if y2_done:
        netl3 = read("kernel", "net", "netl3.h")
        makefile = read("Makefile")
        k32 = makefile.split("KERNEL32_SHARED")[1][:800] if \
            "KERNEL32_SHARED" in makefile else ""
        checks.append((
            "Y2: netl3.h is the seam (ops + family+16-byte key + v4 build)",
            "struct netl3_ops" in netl3 and
            "netl3_addr_t" in netl3 and
            "netl3_v4_build" in netl3 and
            "NETL3_AF_INET6" in netl3))
        checks.append((
            "Y2: tcp.c has zero inline IPv4 spellings (the opener pin "
            "moved 7→0)",
            tcp.count("ip4") + tcp.count("ipv4") == 0))
        checks.append((
            "Y2: tcp.c drives the seam (ops_for / resolve / output / input) "
            "and the connection key is family + 16 bytes",
            "netl3_ops_for" in tcp and
            "l3->resolve" in tcp and
            "l3->output" in tcp and
            "netl3_input" in tcp and
            "netl3_addr_t peer" in tcp))
        checks.append((
            "Y2: the host A/B gate is registered and the phase patch exists",
            "test_netl3" in makefile and
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y2_seam.patch"))))
        checks.append((
            "Y2: netl3.c is a KERNEL32_SHARED row (D3: i386 compiles "
            "against the seam unchanged)",
            "kernel/net/netl3.c" in k32))
        if not y3_done:
            checks.append((
                "Y2: libc still has no sockaddr_in6 (Y3's pin is untouched)",
                "sockaddr_in6" not in read("lib", "libc", "include",
                                           "sys", "socket.h")))

    # --- Y3: TCP-over-IPv6 + AF_INET6 + AAAA ---------------------------
    if y3_done:
        makefile = read("Makefile")
        checks.append((
            "Y3: netl3_v6_ops exist and ops_for dispatches INET6",
            "netl3_v6_ops" in read("kernel", "net", "netl3.c") and
            "NETL3_AF_INET6" in read("kernel", "net", "netl3.c")))
        checks.append((
            "Y3: tcp_open_addr + tcp6_self_test are the transport entry",
            "tcp_open_addr" in tcp and "tcp6_self_test" in tcp and
            "[tcp6] PASS" in tcp))
        checks.append((
            "Y3: DNS has an AAAA path and libc has sockaddr_in6",
            "AAAA" in dns and
            "sockaddr_in6" in read("lib", "libc", "include",
                                   "netinet", "in.h") and
            "sockaddr_in6" in read("lib", "libc", "include",
                                   "sys", "socket.h")))
        checks.append((
            "Y3: dualstack pick is a pure-C core with a host gate",
            "dualstack_pick" in read("kernel", "net", "dualstack.h") and
            "test_dualstack" in makefile and
            "test_dns_aaaa" in makefile))
        checks.append((
            "Y3: the guest receipt is registered (test_tcp6 greps "
            "[tcp6] PASS)",
            "[tcp6] PASS" in read("tests", "integration", "cases",
                                  "test_tcp6.sh") and
            "test_tcp6" in read("tests", "integration", "run_all.sh")))
        checks.append((
            "Y3: the phase patch exists",
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y3_tcp6.patch"))))

    # --- Y4: HTTPS-over-IPv6 (RES-26) -----------------------------------
    if re.search(r"^### Y4[^\n]*✅ COMPLETE", plan, re.M):
        ahttp = read("lib", "libahttp", "src", "ahttp.c")
        ledger = read("docs", "residue_ledger.md")
        checks.append((
            "Y4: libahttp dials v6 (parse_ip6 + connectaddr/AAAA) and "
            "falls back to v4",
            "parse_ip6" in ahttp and "dial v6" in ahttp and
            "falling back to v4" in ahttp and
            "dns_resolve_aaaa" in ahttp))
        checks.append((
            "Y4: the guest receipt is registered (test_https6 greps "
            "[https6] PASS)",
            "[https6] PASS" in read("tests", "integration", "cases",
                                    "test_https6.sh") and
            "test_https6" in read("tests", "integration", "run_all.sh")))
        checks.append((
            "Y4: RES-26 flipped to DONE@Y4 and the five R-series "
            "OPEN rows remain named",
            "| RES-26 | W | DONE@Y4 |" in ledger and
            "| RES-02 | W | OPEN |" in ledger and
            "| RES-06 | W | OPEN |" in ledger and
            "| RES-07 | W | OPEN |" in ledger and
            "| RES-16 | W | OPEN |" in ledger and
            "| RES-18 | W | OPEN |" in ledger))
        checks.append((
            "Y4: the phase patch exists",
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y4_https6.patch"))))

    # --- Y5: ML-KEM-768 (FIPS 203) --------------------------------------
    if re.search(r"^### Y5[^\n]*✅ COMPLETE", plan, re.M):
        mlkem = read("lib", "libatls", "src", "atls_mlkem.c")
        sha3 = read("lib", "libatls", "src", "atls_sha3.c")
        testh = read("tests", "unit", "test_atls_hash.c")
        testm = read("tests", "unit", "test_atls_mlkem.c")
        makefile = read("Makefile")
        checks.append((
            "Y5: atls_mlkem.c is 768-only K-PKE + FO with NTT/invNTT",
            "ntt(" in mlkem and "invntt(" in mlkem and
            "sample_cbd2" in mlkem and "kbar" in mlkem and
            "ATLS_MLKEM768" in read("lib", "libatls", "include",
                                    "atls", "mlkem.h")))
        checks.append((
            "Y5: atls_sha3.c is Keccak-f[1600] + SHA3/SHAKE",
            "keccakf" in sha3 and "atls_sha3_256" in sha3 and
            "atls_shake128" in sha3 and "atls_shake256" in sha3))
        checks.append((
            "Y5: the host KAT + FO gate is registered",
            "test_atls_mlkem" in makefile and
            "ACVP" in testm and "J(z || ct)" in testm))
        checks.append((
            "Y5: D7's grep grew the new files and scans whole-file",
            "atls_sha3.c" in testh and "atls_mlkem.c" in testh and
            "Whole-file scan" in testh))
        checks.append((
            "Y5: the four-width scripts execute test_atls_mlkem",
            "atls_mlkem.c" in read("tests", "unit",
                                   "test_libatls_m32.sh") and
            "test_atls_mlkem" in read("tests", "unit",
                                      "test_libatls_m32.sh") and
            "test_atls_mlkem" in read("tests", "unit",
                                      "test_libatls_rv64.sh") and
            "test_atls_mlkem" in read("tests", "unit",
                                      "test_libatls_a64.sh")))
        checks.append((
            "Y5: the phase patch exists",
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y5_mlkem.patch"))))

    # --- Y6: X25519MLKEM768 hybrid handshake ----------------------------
    if y6_done:
        tls_h = read("lib", "libatls", "include", "atls", "tls.h")
        testh = read("tests", "unit", "test_atls_tls.c")
        guest = read("tests", "integration", "cases",
                     "test_x25519mlkem.sh")
        checks.append((
            "Y6: ClientHello names both groups and the hybrid share "
            "is ML-KEM ek ∥ X25519 (0x11EC)",
            "ATLS_GROUP_X25519MLKEM768" in tls and
            "0x11EC" in tls and
            "ATLS_MLKEM768_EK_BYTES + 32" in tls and
            "ATLS_GROUP_X25519" in tls))
        checks.append((
            "Y6: selected_group accepts either share; IKM is "
            "ML-KEM-ss ∥ X25519-ss",
            "atls_tls_derive_handshake_secrets_ikm" in tls and
            "atls_mlkem768_decaps" in tls and
            "ikmlen = 64" in tls))
        checks.append((
            "Y6: the public API surfaces the negotiated group",
            "ATLS_TLS_GROUP_X25519MLKEM768" in tls_h and
            "atls_tls_negotiated_group" in tls_h))
        checks.append((
            "Y6: host interop is pinned to both fixtures "
            "(X25519 D4 + X25519MLKEM768)",
            "start_s_server_groups" in testh and
            "X25519MLKEM768" in testh and
            "\"X25519\"" in testh))
        checks.append((
            "Y6: the guest receipt is registered "
            "([tls] PASS: X25519MLKEM768)",
            "[tls] PASS: X25519MLKEM768" in guest and
            "test_x25519mlkem" in read("tests", "integration",
                                       "run_all.sh")))
        checks.append((
            "Y6: the phase patch exists",
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y6_hybrid.patch"))))

    # --- Y7: close-out --------------------------------------------------
    if re.search(r"^### Y7[^\n]*✅ COMPLETE", plan, re.M):
        live = read("docs", "live_web.md")
        ahttp = read("lib", "libahttp", "src", "ahttp.c")
        ledger = read("docs", "residue_ledger.md")
        checks.append((
            "Y7: the live-web protocol is a paste-back (pending-user, "
            "not a CI dial of a public name)",
            "pending-user" in live and
            "run http https://www.ietf.org/" in live and
            "[tls] group=X25519MLKEM768" in live and
            "Not CI" in live))
        checks.append((
            "Y7: libahttp prints [tls] group= after the handshake",
            "[tls] group=X25519MLKEM768" in ahttp and
            "atls_tls_negotiated_group" in ahttp))
        checks.append((
            "Y7: leftover rows landed (live-web / happy-eyeballs / "
            "window scale / rsa_pss)",
            "| RES-49 | M | PENDING-USER@Y7 |" in ledger and
            "| RES-50 | N | RE-AFFIRMED@Y7 |" in ledger and
            "| RES-51 | N | RE-AFFIRMED@Y7 |" in ledger and
            "| RES-52 | S | HANDED-OFF@Y7 |" in ledger and
            "| RES-53 | W | OPEN |" in ledger))
        checks.append((
            "Y7: §5 quotes the Y0 opener greps against the close",
            "ip4" in plan and "**0**" in plan and
            "0x11EC" in plan and "32/32" in plan and
            "586" in plan))
        checks.append((
            "Y7: the Y4 IPv6 fixture is pinned to X25519 (D4)",
            "-groups X25519" in read("tests", "integration", "cases",
                                     "test_https6.sh")))
        checks.append((
            "Y7: the phase patch exists",
            os.path.exists(os.path.join(ROOT, "patches",
                                        "RINET2_Y7_close.patch"))))

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
