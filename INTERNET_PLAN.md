# AuraLite OS — Real Internet Access Plan

## Status: IN PROGRESS 🚧 — N0 complete, N1–N9 pending

| Phase | Result | Deliverable |
|-------|--------|-------------|
| N0 — real entropy source | ✅ complete | `patches/NET_N0_entropy.patch` |
| N1–N9 | pending | — |

This document answers:

> *What is missing before AuraLite can reach the actual internet, rather than
> a plain-HTTP server on the same subnet?*

One answer dominates all the others: **there is no TLS, and no cryptography of
any kind in the tree.** Everything else here is refinement; that one is the
difference between a network stack and internet access.

It follows the structure of the existing plans (`GL_PLAN.md`,
`FSLAYOUT_PLAN.md`, `SDK_PLAN.md`, `WEBVIEW_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, one `.patch` per
phase.

**Baseline:** commit `f842634`. Companion to `WEBVIEW_PLAN.md`, whose decision
D6 defers HTTPS to exactly this document.

The two are independent: a web view without TLS renders local and plain-HTTP
pages, and TLS without a web view still gives `/apps/http` and any SDK
application a way to reach an HTTPS host. Built together they give a browser
that can reach the public web; built separately, each is still worth having.

---

## 1. Where things actually stand

Measured against the tree, not assumed.

### What works today

| Piece | State | Evidence |
|---|---|---|
| Ethernet (e1000, virtio-net) | ✅ | `test_e1000_irq`, `test_virtio_net` |
| ARP, IPv4, ICMP | ✅ | `ping` works |
| DHCP (discover/offer/request/ack, DNS option) | ✅ | `kernel/net/net.c` |
| UDP + sockets | ✅ | `test_udp_sockets` |
| TCP with sliding window, cwnd, RTO retransmit | ✅ | `test_tcp_server`, `test_http_get` |
| DNS resolver | ✅ | `nslookup` |
| BSD-ish socket API | ✅ | `kernel/net/socket.h` |
| `getentropy()` syscall | ⚠️ | present — see §1.3, it is not random |

This is a genuinely working stack. `test_http_get` fetches a real page over
the real internet. The gap is not "networking"; it is everything above it.

### What is missing

| Missing | Consequence |
|---|---|
| **All cryptography** | No TLS, so no HTTPS |
| **TLS** | ~95% of the web is unreachable |
| Certificate parsing + trust store | Even with TLS, no way to know *who* answered |
| HTTP/1.1 (chunked, keep-alive) | Servers increasingly refuse 1.0 |
| IP fragment reassembly | `flags_frag` is written as 0 and never read |
| IPv6 | Some hosts are v6-only |
| TCP connection limit | `TCP_MAX_CONNS` is **8** |
| DNS caching | Every lookup is a fresh query |

A grep for `sha256|aes|curve25519|rsa|hmac` across the tree finds **nothing**
outside comments — `kernel/fs/btrfs.c` even writes its SHA-256 checksum field
as zeros and says so.

### 1.3 The entropy source is not random, and TLS would be worthless on it

`getentropy()` exists, and this is what it returns
(`kernel/arch/x86_64/syscall.c`, `case 318`):

```c
uint64_t rnd = tsc ^ timer_get_ticks() ^ (uint64_t)(uintptr_t)out
             ^ ((uint64_t)i * 6364136223846793005ULL + 1442695040888963407ULL);
```

A timestamp counter, a tick count, a stack address and a fixed LCG constant.
Every input is either observable or guessable by anyone who knows roughly when
the machine booted.

**A TLS implementation seeded from this is not secure against anybody.** The
private key of an X25519 handshake is derived from it, and an attacker who can
narrow the boot time to a few million TSC ticks can enumerate the keyspace.

This is why phase **N0 is the entropy source**, before a single line of
crypto. Building TLS on top of the current one would produce something that
*looks* like HTTPS in the address bar and protects nothing, which is worse
than having no HTTPS at all — the padlock is a claim, and a false claim is a
liability.

### 1.4 The cost of cryptography, measured

Whether TLS is a phase or its own multi-year project depends on how slow this
is in plain C. Measured on this machine, portable C, `-O2`:

| Operation | Cost |
|---|---|
| SHA-256 | **205 MB/s** (0.31 µs per 64-byte block) |
| Curve25519 field multiply | **0.059 µs** |
| X25519 scalar multiplication (255 ladder steps) | **~0.2 ms** |
| A full handshake's two scalar multiplications | **~0.3 ms** |

A sub-millisecond handshake and 205 MB/s hashing mean the arithmetic is not
the problem. **TLS here is an engineering-effort problem, not a performance
problem** — which is the answer that makes this plan worth writing.

(These are host numbers. The kernel builds with `-mno-sse`, and userspace
does not, so the crypto belongs in userspace — see D2.)

---

## 2. Decisions

### D1. Entropy first, and it gates everything

No cryptographic code lands before N0. A CSPRNG seeded from RDRAND where
available, from a mix of genuinely unpredictable sources otherwise, and an
explicit **refusal to run TLS at all** when neither is available. A loud
failure beats a quiet fake.

### D2. Crypto and TLS live in userspace, not the kernel

The kernel builds `-mno-sse -mno-mmx` (this is why libgl is user-space, per
`GL_PLAN.md`). More importantly, a bug in an ASN.1 parser should not be a
kernel bug: certificate parsing is the single most attacked surface in a TLS
stack, and it should run in a process the kernel can kill.

`libatls` is a userspace library, linked like `libaurac`. The socket layer
stays exactly as it is.

### D3. TLS 1.3 only. No TLS 1.2, no SSLv3, no downgrade path

TLS 1.3 is *smaller* than 1.2: one handshake shape, AEAD-only, no
renegotiation, no compression, no CBC padding oracles, no RSA key transport.
Supporting 1.2 as well would roughly double the work and add every legacy
vulnerability class back.

A server that cannot do 1.3 is a server this OS does not talk to. In 2026
that is a small and shrinking set.

### D4. One cipher suite, one key exchange, one signature algorithm

- `TLS_CHACHA20_POLY1305_SHA256` for the record layer
- X25519 for key agreement
- Ed25519 **and** RSA-PKCS#1v1.5-SHA256 for certificate signatures

ChaCha20-Poly1305 over AES-GCM because it is fast in portable C without
AES-NI, and constant-time by construction — AES in C is either slow or
table-driven and cache-timing-leaky.

RSA verification is included reluctantly: most of the web's certificate chains
still have an RSA root, so Ed25519-only would fail on real sites. Verification
only — no RSA key generation, no signing, no decryption.

### D5. Certificate validation is not optional, and not a stretch goal

A TLS stack that skips validation is a plaintext connection with extra steps.
Chain building, signature verification, validity dates, hostname matching
against SAN — all of it in phase N5, before anything can call itself HTTPS.

The trust store is a **pinned, in-image set of root certificates** shipped in
`/etc/ssl`. Not a system store, not user-editable at runtime. Small, auditable,
and updated by rebuilding the image.

### D6. No client certificates, no session resumption, no 0-RTT

Each is real work with no payoff for a browser fetching public pages. 0-RTT in
particular has replay semantics that are subtle to get right and dangerous to
get wrong.

### D7. Constant-time where it matters, and tested for it

Comparisons of MACs and secrets use a constant-time primitive, and the tests
assert that primitive's behaviour. Timing side channels cannot be tested
directly in QEMU with any credibility, so the plan does the thing that *can*
be verified: a single audited comparison function, used everywhere, with no
`memcmp` on secret data anywhere in `libatls`.

---

## 3. Phases

### Phase N0 — A real entropy source ✅ COMPLETE

**Objective:** `getentropy()` that is not guessable.

#### Tasks

- [x] Detect RDRAND and RDSEED via CPUID leaf 1 ECX bit 30 / leaf 7 EBX bit 18.
      (Verified: this QEMU accepts `-cpu qemu64,+rdrand`, so it is testable.)
- [x] A ChaCha20-based CSPRNG in the kernel, reseeded from RDSEED when present.
- [x] Fallback entropy pool: interrupt timing jitter, not just TSC reads.
- [x] **`getentropy()` returns `-ENOSYS` when neither hardware RNG nor a
      sufficiently stirred pool is available.** Callers must be able to tell.
- [x] Record the estimated entropy at boot, so a weak source is visible in the
      log rather than silently accepted.

#### Test gate

- Statistical smoke tests on the output: byte-frequency and bit-run checks
  over 1 MB, which catch a stuck or counter-like generator. Not a claim of
  cryptographic quality — a claim that the obvious failures are absent.
- Two boots produce different output (the current implementation does not
  reliably manage even this).
- With `-cpu qemu64,+rdrand`, RDRAND is detected and used.
- Without it, the fallback runs and the log says so.

#### Deliverable

`patches/NET_N0_entropy.patch`

#### Phase result (2026-08-06)

The Q16 xorshift128+ pool is gone.  `kernel/rng_core.h` is a freestanding
ChaCha20 DRBG core (RFC 8439 block function, key-after-every-request
backtracking resistance, XOR-fold reseed), unit-tested on the host against
the RFC 8439 §2.3.2/§2.4.2 vectors plus a 1 MiB frequency/run battery
(`tests/unit/test_rng.c`, 16 checks).  `kernel/rng.c` feeds it: RDSEED or
RDRAND when CPUID reports them (384 bits, bounded retry), otherwise an
interrupt-timing jitter pool stirred from every IRQ via `rng_jitter_event()`
in `irq_dispatch()`.  Until the pool's measured variation reaches 128
estimated bits, `rng_try_fill()` returns `-ENOSYS` and `getrandom()` blocks
(`GRND_NONBLOCK` → `EAGAIN`); the estimate is logged at boot
(`[rng] pool: N samples, est. E bits`).  A 16 KiB in-kernel self-test
(byte-frequency + bit-runs) and a 32-byte `[rng] sample:` line are printed
on seeding.  Measured in QEMU: `qemu64,+rdrand,+rdseed` boots seed from
RDSEED; plain `qemu64` seeds from the jitter pool (~4 bits of observed
delta variation per IRQ; 197 samples / est. 780 bits at seed time in the
reference run), and two boots produce different samples.  Integration gate:
`tests/integration/cases/test_rng.sh` (14 assertions), gated through
`tests/integration/run_all.sh`.

---

### Phase N1 — Cryptographic primitives

**Objective:** the algorithms, standing alone and heavily tested.

#### Tasks

- [ ] SHA-256, HMAC-SHA256, HKDF (RFC 5869).
- [ ] ChaCha20 and Poly1305, and the AEAD construction (RFC 8439).
- [ ] Curve25519 / X25519 (RFC 7748).
- [ ] Ed25519 verification (RFC 8032).
- [ ] A constant-time comparison, and a rule that nothing else compares
      secrets (D7).
- [ ] `libatls` as a userspace static library, shipped in the SDK.

#### Test gate

- **Every algorithm against its RFC test vectors.** These are published,
  unambiguous, and the only honest way to know an implementation is correct;
  "it produces consistent output" proves nothing.
- X25519 against the RFC 7748 iterated test (1 000 iterations).
- Wycheproof edge cases where they are small enough to embed: low-order
  points, all-zero shared secrets, malformed signatures.
- A rejected signature must be rejected for the right reason, checked
  individually rather than as a lump.

#### Deliverable

`patches/NET_N1_crypto.patch`

---

### Phase N2 — ASN.1 and X.509 parsing

**Objective:** read a certificate without being read by it.

This is the most dangerous code in the plan. It parses attacker-controlled,
deeply nested, length-prefixed binary from a stranger.

#### Tasks

- [ ] A DER reader with **explicit depth and length limits** — the 64 KB user
      stack (`USER_STACK_SIZE`) makes a recursive descent parser a real
      overflow risk, exactly as `WEBVIEW_PLAN` §2 and GL phase G11b describe.
- [ ] X.509 v3: subject, issuer, validity, SAN, basic constraints, key usage,
      signature algorithm and value, SPKI.
- [ ] Reject rather than interpret: unknown critical extensions are fatal.

#### Test gate

- Real certificates from real sites parse correctly.
- A fuzz corpus of mutated certificates: **no crash, no hang, bounded memory,
  bounded stack**, asserted in QEMU where the 64 KB limit is real.
- Truncated, over-long, and self-referential structures are all refused.
- A certificate with a 10 000-deep nesting is refused, not followed.

#### Deliverable

`patches/NET_N2_x509.patch`

---

### Phase N3 — The TLS 1.3 handshake

**Objective:** get to application data with a server that is who it claims.

#### Tasks

- [ ] ClientHello with the D4 suite, key share, SNI, ALPN (`http/1.1`).
- [ ] ServerHello, key schedule (RFC 8446 §7.1), transcript hash.
- [ ] EncryptedExtensions, Certificate, CertificateVerify, Finished.
- [ ] HelloRetryRequest handled — some servers use it.
- [ ] Alerts sent and understood, and a `close_notify` on shutdown.

#### Test gate

- A handshake against a local `openssl s_server` in the integration harness,
  so the test does not depend on the internet.
- Interop against at least three real public hosts, run manually and recorded
  — a test that needs the network cannot gate CI, but the result belongs in
  the docs.
- A tampered Finished, a wrong transcript, and a downgrade attempt are each
  refused with the correct alert.

#### Deliverable

`patches/NET_N3_tls_handshake.patch`

---

### Phase N4 — The TLS record layer

**Objective:** move bytes, correctly, with the awkward cases handled.

#### Tasks

- [ ] Record encryption/decryption, sequence numbers, nonce construction.
- [ ] Fragmentation and coalescing: a TLS record can span TCP segments, and
      several records can arrive in one.
- [ ] Key update.
- [ ] The record-size limit, and a refusal past it rather than a large
      allocation on a stranger's say-so.

#### Test gate

- A 10 MB transfer arrives byte-identical.
- A record split across three TCP segments is reassembled.
- Three records in one segment are all processed.
- A record claiming an absurd length is refused without allocating it.

#### Deliverable

`patches/NET_N4_tls_record.patch`

---

### Phase N5 — Certificate validation

**Objective:** the phase that makes the padlock mean something (D5).

#### Tasks

- [ ] Chain building from the server's list to a pinned root.
- [ ] Signature verification at each link (Ed25519 and RSA-PKCS#1v1.5).
- [ ] Validity dates — which requires the clock to be right, so N5 also
      verifies the RTC is sane and **fails closed** if it is not.
- [ ] Hostname matching against SAN dNSName, with wildcards limited to a
      single leftmost label.
- [ ] Basic constraints: a leaf may not sign, a CA must be marked as one.
- [ ] `/etc/ssl/roots.pem` shipped in the image, with a documented provenance.

#### Test gate

- A valid chain to a pinned root is accepted.
- **Each of these is refused, individually:** expired certificate, wrong
  hostname, self-signed, chain to an unknown root, leaf used as a CA, valid
  chain with one signature byte flipped.
- Refusal is by default: an unreachable code path in validation must fail
  closed, and the test asserts the specific reason so a wrong-reason pass is
  visible.

#### Deliverable

`patches/NET_N5_cert_validation.patch`

---

### Phase N6 — HTTPS client and `libahttp`

**Objective:** a usable client, and the thing `WEBVIEW_PLAN` D6 waits for.

#### Tasks

- [ ] HTTP/1.1: `Host`, chunked transfer decoding, `Content-Length`,
      keep-alive, redirects (bounded, with a loop check).
- [ ] `libahttp` over `libatls`, one `http_get(url)` that handles both
      schemes.
- [ ] Port `/apps/http`, `/apps/browser` and `/apps/gbrowser` onto it.
- [ ] A growing response buffer with an explicit cap, replacing the 16 KB
      static one in `gbrowser`.

#### Test gate

- `https://` against a local TLS server in the harness.
- A chunked response decodes identically to an unchunked one.
- A redirect loop terminates with a diagnosis rather than hanging.
- The existing `test_http_get` still passes unchanged.

#### Deliverable

`patches/NET_N6_https_client.patch`

---

### Phase N7 — Stack robustness

**Objective:** fix what the real internet exposes that a local server does not.

#### Tasks

- [ ] IP fragment reassembly, with a reassembly timeout and a memory cap
      (`flags_frag` is currently written as 0 and never read).
- [ ] Raise `TCP_MAX_CONNS` from 8, or document why 8 is enough.
- [ ] A DNS cache honouring TTL, plus retry against the secondary server.
- [ ] Path MTU black-hole tolerance.

#### Test gate

- A fragmented UDP datagram is reassembled; an incomplete one is dropped after
  the timeout rather than held forever.
- Overlapping-fragment attacks are refused.
- Eight simultaneous connections work; the ninth fails cleanly.

#### Deliverable

`patches/NET_N7_stack_hardening.patch`

---

### Phase N8 — IPv6 (optional, and last for a reason)

**Objective:** reach v6-only hosts.

Last because it is the largest phase with the smallest immediate payoff: dual
stack, NDP, SLAAC, ICMPv6, and a second address family through every layer.
Deferring it is a legitimate outcome.

#### Test gate

- `ping6` to a link-local address.
- An HTTPS fetch over IPv6.
- A dual-stack host is reached by whichever family works.

#### Deliverable

`patches/NET_N8_ipv6.patch`

---

### Phase N9 — Documentation and an honest security statement

**Objective:** say precisely what this protects against, and what it does not.

#### Tasks

- [ ] `docs/tls.md`: the implemented subset, the trust store's provenance, the
      known gaps.
- [ ] An explicit statement that this is **not audited**, has no side-channel
      review beyond D7, and should not protect anything valuable.
- [ ] Update `WEBVIEW_PLAN.md` D6, which points here.

#### Test gate

- Every limitation in this document appears in `docs/tls.md`.

#### Deliverable

`patches/NET_N9_docs.patch`

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| N0 | Crypto on a guessable seed is theatre; nothing may precede it |
| N1 | Primitives, verified against published vectors, before any protocol |
| N2 | Certificates must be parsed before they can be validated |
| N3 | The handshake needs primitives and a parsed certificate |
| N4 | Records need a completed handshake's keys |
| N5 | Validation needs a working handshake to validate within |
| N6 | The client is the payoff, and needs all of the above |
| N7 | Robustness matters once real hosts are being reached |
| N8 | Largest effort, smallest payoff; legitimate to skip |
| N9 | The claims can only be written once the code exists |

**If only two phases are ever built, build N0 and N1.** A real entropy source
fixes an existing latent defect that already affects anything using
`getentropy()`, and verified primitives are useful on their own — the package
manager's CRC-32 could become a real hash, for one.

---

## 5. Risks

**Writing your own TLS is famously a bad idea.** It is being done here because
the alternative is no HTTPS at all, and because a hobby OS has no way to
consume OpenSSL. That is a reason, not a defence: this stack will have bugs
that a reviewed implementation would not, and N9 has to say so in the
documentation rather than implying a security guarantee.

**Side channels cannot be honestly tested here.** Constant-time discipline
(D7) is a coding rule, and the plan verifies the rule is followed rather than
claiming the result is timing-safe. A QEMU-based timing measurement would be
noise and would be worse than admitting the limit.

**The trust store will rot.** Pinned roots expire. An image built today will
fail against sites whose chains move to a root it does not carry, and the
failure will look like a bug in the TLS stack. N9 must document the expiry
dates of what is shipped.

**The 64 KB stack, again.** The X.509 parser is the third place in this
codebase where recursion over attacker-supplied structure meets a small stack.
GL phase G11b and `WEBVIEW_PLAN` W2 are the precedents; N2's gate puts the
test in QEMU for that reason.

**`SPAWN_MAX_IMAGE` is 1 MB.** `libatls` plus a browser plus a layout engine
may not fit. The failure is a diagnosed refusal at spawn time, but it is a
ceiling that N6 and `WEBVIEW_PLAN` share and neither can see alone.

**RSA is a compromise.** Included only because real chains need it (D4).
Verification-only limits the exposure, but a bignum implementation is
error-prone and this one will not be constant-time in any meaningful sense.
It verifies public signatures, where that matters least.

---

## 6. What this plan does not do

- **No TLS 1.2 or earlier** (D3).
- **No client certificates, session resumption or 0-RTT** (D6).
- **No AES.** ChaCha20 only, for the reasons in D4.
- **No RSA signing, key generation or decryption.** Verification only.
- **No OCSP, no CRL, no certificate transparency.** Revocation checking is a
  networked protocol of its own; the plan ships without it and says so.
- **No system-wide trust store management.** Roots are pinned in the image.
- **No claim of security.** It is unaudited hobby cryptography.
