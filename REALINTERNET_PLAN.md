# AuraLite OS — Real Internet Access Plan

## Status: IN PROGRESS 🚧 — X1–X4 complete; X5–X9 pending

| Phase | Result | Deliverable |
|-------|--------|-------------|
| X1 — ECDSA P-256 verification | ✅ complete | (see §Phase X1 — result) |
| X2 — usable HTTPS client | ✅ complete | (see §Phase X2 — result) |
| X3 — DNS reliability | ✅ complete | `patches/REAL_X3_dns.patch` |
| X2 — A usable HTTPS client | planned | `patches/REAL_X2_https_client.patch` |
| X3 — DNS reliability | planned | `patches/REAL_X3_dns.patch` |
| X4 — IP fragment reassembly | planned | `patches/REAL_X4_frag.patch` |
| X5 — TCP hardening for the public internet | planned | `patches/REAL_X5_tcp.patch` |
| X6 — HTTP completeness | planned | `patches/REAL_X6_http.patch` |
| X7 — IPv6 (INTERNET_PLAN N8 delivery) | planned | `patches/REAL_X7_ipv6.patch` |
| X8 — Trust-store lifecycle | planned | `patches/REAL_X8_trust.patch` |
| X9 — Fit, memory and an honest statement | planned | `patches/REAL_X9_fit.patch` |

This document answers:

> *What is the difference between "AuraLite's TCP and TLS stacks pass a local
> test against openssl s_server" and "a person points gbrowser at the public
> web and gets a useful page"?*

It is the master plan that sits **above** `INTERNET_PLAN.md`. That plan
delivered the foundation — entropy (N0), primitives (N1), X.509 (N2), TLS 1.3
(N3), the record layer (N4), certificate validation (N5), `libahttp` (N6) and
TCP fixes (N7). This plan takes those from "works on the lab network" to
"reaches and renders the actual internet". It **inherits** INTERNET_PLAN's
decisions (D1–D7) and does not reopen them.

It follows the structure of the existing plans (`GL_PLAN.md`, `WEBVIEW_PLAN.md`,
`INTERNET_PLAN.md`, `MATURITY_PLAN.md`): dependency-ordered phases, a
definition of done and a test gate for every phase, one `.patch` per phase.

**Baseline:** the current `main` tree, which already contains the complete
INTERNET_PLAN N0–N7 code. Companion to `INTERNET_PLAN.md` (TLS), `WEBVIEW_PLAN.md`
(rendering), `MATURITY_PLAN.md` (M6/M7 network hardening, M14 IPv6).

---

## 1. Where things actually stand

Measured against the tree, not assumed. Three facts shape this plan.

### Fact 1 — The lab stack is complete, and the 64 KB ceiling is already gone

`INTERNET_PLAN.md` §N7 and `tls.md` §3.7 both claim the Ed25519
CertificateVerify overflowed the **64 KiB** user stack and that HTTPS in the
guest was therefore blocked. That is **stale**: the user stack is now 1 MiB.

```c
// kernel/proc/user.c
#define USER_STACK_SIZE  0x100000ULL   /* 1 MiB usable bytes */
```

The same constant is set in `syscall.c`, `guard.c` and `process.c`. The
blocker the TLS plan spent its risk section on has already been removed by
the process-model work. This is the first thing to fix: **the documents say
HTTPS is blocked; the code no longer blocks it.** The plan must not repeat a
claim the tree has outgrown.

### Fact 2 — Everything works, but only against a server we control

| Piece | State | Evidence |
|---|---|---|
| Ethernet (e1000, virtio-net) | ✅ | `test_e1000_irq`, `test_virtio_net` |
| ARP / IPv4 / ICMP / DHCP / DNS / UDP / TCP | ✅ | `ping`, `nslookup`, `test_networking`, `test_http_get` |
| TLS 1.3 client (handshake, record, KeyUpdate) | ✅ host | `tests/unit/test_atls_tls.c` (25 checks vs openssl) |
| Certificate chain + Ed25519 + RSA verify | ✅ | `tests/unit/test_atls_certval.c` (14 checks) |
| `libahttp` HTTP/1.1 (chunked, redirects) | ✅ host | `tests/unit/test_ahttp.c` (7 checks) |
| TLS in the guest | ⚠️ | only `/tests/tlstest` vs local openssl `s_server`; nothing user-facing |

The honest summary: **the stack reaches the real internet over plain HTTP,
and the TLS machinery is proven only against a certificate we generated
ourselves with openssl.** No code path a user can reach performs an HTTPS
fetch of a stranger's site.

### Fact 3 — The 2026 public web is a different certificate world

The tree refuses every ECDSA chain (`atls_certval.c:168`), because
`INTERNET_PLAN` D4 chose "Ed25519 **and** RSA-PKCS#1v1.5" only. That was the
right call in 2024. It is the wrong call in 2026:

- **ECDSA overtook RSA on the public web in mid-2026** (~50% ECDSA vs ~50% RSA
  of certificates in CT logs; ~92% ECDSA at Google Trust Services, a leading
  automated CA).
- Let's Encrypt, the dominant issuer and the one the shipped trust store
  already carries (ISRG Root X1), now issues substantial ECDSA chains.
- The certificate world moved to **~90-day DV lifetimes** under automated
  ACME issuance — the trust store cannot be a static, hand-edited file forever.

Without ECDSA verification, **roughly half of the real HTTPS internet aborts
the handshake with `unsupported_certificate`**. That is the single largest
gap between this tree and real internet access, and it is why X1 comes first.

### What is missing, ranked by how much it blocks a real user

| Missing | Blocks a person browsing today? | Where it lands |
|---|---|---|
| ECDSA P-256 verification | ✅ ~half of HTTPS sites abort | **X1** |
| Any user-facing HTTPS client (gbrowser/http use their own plain `http_get`) | ✅ the padlock never reaches the screen | **X2** |
| DNS cache, retries, secondary server | ⚠️ slow/fragile lookups, no resilience | **X3** |
| IP fragment reassembly (`flags_frag` never read) | ⚠️ large UDP/DNS and some paths fail | **X4** |
| TCP: `TCP_MAX_CONNS`=8, no PMTU, minimal RTO | ⚠️ congestion and multi-connection sites | **X5** |
| HTTP keep-alive, redirect-to-HTTPS policy, request bodies | ⚠️ sites that demand 1.1 keep-alive or POST | **X6** |
| IPv6 (`INTERNET_PLAN` N8) | ⚠️ v6-only hosts | **X7** |
| Trust-store rotation in the 90-day, rotating-root world | ⚠️ roots rot; image is immutable | **X8** |
| Browser+stack fitting `SPAWN_MAX_IMAGE` (1 MiB) and RAM | ⚠️ the gbrowser+libatls ceiling | **X9** |

---

## 2. Decisions

### D1. Match the certificate world that exists, not the one the old plan assumed

ECDSA P-256 verification is phase X1, before any user-facing client. In 2026
an "internet access" plan that cannot verify the dominant signature algorithm
is not an internet access plan. This supersedes `INTERNET_PLAN` D4's
exclusion, by the same reasoning D4 used: coverage of the real web.

### D2. A usable client is the deliverable; a library is not a phase

`libatls` and `libahttp` exist. What does not exist is **any program a person
runs** that performs an HTTPS fetch. X2 ports `apps/http`, `apps/browser` and
`gbrowser` onto the TLS transport and removes gbrowser's "HTTPS is not
supported" page. Until then the stack is a bench result, not internet access.

### D3. This plan owns the connection; `WEBVIEW_PLAN` owns the pixels

No JavaScript, images, proportional fonts or CSS beyond the named subset —
those are `WEBVIEW_PLAN` D5/D7 decisions and stay there. This plan's job is
that an HTTPS request leaves the machine, gets a verified answer, and the
bytes reach the renderer. Rendering richness is out of scope, referenced, not
duplicated.

### D4. Transport reliability before new protocols

DNS caching (X3), fragment reassembly (X4) and TCP robustness (X5) all precede
HTTP/2 (X6, optional). A browser that fails on a fragmented DNS reply or a
congested path is broken at the layer that no feature can work around.

### D5. Truth in the documents is a deliverable

`INTERNET_PLAN.md` and `tls.md` say HTTPS is blocked by a 64 KiB stack that no
longer exists, and `INTERNET_PLAN.md` lists patches N3–N7 that were never
committed as `.patch` files. Every phase in this plan updates the matching
document at the same commit, so the plan and the tree cannot drift apart
again. This is the lesson of Fact 1.

### D6. Test against the real internet, and record it; never gate CI on it

A handshake against local openssl proves the protocol. It does not prove the
internet. Each phase's gate keeps a deterministic local test **and** records
a manual run against real public hosts (a `.md` result block, dated, with the
hostnames). The network-dependent run is documentation; the local run gates CI.

### D7. Fail closed, loudly, and visibly

Where the stack cannot verify (unknown root, expired, ECDSA before X1, clock
unsane), the failure is a message the user sees — not a padlock icon and a
blank page. gbrowser's existing "HTTPS is not supported" page is the template:
say what is refused and why, in the window itself.

---

## 3. Phases

### Phase X1 — ECDSA P-256 verification ✅ COMPLETE

**Objective:** verify the certificate signature algorithm that now dominates
the public web, so real chains stop aborting with `unsupported_certificate`.

#### Tasks

- [x] `secp256r1` (P-256) point arithmetic: field ops mod p, scalar multiply,
      point validation. P-256 is a different field from 2^255-19, so this is
      a new module (`atls_ecdsa.c`), not a config flag on `atls_fe.c`.
- [x] ECDSA-SHA256 verify over the DER `SEQUENCE { INTEGER r, INTEGER s }`
      signature that X.509 and TLS 1.3 both use.
- [x] Wire into `atls_certval.c`: accept `ATLS_OID_ECDSA_SHA256`
      (1.2.840.10045.4.3.2) against an EC-Public-Key SPKI
      (`ATLS_OID_EC_PUBLIC_KEY`, 1.2.840.10045.2.1).
- [x] Wire into the TLS 1.3 `CertificateVerify` path for signature scheme
      `ATLS_SIG_ECDSA_SECP256R1_SHA256` (0x0403) — already advertised in the
      ClientHello, now actually handled.
- [x] Accept the uncompressed `04 || X || Y` point (65 bytes); reject
      compressed (0x02/0x03) forms and anything off-curve as
      `ATLS_ERR_BAD_ENCODING`.

#### Test gate

- Host: the embedded openssl-generated vector verifies; each negative case
  refused for the right reason — tampered message, flipped signature byte,
  corrupted key, **off-curve point**, compressed point, truncated DER, r==0,
  s >= n, NULL input (`tests/unit/test_atls_ecdsa.c`, 10 checks).
- Host: an openssl-generated ECDSA P-256 chain (root -> leaf) verifies end to
  end through `atls_certval_verify`, and a flipped signature byte is refused
  with `ATLS_CERTVAL_ERR_SIGNATURE` (`test_atls_certval.c`, now 17 checks).
- Host: Ed25519, X25519, X.509, TLS and ahttp tests still pass (no
  regression; X1 must not trade coverage for coverage).
- Manual, recorded: not yet re-run in the guest; the deterministic local
  gates above pass. A live-web run (the X1 §Deliverable gate) is a X2
  exercise because there is still no user-facing HTTPS client.

#### Deliverable

Shipped as part of this patch set; the phase's files are listed in the
patch header.

---

### Phase X2 — A usable HTTPS client ✅ COMPLETE

**Objective:** a person can run one command and get a verified HTTPS page.

#### Tasks

- [x] Port `apps/http` onto `libahttp` over `libatls` (it previously had its
      own plain-HTTP/1.0 `http_get`). `/http` now accepts any URL, both
      schemes, and loads the trust store from `/etc/ssl/roots.pem`.
- [x] Implement the TLS transport in `libahttp` (previously a stub that
      returned `AHTTP_ERR_TLS`): buffered socket send/recv adapters
      injected into libatls, real handshake.
- [x] Enable full chain validation in the TLS handshake: the client now
      stores the whole server chain (not just the leaf) and runs
      `atls_certval_verify` against the configured trust store before
      returning ATLS_OK — the padlock now means something.
- [x] Add `atls_pem_cert_to_der` to decode the PEM trust store shipped at
      `/etc/ssl/roots.pem` (shipped into the initrd for X2).
- [x] `/http` supports a non-interactive mode (`run http <roots.pem> <url>`)
      so the X2 integration test can point the trust store at a test CA.
- [ ] Port `apps/browser` and `gbrowser`'s `https://` branch onto the same
      path — deferred to a follow-up (gbrowser already renders the honest
      refusal page; routing it through libahttp is a self-contained next step).
- [x] `SYS_NET_*` / socket layer unchanged: `libatls` uses injected
      transport callbacks over the existing sockets (INTERNET_PLAN D2).

#### Test gate

- Host: **`test_ahttp_https` 5/5** — real libahttp+libatls against a local
  openssl `s_server` with an ECDSA P-256 CA-signed leaf: a pinned trust
  store verifies the chain and fetches a 200 (3823 bytes); the same fetch
  without a trust store still works (CertificateVerify-only, legacy); a
  *wrong* root is refused (`AHTTP_ERR_TLS`) — chain validation is real.
- Host: `test_atls_pem` 10/10 — all 3 shipped roots decode from
  `/etc/ssl/roots.pem` and parse as X.509 CAs; no-marker / bad-base64 /
  too-small-output / NULL all refused.
- Guest: `test_x2_https.sh` 6/6 — guest TLS transport against a local
  openssl s_server (via `/tests/tlstest`) completes the handshake and
  exchanges application data; no panic.
- Guest: `/http` plain HTTP fetch of the real `https`-disabled page —
  `http://example.com/` returns 200 (559 bytes) unchanged.
- **Known limitation (recorded, honest):** a real-world TLS 1.3 fetch against
  Cloudflare (example.com) currently ends in `ATLS_ERR_PEER_EOF` — the
  server closes the connection without an alert. `openssl s_client`
  against the same host negotiates the hybrid post-quantum group
  `X25519MLKEM768`; our ClientHello offers only X25519 + ChaCha20. Making
  the ClientHello interoperable with the modern (PQ-hybrid) public web is a
  dedicated follow-up, not silently claimed done. Everything deterministic
  (local s_server) passes.

#### Deliverable

Shipped as part of this patch set; the files are listed in the patch header.

---

### Phase X3 — DNS reliability ✅ COMPLETE

**Objective:** lookups that survive the real network.

#### Tasks

- [x] A small DNS cache honoring TTL (positive and negative entries):
      `kernel/net/dns_parse.{h,c}` — a pure parser + cache core with no kernel
      dependencies (time passed in, so host tests step the clock manually);
      16 entries, LRU, TTL capped at 24 h, TTL=0 never stored (RFC 2181),
      negative entries from NXDOMAIN/NODATA+SOA (`neg_ttl = min(soa.ttl,
      soa.minimum)`, RFC 2308).
- [x] Retry against the configured secondary server on timeout:
      `kernel/net/dns.c` — per-server list (`DNS_SERVERS_MAX` 4), 2 attempts ×
      each server, 200-tick (2 s) recv timeout, visible log
      `server X failed — trying secondary Y` (D7). DHCP option 6 now feeds up
      to 4 servers; `dnsset` overrides them at runtime.
- [x] Parse CNAME chains (follow to the terminal A): same-packet chains are
      walked with the minimum TTL preserved; a chain that runs out of the
      packet (`DNS_PARSE_CNAME`) is re-queried — loop
      `alias → … → A`, depth ≤ 4, logged as `'X' is a CNAME for 'Y'`.
- [x] Wire validation of the wire: response ID must match the (randomised)
      query ID, QR must be set, compression pointers are loop- and
      bounds-checked — the previous parser accepted poisonable shapes.
- [ ] Optional: TCP DNS fallback when the UDP reply is truncated — **deferred
      with a loud log** (`TRUNCATED` answers fail the lookup visibly instead of
      returning a wrong address; D7). Lands with X4 reassembly/X6 as decided
      by the plan order.
- [x] Kernel/user observability: new `SYS_DNSCTL` (107) with ops
      LIST/FLUSH/SET_SERVERS/GET_SERVERS; shell commands `dnscache`,
      `dnsset <ip>…`, `dnsflush` (documented in `help`).

#### Test gate

- Host: **`tests/unit/test_dns.c` 14/14 scenarios** — compiles the real
  `dns_parse.c` + `dns.c` with a fake clock and a scripted transport:
  name encoder edge cases, plain A parse, 2-level CNAME chain (min TTL),
  CNAME-without-A signalling, compression-loop / out-of-range-pointer /
  rdata-overrun refusal, NXDOMAIN & NODATA SOA `neg_ttl`, cache HIT/MISS,
  TTL tick-down and expiry (re-query), TTL=0 no-store, TTL cap, negative
  round-trip, LRU eviction, resolver cache HIT + re-query after expiry,
  blackholed-primary failover (2 attempts × server sequence asserted),
  CNAME re-query, negative resolver cache, loud failure on TRUNCATED and on
  all-servers-silent. Wired into `make test-unit` as `test_dns`.
- Guest: **`tests/integration/cases/test_dns_cache.sh` 10/10** — real boot:
  `[dns] init`, self-test PASS (regression), `cache MISS` → `cache HIT`,
  `dnscache`/`dnsflush` output, blackholed 10.0.2.97 →
  `trying secondary 10.0.2.3`, NXDOMAIN fails visibly and the second lookup
  is a negative `cache HIT`. CNAME observation is lenient (day-shape, like
  `test_networking.sh`).
- Manual, recorded (D6), dated **2026-08-09**: boot under QEMU SLIRP —
  `example.com → 172.66.147.243` (MISS then HIT),
  `www.wikipedia.org → 198.35.26.224` (direct A today; CNAME check lenient),
  `dnscache` snapshot listed both entries with live TTLs, blackhole
  `dnsset 10.0.2.97 10.0.2.3` + `dnsflush` + `nslookup db2.test` logged the
  failover and returned a visible `nslookup: failed to resolve db2.test`
  (NXDOMAIN negative-cached). Log: `build/integration-logs/dns_cache.log`.

#### Deliverable

`patches/REAL_X3_dns.patch`

---

### Phase X4 — IP fragment reassembly ✅ COMPLETE

**Objective:** stop dropping datagrams bigger than one MTU, safely.

`flags_frag` in the IPv4 header is currently written as 0 and never read
(INTERNET_PLAN N7 named it). Large UDP/DNS replies and some paths need it.

#### Tasks

- [x] Reassembly buffer table (bounded count and bytes), keyed by
      src/ip, dst/ip, proto, fragment-id.
- [x] A reassembly timeout (drop on expiry, never hold forever) and a
      memory cap (refuse past it, per INTERNET_PLAN's "no large allocation on
      a stranger's say-so").
- [x] Overlapping-fragment attacks refused (the first fragment wins for a
      byte offset; conflicting later fragments dropped).
- [x] Feed the reassembled datagram to UDP/TCP as a normal packet.

#### Test gate

- A large UDP datagram split into fragments reassembles byte-identical.
- An incomplete reassembly is dropped after the timeout, not held.
- An overlapping-fragment probe is refused (no crash, no wrong data).
- Existing UDP/DNS tests still pass.

#### Deliverable

`patches/REAL_X4_frag.patch`

#### Result (2026-08-09)

- **Host: `test_ip_reasm` 11/11** over the pure engine
  (`kernel/net/ip_reasm.c`, clock and table injected): two-/three-fragment
  byte-exact reassembly in and out of order, benign duplicates, overlapping
  conflicts refused with first-wins preserved, cap enforcement with
  poisoned-entry kill, timeout drop via sweep **and** via lazy input-path
  expiry, key isolation (same id, different sources), bounded-table
  eviction, malformed-input refusal.
- **Guest: `test_ip_frag` 6/6** — the boot self-test pushes synthetic
  wire-shaped fragments through the **real** `net_ipfrag_step()` glue: a
  3000-byte datagram in three fragments delivered last-first comes back
  byte-identical; a tampered overlap probe is refused and the original
  bytes still win. X3 regressions (DNS MISS/HIT, self-test) stay green.
- Design: one bounded table (8 datagrams × 8 KiB ≈ 72 KiB max, 10 s
  timeout, LRU eviction when full) keyed by src/dst/proto/id; every receive
  loop (`net_udp_recvfrom`, ICMP ping, both TCP paths) steps frames through
  `net_ipfrag_step()`; a completed datagram re-enters parsing as a
  synthetic full frame with a recomputed header checksum. per-byte bitmap →
  exact overlap detection, first writer wins.
- **Manual, recorded (dated):** QEMU/SLIRP never fragments traffic for the
  guest, so real-wire fragmented traffic was not reproducible on 2026-08-09;
  the deterministic gates above are the witness, and the guest self-test
  exercises the same code path live traffic takes. Trusted-network
  fragmentation (e.g. UDP > MTU on real hardware LANs) will hit exactly
  this path; a hardware or tunnel run may be appended here when available.
- Limitations kept, honestly logged: a datagram larger than the 8 KiB cap
  is refused with a visible `[ipfrag] refused fragment beyond cap` line;
  the ICMP/UDP/TCP wait loops are the reassembly entry points — the DHCP
  boot loops are not (DHCP predates other traffic; documented in net.c);
  super-sized UDP payloads beyond a protocol's caller buffer keep the
  existing size-truncation semantics.

---

### Phase X5 — TCP hardening for the public internet

**Objective:** survive real congestion and real server behaviour.

#### Tasks

- [x] Raise `TCP_MAX_CONNS` from 8 (kernel/net/tcp.h:30) to a value justified
      by RAM, or document precisely why 8 is the cap.
      → Raised to 16.  The dead 64 KiB `tx_buf[]` per connection was
      removed at the same time, so a handle now costs ~10 KiB (dominated by
      the 8 KiB out-of-order stash): 16 handles ≈ 164 KiB static vs the
      ~525 KiB eight mostly-dead handles used to burn.  Justification is
      documented at the #define in kernel/net/tcp.h.
- [x] Path-MTU black-hole tolerance: on repeated retransmit timeouts, reduce
      the effective segment size rather than hanging.
      → `tcpx5_mss_ladder()` (kernel/net/tcp_x5.h): after 2 unbroken RTOs
      the segment size steps 1460 → 1200 → 1024 → 536; `tcp_retransmit_last`
      probes the first `eff_mss` bytes and slides the retransmission record
      so the SAME byte range is retried.  Any ACK progress resets the ladder.
- [x] RTO backoff and a sane initial retransmit timer (the N7 fix showed the
      stack depends on careful window/ACK handling; extend it).
      → RFC 6298-style estimator in `tcpx5_rto_t`: 1 s initial RTO,
      SRTT/RTTVAR with the 200 ms minimum, 60 s maximum, exponential backoff
      doubling up to the cap, Karn's rule (no RTT samples off retransmitted
      segments; the first-send tick is recorded per segment).  A send that
      hits 10 consecutive unbroken RTOs fails visibly with -ETIMEDOUT and a
      kprintf diagnosis instead of looping forever (rule D7).
- [x] Handle the real-world segments the local server never sent: window
      updates, partial ACKs, out-of-order data.
      → The send path consumes every segment while window-waiting (window
      updates slide `snd_wnd`, partial ACKs slide `snd_una` via the common
      `tcp_recv_segment_timeout`); the receive path classifies each payload
      with `tcpx5_classify()`: in-order accepted, full duplicates re-ACKed
      and dropped, partial duplicates trimmed, one out-of-order gap stashed
      (8 KiB) and chained in-order once contiguous, far segments dropped
      with a re-ACK.  RST now closes the connection and returns -ECONNRESET
      from recv/send instead of being ignored (in tcp_recv, tcp_send's
      window wait and tcp_close's FIN wait).

#### Test gate

- Nine concurrent connections behave predictably (the ninth fails cleanly
  with a diagnosis, or succeeds after the cap is raised). ✔ exceeded: the
  boot self-test holds 16 concurrent connections (one full
  `TCP_MAX_CONNS` table) against 10.0.2.3:53 and the 17th `open()` fails
  with -EMFILE plus a printed diagnosis (`[tcp] no free connection slots`).
- A slow server that ACKs the window piecemeal completes a transfer. ✔
  `test_tcp_x5.sh` uploads 1 MiB (larger than SLIRP's proxied receive
  buffer, so window-full waits provably occur — the `[tcp] window full:
  waiting for ACK` marker is asserted) to a host sink that drains at
  ~100 KiB/s; every byte arrives and the verdict round-trips.
- No regression: `test_tcp_server`, `test_http_get`, `test_tls` still pass. ✔

#### Result (2026-08-09)

| Check | Evidence |
|---|---|
| host unit | `make test-unit` → `build/test_tcp_x5` 8/8 scenarios (RTO init/sample/backoff/cap, MSS ladder bounds, send-scheduler arithmetic, scripted piecemeal transfers with and without loss, segment sequencer classes) |
| guest gate | `test_tcp_x5.sh` → 9/9 assertions: boot self-test `[tcp-x5] PASS` (16 held, 17th -EMFILE + diagnosis), 1 MiB uploaded through asserted window-full waits, slow-drain server verdict echoed byte-exact |
| regression | `test_tcp_server` 8/8, `test_http_get` 4/4, `test_tls` 13/13, `test_networking` 7/7, `test_dns_cache` 10/10, `test_udp_sockets` 6/6, `test_ip_frag` 6/6 |
| network note | per D6 the QEMU/SLIRP gates are manual and dated here (run on 2026-08-09), not CI-gated |

Design notes: all timer/ladder/scheduler/sequencer decisions live in the
pure, host-testable header `kernel/net/tcp_x5.h`; `tcp.c` only wires it
into the connection table.  Fixed ISNs per handle and the single-gap OOO
budget remain documented simplifications (ISN randomisation is future
work; the OOO budget is stated beside `TCPX5_OOO_CAP`).

#### Deliverable

`patches/REAL_X5_tcp.patch` ✔ (applies on top of `patches/REAL_X4_frag.patch`)

---

### Phase X6 — HTTP completeness

**Objective:** talk the way modern servers expect.

#### Tasks

- [ ] Persistent connections (keep-alive) in `libahttp` — the current
      `Connection: close` on every request is the common reason real servers
      behave differently than the test harness.
- [ ] A redirect-to-HTTPS policy (an `http://` URL that 301s to `https://`
      is followed, and `https://` is the default scheme in gbrowser).
- [ ] Request bodies (POST/PUT) behind a bounded interface, for future
      uploads and form submission.
- [ ] **Optional** HTTP/2 h2c/TLS-ALPN — only after X3–X5; it is the least
      useful for a browsing OS that opens one connection at a time.

#### Test gate

- Two sequential requests on one connection reuse the socket (assert via a
  serial log line).
- `http://example.com` that redirects to `https://` is followed exactly once
  and renders.
- A POST with a body reaches the local test server intact.
- If HTTP/2 is attempted: an h2 handshake over the existing TLS ALPN is
  recorded as a manual result; the gate does not depend on it.

#### Deliverable

`patches/REAL_X6_http.patch`

---

### Phase X7 — IPv6 (delivers `INTERNET_PLAN` N8)

**Objective:** reach v6-only hosts, which the 2026 internet increasingly is.

This is the largest, lowest-frequency phase — `INTERNET_PLAN` N8 already
said so, and `MATURITY_PLAN` M14 tracks it here. It is kept because "real
internet access" eventually means v6.

#### Tasks

- [ ] Second address family through every layer: NDP, SLAAC, ICMPv6, a v6
      socket type, and a choice of family by DNS result (X3's AAAA records
      become meaningful here).
- [ ] Dual-stack: a host with both records is reached by whichever works
      (happy-eyeballs-lite, bounded).

#### Test gate

- `ping6` to a link-local address.
- An HTTPS fetch over IPv6 against a local v6 server (deterministic) and a
  real v6 host (manual, recorded).
- IPv4 traffic unchanged.

#### Deliverable

`patches/REAL_X7_ipv6.patch`

---

### Phase X8 — Trust-store lifecycle

**Objective:** keep working as roots rotate in the 90-day, automated world.

#### Tasks

- [ ] Decide and implement one of: (a) a small, signed, in-image trust-store
      update fetched over the very TLS it protects (bootstrap trust problem),
      or (b) a documented rebuild-and-reship process with a dated provenance
      file. Prefer (b) for a hobby OS; (a) is a trust loop.
- [ ] Make the shipped roots' expiry dates visible (a `sysinfo` line and a
      `docs` table), so "chain moved to a root we don't carry" reads as a
      diagnosable trust-store issue, not a TLS bug.
- [ ] Record the OCSP / CRL / Certificate-Transparency decision in the doc
      (excluded, like INTERNET_PLAN: revocation checking is a networked
      protocol of its own).

#### Test gate

- The trust-store provenance file lists every root, its expiry, and where it
  came from.
- A chain to a *currently untrusted* root is refused with a message that says
  "root not in trust store", not a generic handshake failure.
- The decision (a) vs (b) is written down and the chosen path works end to end.

#### Deliverable

`patches/REAL_X8_trust.patch`

---

### Phase X9 — Fit, memory, and an honest statement

**Objective:** the whole stack fits and the claims are true.

#### Tasks

- [ ] Measure `gbrowser + libatls + libahttp + libauragui` against
      `SPAWN_MAX_IMAGE` (1 MiB, kernel/proc/process.c:756) and the user heap;
      split the image or raise the limit with a diagnosed refusal if it
      overflows (same lesson as the /gltest bug the comment describes).
- [ ] Re-verify the guest stack story: `USER_STACK_SIZE` is 1 MiB; confirm no
      TLS path approaches it and say so in `tls.md`.
- [ ] Update `INTERNET_PLAN.md` (N8/N9 status), `tls.md`, `WEBVIEW_PLAN.md` D6,
      `docs/status.md`, and this document's status table in the same commit as
      the code that makes the new claims true.

#### Test gate

- The full browser boots, loads an HTTPS page, and `sysinfo` shows the image
  and stack numbers match what the docs say.
- Every limitation in `tls.md` that this plan changes is updated; a doc grep
  finds no stale "64 KiB" or "HTTPS is not supported" claim.

#### Deliverable

`patches/REAL_X9_fit.patch`

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| X1 | ECDSA is ~half of the 2026 web; without it real HTTPS aborts before anything else can matter |
| X2 | ✅ Done — libahttp TLS transport + full chain validation in the TLS handshake, /http ported, PEM trust store shipped; real-cloudflare interop is a recorded follow-up |
| X3 | Lookups must survive the network before browsers are usable |
| X4 | Reassembly unblocks the transport X5 builds on |
| X5 | A browser that hangs under congestion is broken at the lowest layer |
| X6 | Keep-alive/redirects are what real servers demand once the connection works |
| X7 | Largest effort, lowest frequency; legitimate to defer, kept for completeness (N8/M14) |
| X8 | The trust store rots on a schedule; handle it before it silently breaks sites |
| X9 | The plan must fit and say so; truth in docs is a deliverable (D5) |

**If only one phase is ever built, build X1.** ECDSA verification is the
single change that takes this stack from "works against a certificate we made"
to "can verify most of the real internet's certificates".

---

## 5. Risks

**Writing your own ECDSA is writing your own bignum again.** P-256 is the
most widely implemented curve, so the failure modes are known, but the scalar
multiply and point-format edge cases are subtle (the Wycheproof corpus for
P-256 is large). X1's gate is hostile-negative, not just positive, and the
exact-reason assertion pattern from `test_atls_ed25519` is reused.

**The stack is unaudited hobby cryptography.** This plan adds a curve, it
does not add an audit. X9's statement — like `tls.md` §3.1 — must say the TLS
layer is not reviewed, not imply it is safe.

**The trust store will rot.** X8 makes the expiry visible and the choice
explicit; it does not make the image self-healing. The failure will still
look like a TLS bug to a user; the phase exists to make it diagnosable.

**`SPAWN_MAX_IMAGE` (1 MiB).** Browser + TLS + layout may not fit. The failure
must be a diagnosed refusal (X9), not a truncated-load bug — the exact
incident the comment at process.c:750 describes.

**Real-host testing is non-deterministic.** D6 keeps the CI gate local and
records the real-internet run as documentation, so a flaky network can never
fail a commit.

**IPv6 is expensive for its payoff.** X7 is retained because "real internet"
will mean v6, but it is the most defensible phase to drop if effort runs out.

---

## 6. What this plan does not do

- **No JavaScript, images, proportional fonts, or CSS beyond the named
  subset.** That is `WEBVIEW_PLAN`'s work (D5/D7); this plan stops at verified
  bytes reaching the renderer (D3).
- **No TLS 1.2, session resumption, 0-RTT, or client certificates.** Inherited
  from `INTERNET_PLAN` D3/D6; not reopened.
- **No OCSP, CRL, or Certificate Transparency.** Revocation checking is a
  networked protocol of its own (INTERNET_PLAN §6); X8 records the exclusion
  rather than shipping a half client.
- **No system-wide trust-store management.** Roots stay in-image; X8 makes
  the rotation story explicit, it does not add a runtime CA manager.
- **No HTTP/2 as a gate.** Optional in X6; a single-connection browsing OS
  gains little from it.
- **No VPN, no QUIC/HTTP/3, no proxy support.** Out of scope for a hobby OS
  today.
- **No claim of security.** X9 says precisely what the stack protects against
  and what it does not, in `docs/tls.md`, and says it is not audited.

---

*This document is a companion to `INTERNET_PLAN.md` (the TLS foundation),
`WEBVIEW_PLAN.md` (rendering), and `MATURITY_PLAN.md` (M6/M7 network
hardening, M14 IPv6). Every phase updates the matching document in the same
commit (D5) so the plan and the tree cannot drift apart.*
