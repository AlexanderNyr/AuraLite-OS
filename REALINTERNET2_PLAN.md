# AuraLite OS — Real Internet II (the transport grows up, the handshake goes post-quantum)

## Status: COMPLETE — Y0–Y7 closed 2026-08-24

| Phase | Result | Deliverable |
|-------|--------|-------------|
| Y0 — the rig: TCP decision-core host gates, perf counters, opener receipts | ✅ complete | `patches/RINET2_Y0_rig.patch` |
| Y1 — congestion control: the cwnd stops being a constant | ✅ complete | `patches/RINET2_Y1_cc.patch` |
| Y2 — the TCP/IP seam: the transport stops spelling IPv4 inline | ✅ complete | `patches/RINET2_Y2_seam.patch` |
| Y3 — TCP-over-IPv6 + AF_INET6 + AAAA/dual-stack DNS | ✅ complete | `patches/RINET2_Y3_tcp6.patch` |
| Y4 — HTTPS-over-IPv6: the RES-26 receipt | ✅ complete | `patches/RINET2_Y4_https6.patch` |
| Y5 — ML-KEM-768 (FIPS 203) in libatls, KAT-gated | ✅ complete | `patches/RINET2_Y5_mlkem.patch` |
| Y6 — X25519MLKEM768: the hybrid handshake, interop-gated | ✅ complete | `patches/RINET2_Y6_hybrid.patch` |
| Y7 — close-out: the live-web fetch protocol, residue to the ledger | ✅ complete | `patches/RINET2_Y7_close.patch` |

## 1. Where this plan comes from (measured, not assumed)

REALINTERNET (X1–X9) ended with a working TLS 1.3 + HTTPS client and
one honest sentence about the real web: a fetch against Cloudflare
ends in `ATLS_ERR_PEER_EOF` because "the server expects a PQ-hybrid
key share (`X25519MLKEM768`); our ClientHello offers only X25519"
(REALINTERNET_PLAN.md:253-257 — the recorded follow-up this plan
picks up).  The RESIDUE series then closed the IPv6 SUBSTRATE
(R9: SLAAC, NDP both halves, `ping6 fec0::2` end-to-end in CI) and
left exactly one OPEN row pointing here: **RES-26** — "HTTPS-over-
IPv6 fetch receipt missing; the one blocker left is the TCP layer
itself, v4-wired through conn state + ARP + inline IPv4 headers".

The opener facts, measured on the tree this plan is committed to:

- **The transport is better than its own docs said** (the DOCS
  refresh corrected them): X5 already landed a sliding send window
  (min(cwnd, peer wnd)), RFC 6298-style adaptive RTO with Karn and
  exponential backoff, a PMTUD black-hole ladder, and single-gap
  out-of-order receive.  tcp.c is 1556 lines and — PARITY R3's
  receipt — compiles `-m32`-clean and runs on i386 behind netglue32.
- **But cwnd is a constant**: `conns[h].cwnd = TCP_WINDOW` at
  tcp.c:774/886, and the comment at :875 says it plainly — "cwnd is
  wide open until N7 brings real [congestion control]".  N7 never
  came; this plan is where it lands.
- **The transport spells IPv4 inline**: 7 direct ip4 references in
  tcp.c; the connection key, the pseudo-header checksum and the ARP
  resolve are all v4-shaped; the socket ABI has no `AF_INET6` and
  `sockaddr_in6` appears nowhere in libc (grep: zero hits).
- **DNS has no AAAA path** (grep dns.c: zero hits) — dual-stack
  cannot even learn a v6 address today.
- **The PQ gap is one group**: atls_tls.c negotiates exactly
  `ATLS_GROUP_X25519` (:687/:692 reject anything else).  The modern
  public web (Cloudflare et al.) prefers `X25519MLKEM768` (the
  IETF hybrid, code point 0x11EC).  ML-KEM-768 is FIPS 203 —
  standardised, KAT-vectored, implementable in the same
  userspace-library shape as the rest of libatls.
- **The rig precedent exists**: tcp_x5.h holds 7 pure static-inline
  decision helpers — the O5/R11 "policy core in a header, host test
  drives it" pattern is already TCP practice in this tree.

## 2. Decisions

### D1. Measured, not assumed (inherited)
Every phase lands with numbers in its Result; no number, no claim.

### D2. The decision cores stay host-testable
Congestion control, retransmit-queue arithmetic and the dual-stack
address-selection rules land as pure-C headers next to tcp_x5.h,
each with a host unit gate.  QEMU/SLIRP cannot manufacture loss and
reordering on demand; a host test can, deterministically.  The
in-guest lanes then assert RECEIPTS (counters moving, fetches
completing), not timing.

### D3. One transport, both families
No `tcp6.c` fork.  Y2 cuts a seam (the network-layer ops the
transport calls to resolve, frame and checksum) and Y3 adds the v6
implementation behind it — the same shape as PARITY's blkdev seam
and R7's vmmio ops.  The i386 lane (netglue32) keeps compiling
against the seam; PARITY parity is a standing constraint, checked by
the existing i386 smoke.

### D4. Post-quantum is hybrid or nothing
Y5/Y6 implement ML-KEM-768 and offer it ONLY inside
`X25519MLKEM768` (concatenated shares, both secrets fed to the
schedule).  No standalone-PQ mode, no downgrade of the existing
X25519 path: a server that picks plain X25519 gets exactly the
handshake that passes today's gates.  D7 (constant-time on secrets)
applies to the ML-KEM arithmetic as it does to atls_fe.

### D5. Interop gates run against reference peers
The hybrid handshake is proven against `openssl s_server` with
`-groups X25519MLKEM768` in CI (the X-series' fixture pattern); the
LIVE-web fetch is a user-run protocol (metal_receipts style), not a
CI dependency — CI must stay deterministic.

### D6. Phase hygiene (inherited)
Every phase: CHANGELOG entry, plan table + section updated, one
patch in patches/, checker claims added the same commit
(`tools/check_rinet2_claims.py` arrives with Y0), deviations named.

## 3. Phases

### Y0 — the rig — ✅ COMPLETE
- [x] `tools/check_rinet2_claims.py` (the D8 family's tenth member)
      + selftest; wired into test-unit.
- [x] perfstat rows: `tcp_retransmits`, `tcp_fast_retransmits`,
      `tcp_rto_events`, `tcp_cwnd_limited_sends` — visible in
      `/proc/perf` from day one (the H4-counter precedent: the slot
      exists before the win).
- [x] Host gates for the decision cores stand (CORRECTED, see
      Result: test_tcp_x5 + the M6 quartet already exist).
- [x] Opener receipts pinned in the checker: the cwnd-constant line,
      the zero-AAAA grep, the zero-AF_INET6 grep, the X25519-only
      negotiation lines.
- [x] Exit: test-unit green with the new gates; counters print at 0.

Result: the rig's first catch is the PLAN — its own §1 was
understated, the A3 precedent verbatim.  Reading the M6 layer
(which the plan's greps had missed) shows MATURITY M6/M6c/M6d/M6e
already carry: dup-ACK classification per RFC 5681 §2 (window
updates don't count), fast retransmit/recovery WITH the §3.2
ssthresh arithmetic, a real retransmit queue, SACK (RFC 2018),
Nagle + TCP_NODELAY, delayed ACK, TIME_WAIT/CLOSE_WAIT/LAST_ACK,
listen backlog, SO_REUSEADDR, keepalive — each with a host unit
gate (test_tcp_x5 + test_tcp_m6{,c,d,e}; the plan's "they have
never had their own unit test" was flatly wrong, tcp_x5.h's own
header names its test).  The §4 "no SACK" non-goal was moot on
arrival — struck, with the receipt in place.  What Y1 truly owes
is now EXACT: cwnd GROWTH — the init is wide open
(cwnd=ssthresh=TCP_WINDOW at tcp.c:774/886, so slow start never
runs before the first loss), and the per-ACK increase is an
unconditional `cwnd += 1460` (no SS/CA split; CA wants
MSS²/cwnd).  The collapse (RTO → ssthresh=cwnd/2, cwnd=1 MSS) and
the recovery deflate already exist.  The four counters landed
end-to-end (enum, wire names, tcp.c increments; retransmits and
RTO events are LIVE — X5's paths count them; cwnd-limited is
reserved at zero until Y1 by construction) and perfstat.c became
a KERNEL32_SHARED row so the i386 lane counts too.  RIDER: the
red CI on the DOCS commit dissected — test_rng's FULL
byte-frequency band (±50% = 4σ) finally paid its own bill (byte
0x42 count 100 ≈ 4.5σ; kernel bytes identical to the green R12
run; the sibling jitter lane PASSed).  The band is now ±75%
(16..112, P≈9e-6 per boot — once per ~110k boots) with the
arithmetic and the run id at the site; a stuck generator still
overshoots by orders of magnitude.

### Y1 — congestion control — ✅ COMPLETE
- [x] `tcp_cc.h` (pure C): slow start, congestion avoidance,
      the RTO loss-window collapse; ssthresh unified on the m6
      helper (fast retransmit / fast recovery were ALREADY M6's —
      the Y0 correction stands).
- [x] tcp.c drives cwnd from the core; counters from Y0 move.
- [x] Host gate: the growth arithmetic proven deterministically
      (test_tcp_cc, 20 checks).
- [x] Guest gate: the existing socat round-trip lanes stay green;
      `/proc/perf` shows `tcp_cwnd_limited_sends` > 0 on the bulk
      lane (the receipt that cwnd is ALIVE, not decorative).
- [x] Exit: cwnd is a variable with a test that fails if it stops
      being one.

Result: cwnd is alive, with a number — the x5 1 MiB upload now
starts inside slow start (IW per RFC 6928: `tcpcc_iw(1460)` =
14600, not 64240) and hits the cwnd edge **586 times** on its way
up (`tcp_cwnd_limited_sends 586` in the lane's /proc/perf, asserted
> 0 in CI; the counter Y0 reserved at zero moved the day the
arithmetic landed).  The core is 3 functions in tcp_cc.h: the
RFC 6928 IW (both bounds kept so the formula reads like the RFC),
`tcpcc_ack_grow` (slow start with ABC L=1 — growth clamps to
min(acked, SMSS), so a piecemeal-ACK server cannot inflate cwnd
faster than it actually acknowledges — and CA's MSS²/cwnd with a
max(1,·) anti-stall), and the §3.1 loss window.  tcp.c changes are
four sites: two inits, the progress-ACK growth (replacing the
unconditional `+= 1460`), and the RTO collapse — which now shares
tcpm6_recovery_ssthresh with fast retransmit (the old RTO path
halved CWND instead of FLIGHT and floored at 1 SMSS; one formula,
both signals, named).  CATCH, fixed and named at the site: the
M6 recovery-deflate had NO EDGE — `if (!in_recovery && cwnd >
ssthresh) cwnd = ssthresh` fired on every progress ACK, invisible
while ssthresh sat at TCP_WINDOW, but it would have clamped
congestion avoidance to ssthresh FOREVER the moment ssthresh went
live (i.e. the moment this phase shipped).  The deflate now fires
on the recovery exit edge only (`was_recovery &&
!in_recovery`).  test_tcp_cc pins 20 decisions: IW across the
PMTUD ladder, SS doubling per RTT, the ABC clamps both ways, the
SS→CA crossover at ssthresh, CA's ~1-MSS-per-RTT rate, the
anti-stall at giant cwnd, the loss window, the unified ssthresh
floor, the 1-MSS→ssthresh climb-back in ~log2 RTTs, the cap and
the uint32 wrap guard.  All 12 x5 assertions green including the
three new counter pins; kernel and kernel32 both carry the change
(tcp.c is a shared row — the i386 lane counts too).

### Y2 — the TCP/IP seam — ✅ COMPLETE
- [x] `kernel/net/netl3.h` ops: resolve, frame+send, pseudo-header
      checksum, MSS hint — the v4 implementation is TODAY's code
      moved behind the ops, byte-identical wire output (pcap A/B
      as the gate).
- [x] The connection key widens (family + 16-byte `netl3_addr_t`);
      `NETL3_AF_INET6` is numbered to match libc `AF_INET6` (=10).
      `sockaddr_in6` stays out of libc (Y3 wires the ABI; the Y0
      opener pin is untouched).
- [x] i386: netglue32 is unchanged; `netl3.c` is a KERNEL32_SHARED
      row so the port compiles against the seam (D3).
- [x] Exit: zero inline-IPv4 references left in tcp.c (the Y0
      checker pin flips from 7 to 0).

Result: the transport no longer spells L3.  `struct netl3_ops`
carries the four named members (resolve / output / pseudo / mss);
`netl3_v4_ops` is today's sender moved — IP ident=3, DF, TTL=64,
RFC 793 v4 pseudo-header, 60-byte pad — and `netl3_input` is the
shared ethertype demux (0x0800 today; Y3 adds 0x86DD behind the
same function, not a `tcp6.c`).  tcp.c's connection key is
`netl3_addr_t peer` (family + 16 octets); `tcp_open(uint32_t)` is
unchanged so every existing caller keeps compiling.  CATCH, named
at the site: the pre-seam sender stored `htons(~sum)` into a
packed `uint16_t`, so the A/B copies those two bytes rather than
writing network-order octets by hand — the first draft of the
builder failed the SYN frame on exactly that store.  Receipts:
`test_netl3` pins 28 decisions (address key, family numbers, MSS,
ops shape, pseudo match, SYN pad-to-60 A/B, odd-length data A/B,
parse of a built frame, short-frame refuse, v6 ethertype refuse);
`tcp.c` greps `ip4`+`ipv4` at 0 (was 7); both kernels link
(`kernel.elf`, `kernel32.elf` — netglue32 untouched).  The Y0
opener pin for inline-IPv4 retired with the phase.

### Y3 — TCP-over-IPv6 + AF_INET6 + AAAA — ✅ COMPLETE
- [x] netl3 v6 implementation: NDP resolve (R9's neighbour cache),
      v6 pseudo-header, no-fragment MSS from the v6 MTU.
- [x] `AF_INET6` end-to-end through the socket syscalls; `connectaddr`
      to a `sockaddr_in6` opens over v6 (`SYS_SOCKET_CONNECT6` = 308).
- [x] DNS: AAAA parse + query (`dns_parse_aaaa` / `dns_resolve_aaaa`)
      and dual-stack selection in `dualstack.h` (v6 preferred when a
      global address exists; v4 fallback otherwise).
- [x] Guest gate: TCP round-trip to `fec0::2:8036` (SLIRP vhost,
      same-port host listener — guestfwd is IPv4-only on QEMU 10),
      pinned in `test_tcp6.sh`.
- [x] Exit: `[tcp6] PASS: round-trip N byte(s)` in CI.

Result: one transport, both families — no `tcp6.c`.  `netl3_v6_ops`
is the second consumer of the Y2 seam: resolve is R9's NDP (exported
as `net_ipv6_resolve`), output builds Ethernet+IPv6 (ethertype
0x86DD, hop 64, MSS 1440), input demuxes 0x86DD before the v4
fragment table can see it.  `tcp_open_addr` is the family-agnostic
open; `tcp_open(uint32_t)` is a v4 wrapper so every existing caller
is unchanged.  CATCH, named: i386 has no v6 stack, so netglue32
stubs `net_ipv6_resolve` / `net_ipv6_src_for` to fail closed — the
SHARED row still compiles (D3), TCP-over-IPv6 is x86_64 this phase.
`sockaddr_in6` landed in `<netinet/in.h>` and is named in
`<sys/socket.h>` so the Y0 opener pin flips.  AAAA is parsed and
queried; the A cache stays IPv4-shaped (AAAA is not inserted there).
`dualstack_pick` is 8 host checks.  `test_netl3` is 40/40 (v6 build/parse added); it grew the v6
build/parse pins; `test_dns_aaaa` pins type 28 / fec0::2 / TC / bad
ID.  The guest receipt is the same shape as i386's `[tcp32] PASS`:
a boot probe to fec0::2:8036, skip-loud when nobody is listening,
`[tcp6] PASS: round-trip 15 byte(s): PONG-FROM-HOST` when the case's listener is.
CATCH, named at the fixture: QEMU 10 rejects
`guestfwd=tcp:[fec0::2]:8036-...` ("Invalid guest forwarding rule");
fec0::2 is SLIRP's ipv6-host, so the host binds :8036 like tcp32
binds :8032.  CATCH, named at the RX loop: the first boot's pcap
was four SYNs and zero SYN-ACKs — TCP owned the NIC and ate SLIRP's
NS for our SLAAC address; `tcp_recv_segment` now feeds every frame
to the R9 NDP responder (the same "serve NDP meanwhile" shape
`net_ping6` already had).  The Y0 opener pins for AAAA /
sockaddr_in6 retired with the phase.
Happy-eyeballs remains a named non-goal.

### Y4 — HTTPS-over-IPv6 (RES-26 closes) — ✅ COMPLETE
- [x] libahttp resolves AAAA, dials v6, falls back v4; the TLS layer
      is transport-agnostic already (it reads/writes a handle).
- [x] Guest gate: an HTTPS fetch over IPv6 against the local
      s_server fixture on SLIRP's ipv6-host (same-port listener —
      guestfwd is IPv4-only on QEMU 10, the Y3 catch).
- [x] Ledger: RES-26 → DONE@Y4; the terminal OPEN count drops 6→5.
- [x] Exit: `[https6] PASS` pinned in CI; ledger arithmetic moved
      same-commit.

Result: HTTPS rides the Y3 transport.  `ahttp_url_parse` accepts
`[fec0::2]:port`; `parse_ip6` + `dns_resolve_aaaa` (SYS_DNS_AAAA=309)
feed `dualstack_pick` (a learned AAAA is enough to prefer v6 —
userspace has no SLAAC query this phase); the dial is v6 first,
serial v4 fallback, logged `[ahttp] dial v6`.  CATCH, named at the
fixture: QEMU 10 still cannot guestfwd IPv6, so s_server binds
`[::]:8446` and the guest hits `fec0::2` the Y3 way.  CATCH, named
at TLS: IP-literal fetches skip chain hostname match (atls is
DNS-SAN only; CertificateVerify still runs).  Guest receipt
`[https6] PASS: status 200 body 3912 via v6` from `/tests/https6`.
RES-26 is DONE@Y4; ledger OPEN 6→5.

### Y5 — ML-KEM-768 (FIPS 203) — ✅ COMPLETE
- [x] `lib/libatls/src/atls_mlkem.c`: K-PKE + the FO transform,
      768-parameter set only; NTT/invNTT over Z_q[X]/(X^256+1),
      centered binomial sampling, SHAKE via a new atls_sha3.c
      (Keccak-f[1600] — also needed by the FIPS 203 hashes G/H/J).
- [x] Constant-time discipline: no secret-dependent branches or
      table lookups (D7's grep extends to the new files).
- [x] Host gate: NIST KAT vectors (keygen/encaps/decaps), negative
      controls (corrupted ciphertext → implicit-rejection secret,
      not an error path — the FO contract).
- [x] The i386 question is measured, not assumed: the reference
      arithmetic is 16/32-bit friendly (q = 3329); the -m32 gate
      grows the new files (the R10 lane pattern).
- [x] Exit: KAT battery green on x86_64 host, -m32, rv64, a64 (the
      four-width crypto precedent from R10).

Result: ML-KEM-768 is a userspace primitive, not a TLS group
(Y6 wires 0x11EC).  `atls_sha3.c` is Keccak-f[1600] + SHA3-256/512
+ SHAKE128/256 (5 FIPS 202 shorts green).  `atls_mlkem.c` is the
768 set only (k=3, η1=η2=2, du=10, dv=4); KeyGen_internal domain-
separates G(d ‖ k).  Host gate `test_atls_mlkem` is **23/23**:
ACVP sample keyGen ek+dk, encaps ct+ss, decaps ss (NIST
`ML-KEM-*-FIPS203` sample set), a local round-trip, and the FO
contract — a flipped ciphertext byte returns `ATLS_OK` and
`J(z ‖ ct)`, not the honest K.  Four widths EXECUTED:
x86_64 host, `-m32` (FORCE32 + real ILP32), `qemu-riscv64`,
`qemu-aarch64`.  CATCH, named at D7: `test_atls_hash`'s
`file_contains` read 8 KiB and would have missed a banned token
past the first page of `atls_mlkem.c`; the scan is whole-file
now and the new TUs join the list.

### Y6 — X25519MLKEM768 — ✅ COMPLETE
- [x] ClientHello offers 0x11EC alongside X25519; key-share =
      ML-KEM encaps key ∥ X25519 public (the IETF draft order);
      shared secret = ML-KEM ss ∥ X25519 ss into the existing
      HKDF schedule.
- [x] Server-side selected_group handling accepts either group;
      plain-X25519 servers negotiate exactly as before (D4).
- [x] CI interop gate: `openssl s_server -groups X25519MLKEM768`
      fixture (the X-series pattern); the fetch completes and the
      transcript names the group.
- [x] Exit: `[tls] PASS: X25519MLKEM768` in CI; the X9 "PEER_EOF
      against Cloudflare" sentence becomes closable by a user run.

Result: the ClientHello carries both groups and both key shares
(hybrid first).  Hybrid share is 1216 octets — ML-KEM-768 ek
(1184) ∥ X25519 public (32); server share is ct (1088) ∥ X25519
public (32) = 1120; IKM into HKDF-Extract is ML-KEM ss ∥ X25519
ss (64).  Host gate `test_atls_tls` is **32/32**: the X25519
fixture (D4, `-groups X25519`) stays green; a second
`s_server -groups X25519MLKEM768` negotiates `group=0x11ec`
and exchanges application data.  Guest
`tests/integration/cases/test_x25519mlkem.sh` against
`:4434` prints `[tls] PASS: X25519MLKEM768` and
`[tlstest] ALL PASS` (7/7 asserts).  CATCH, named at the
ClientHello: offering 0x11EC without a matching KeyShareEntry
makes OpenSSL 3.5.6 `extract_keyshares` emit `bad key share`
(alert 47); both entries travel so neither a hybrid-only nor
an X25519-only peer needs HelloRetryRequest.  CATCH, named at
the N3 fixture: OpenSSL 3.5's default group list includes
`X25519MLKEM768`, so `test_tls.sh` is pinned `-groups X25519`
and stays the fast D4 path.

### Y7 — close-out — ✅ COMPLETE
- [x] The live-web protocol: a metal_receipts-style section — the
      user runs the fetch against a real PQ-preferring host and
      pastes the transcript line (pending-user, not CI).
- [x] Residue to the ledger: every leftover named, classed W/M/N/S,
      appended as RES-49..53; this plan's harvest stays 8 (no new
      marker lines).
- [x] Terminal arithmetic against the Y0 opener receipts.

Result: the series is closed with numbers, not a live-web CI
lane (D5).  `docs/live_web.md` is the paste-back protocol;
`libahttp` prints `[tls] group=` after every handshake so the
line exists even when chain validation refuses the peer.
Host openssl 3.5.6, 2026-08-24: `www.ietf.org`,
`www.cloudflare.com` and `example.com` all negotiate
`X25519MLKEM768` + `TLS_CHACHA20_POLY1305_SHA256`.
`www.ietf.org` is the slot-1 host (P-256 SHA-256 CV + ISRG
Root X1).  CATCH, named at the store: Cloudflare / example.com
chain to GlobalSign / SSL.com — three shipped roots, so slot 3
is a group receipt, not an HTTP 200.  CATCH, named at CV:
`rsa_pss_rsae_sha256` is advertised and not verified
(`rust-lang.org` fails there) — RES-53.  CATCH, named at Y4:
`test_https6.sh` is pinned `-groups X25519` so OpenSSL 3.5
cannot pull hybrid into the 90s IPv6 fixture.  Ledger grew
48→53; OPEN 5→6 (only RES-53 is new work).  Y0 opener vs
this tree is §5.

## 4. What this plan deliberately does not do

- No happy-eyeballs connection racing (dual-stack picks, then
  falls back serially) — named non-goal, revisit on measured pain.
- No TCP window scaling — the 64240 window stays; a follow-up row if
  bulk numbers demand it.  ~~No SACK~~ **struck at Y0 — stale on
  arrival: SACK landed with MATURITY M6d (RFC 2018, tcp_m6d.h + the
  retransmit queue M6c laid) before this plan was written; the Y0 rig
  measured the layer this plan's opener section had missed.**
- No DTLS, no QUIC, no TLS client certificates.
- No standalone ML-KEM TLS group (hybrid only, D4).
- No kernel-side TLS: the crypto stays userspace libatls (N-series
  D2 stands).

## 5. Terminal arithmetic — filled at close

Y0 opener facts vs this tree, measured 2026-08-24.  The checker
pins the same greps.

| Y0 opener | Y7 close |
|-----------|----------|
| `conns[h].cwnd = TCP_WINDOW` at two inits; comment "until N7" | both inits are `tcpcc_iw(TCP_MSS)`; x5 1 MiB lane hit the edge **586** times (`tcp_cwnd_limited_sends 586`) |
| tcp.c `ip4`+`ipv4` = 7 | **0** |
| dns.c AAAA = 0; no `sockaddr_in6` | AAAA = **3**; `sockaddr_in6` in `<netinet/in.h>` and `<sys/socket.h>` |
| `ATLS_GROUP_X25519` only (reject anything else) | `0x11EC` + `0x001d`; host `test_atls_tls` **32/32**; guest `[tls] PASS: X25519MLKEM768` |
| RES-26 OPEN (HTTPS-over-IPv6) | DONE@Y4; the five R-series OPEN rows stay (RES-02/06/07/16/18); Y7 adds RES-53 |
| tcp.c 1556 lines | **1518** (L3 moved behind `netl3`) |

Patches named in the table: Y7 writes `RINET2_Y7_close.patch` this
close.  The four Y0 counters still exist in `/proc/perf`; cwnd-limited
is live, not decorative.
