# AuraLite OS — TLS Implementation Documentation

**Last updated:** 2026-09-05 (REALINTERNET2_PLAN Y5–Y7: ML-KEM-768 + X25519MLKEM768 hybrid ClientHello, SHA3/SHAKE, live-web protocol)

This document describes the TLS stack implemented in AuraLite OS, its
capabilities, limitations, and security properties.  It exists because an
honest statement about what this protects against — and what it does not —
is more valuable than a padlock icon.

---

## 1. What is implemented

### 1.1 Protocol

| Component | Status | Standard |
|---|---|---|
| TLS 1.3 handshake | ✅ | RFC 8446 |
| Full chain validation in handshake | ✅ when a trust store is set (REALINTERNET_PLAN X2) | RFC 5280 |
| TLS 1.2 and earlier | ❌ Refused | Decision D3 |
| Cipher suite | TLS_CHACHA20_POLY1305_SHA256 only | RFC 8446, D4 |
| Key exchange | X25519, **X25519MLKEM768 hybrid** (group `0x11EC`, offered first; falls back to X25519) | RFC 7748; hybrid per draft-ietf-tls-hybrid-design (REALINTERNET2 Y6) |
| Certificate signatures | Ed25519, RSA PKCS#1v1.5-SHA256, **RSA-PSS-SHA256** (`0x0804`), **ECDSA P-256** | RFC 8032, RFC 8017, RFC 6979, RFC 8446 §4.2.3 |
| ECDSA P-256 | ✅ Verify only (REALINTERNET_PLAN X1) | secp256r1, DER ECDSA-Sig-Value |
| Client certificates | ❌ | D6 |
| Session resumption | ❌ | D6 |
| 0-RTT | ❌ | D6 |
| KeyUpdate | ✅ | RFC 8446 §4.6.3 |
| Alerts | ✅ Send and receive | RFC 8446 §6 |
| close_notify | ✅ | |

### 1.2 Cryptographic primitives (`libatls`)

| Primitive | Standard | Test vectors |
|---|---|---|
| SHA-256 | FIPS 180-4 | NIST |
| SHA-512 | FIPS 180-4 | NIST |
| HMAC-SHA256 | RFC 2104 | RFC 4231 (7 test cases) |
| HKDF-SHA256 | RFC 5869 | RFC 5869 (3 test cases) |
| ChaCha20 | RFC 8439 §2.4 | RFC 8439 §2.4.2 |
| Poly1305 | RFC 8439 §2.5 | RFC 8439 §2.5.2 |
| AEAD_CHACHA20_POLY1305 | RFC 8439 §2.8 | RFC 8439 §2.8.2 |
| X25519 | RFC 7748 | RFC 7748 §5.2 + §6.1 (1000 iterations) + Wycheproof low-order |
| SHA3-256 / SHA3-512 | FIPS 202 | NIST SHA3 vectors (Y5) |
| SHAKE128 / SHAKE256 | FIPS 202 | NIST SHAKE vectors (Y5) |
| ML-KEM-768 (decap only) | FIPS 203 | FIPS 203 KATs + ACVP vectors (Y5) |
| Ed25519 (verify only) | RFC 8032 | RFC 8032 §7.1 TEST 1–3 + SHA(abc) |
| RSA PKCS#1v1.5 (verify only) | RFC 8017 | Self-signed certificate chain |
| RSA-PSS-SHA256 (verify only) | RFC 8017 §9.1.2 | EMSA-PSS-VERIFY vectors (0x0804) |
| Constant-time comparison | — | Unit test asserts behavior |
| CSPRNG (kernel) | ChaCha20 DRBG | RFC 8439 block vectors + statistical |

### 1.3 Certificate handling

| Feature | Status |
|---|---|
| X.509 v3 parsing | ✅ (DER, zero-allocation) |
| Chain building | ✅ (issuer DER byte-equality) |
| Ed25519 signature verification | ✅ |
| RSA PKCS#1v1.5-SHA256 verification | ✅ |
| ECDSA P-256 (secp256r1) signature verification | ✅ (verify only, REALINTERNET_PLAN X1) |
| Hostname matching (SAN dNSName) | ✅ (exact + single-label wildcard) |
| Validity date checking | ✅ (UTCTime + GeneralizedTime, fail-closed) |
| Basic constraints | ✅ (leaf ≠ CA, CA must have keyCertSign) |
| Key usage | ✅ |
| Unknown critical extensions | ✅ Refused (D5) |
| OCSP / CRL | ❌ Not implemented |
| Certificate Transparency | ❌ Not implemented |

### 1.4 HTTP client (`libahttp`)

| Feature | Status |
|---|---|
| HTTP/1.1 | ✅ |
| Host header | ✅ |
| Chunked transfer encoding | ✅ |
| Content-Length | ✅ |
| Connection: close | ✅ |
| Redirects (301/302/307/308) | ✅ (max 5 hops) |
| Growing response buffer | ✅ (1 MiB cap) |
| HTTPS (over TLS) | ✅ TLS transport implemented (REALINTERNET_PLAN X2) |
| Chain validation in HTTPS | ✅ when a trust store is set (`ahttp_set_trust_roots`); default is CertificateVerify-only |

### 1.5 Entropy source

The kernel CSPRNG (`kernel/rng.c`) is a ChaCha20-based DRBG seeded from:

1. **RDSEED** (CPUID leaf 7, EBX bit 18) — preferred, when available
2. **RDRAND** (CPUID leaf 1, ECX bit 30) — fallback
3. **Interrupt timing jitter** — collected from every IRQ via
   `rng_jitter_event()` in `irq_dispatch()`; requires 128 estimated bits
   of variation before seeding

Until the pool is seeded, `getentropy()` returns `-ENOSYS` and
`getrandom()` blocks.  The estimated entropy is logged at boot.

---

## 2. What this protects against

- **Passive eavesdropping** on a network path between the client and a
  compliant TLS 1.3 server.  The handshake uses X25519 ephemeral keys —
  or the X25519MLKEM768 hybrid when the server accepts it, which also
  resists harvest-now-decrypt-later by a quantum adversary (Y6) — and the
  record layer uses AEAD_CHACHA20_POLY1305.  Both give forward secrecy.

- **Server impersonation** by an attacker who does not possess a valid
  certificate chain rooted in the shipped trust store.  Ed25519, RSA
  PKCS#1v1.5-SHA256, RSA-PSS-SHA256 and ECDSA P-256 chain signatures are
  verified (REALINTERNET X1); chains signed with any other algorithm
  (e.g. ECDSA P-384) are refused with a named `certval` error, not
  silently skipped.

- **Tampered records** — the AEAD tag is verified before any plaintext
  is released; a tampered record causes the connection to abort.

---

## 3. What this does NOT protect against

This is the important section.  Read it before trusting this TLS stack
with anything valuable.

### 3.1 Not audited

This code has never been reviewed by a cryptographer or a security
engineer.  It was written by a hobby OS developer and tested against RFC
test vectors.  RFC vectors verify correctness, not security.  There are
almost certainly bugs that an audit would find.

### 3.2 Side channels

Constant-time discipline (decision D7) is a coding rule: a single
`atls_ct_eq` function is used for all secret comparisons, and the test
suite greps the sources to verify no `memcmp` on secret data.  This is
the limit of what can be verified.  Timing side channels in the
field arithmetic, the bignum exponentiation, or the AEAD construction
have not been measured and cannot be honestly claimed absent.

### 3.3 RSA is not constant-time

The bignum implementation (`atls_rsa.c`) uses 32-bit limbs with
64-bit intermediates.  Modular exponentiation uses binary
square-and-multiply.  This is not constant-time.  RSA is used only
for verification of public signatures, where the inputs are public
data; the risk is lower than for signing or decryption, but it is
not zero (a fault attack could forge a signature if the implementation
leaks through a side channel during verification).

### 3.4 No revocation checking

There is no OCSP client, no CRL parser, and no Certificate Transparency
verification.  A revoked certificate will be accepted until it expires.
The trust store is a pinned, in-image set of root certificates; there
is no mechanism to update it at runtime.

### 3.5 Trust store will rot

The shipped roots (`/etc/ssl/roots.pem`) expire on fixed dates.  An
image built today will eventually fail against sites whose chains move
to a root it does not carry.  Updating requires rebuilding the image.

Root certificates shipped:

| Root | Expires | Notes |
|---|---|---|
| ISRG Root X1 | 2035-06-04 | Let's Encrypt |
| DigiCert Global Root CA | 2038-11-10 | |
| DigiCert Global Root G3 | 2038-01-15 | |

### 3.6 ECDSA leaf certificates

ECDSA P-256 (secp256r1) leaf certificates are now verified
(REALINTERNET_PLAN.md phase X1) — signature verification over the DER
ECDSA-Sig-Value, against the uncompressed `04 || X || Y` point.  ECDSA on
other curves (P-384, P-521) is still refused with
`unsupported_certificate`, not silently accepted.  Like the rest of the
TLS layer, the P-256 arithmetic is not audited and not constant-time (it
runs on public certificate data; see §3.1–§3.3).

### 3.7 User stack size

**Resolved.** The user stack is now **4 MiB** (`USER_STACK_SIZE =
0x400000`, `kernel/proc/guard.c`), with a randomised top and an unmapped
guard page below it. The earlier claim that Ed25519 CertificateVerify
overflowed a *64 KiB* stack is stale twice over: that was true only for the
old 64 KiB default, and the Ed25519 scalar multiplication uses only ~3 KiB
of stack per verification anyway. A full TLS 1.3 handshake with Ed25519,
RSA, or ECDSA P-256 CertificateVerify runs well within the 4 MiB stack;
no TLS path approaches it.

### 3.8 No hostname verification against a CA policy

The hostname matching logic (RFC 6125) verifies that the server's SAN
matches the requested hostname.  There is no CA policy enforcement
beyond "the chain must reach a root in the trust store."  A self-signed
certificate with a matching SAN will be rejected (the chain must
actually verify against a pinned root).

### 3.9 HTTP client limitations

- No HTTP/2 or HTTP/3.
- Persistent connections: **kept alive** (REALINTERNET_PLAN X6). `libahttp`
  caches one live connection per origin, honours `Connection: close` and
  HTTP/1.0, skips 1xx, and reopens a stale socket once for idempotent methods.
- Request bodies: **supported for POST/PUT** (X6), bounded to
  `AHTTP_MAX_REQ_BODY` (64 KiB), `Content-Length`-framed.
- Redirects: **followed** (X6) with RFC 3986 dot-segment resolution, 301/302
  → GET, 307/308 → resend body, max 5 hops, `data:`/`javascript:` refused.
- **Real-world ClientHello interop:** **landed** (REALINTERNET2 Y6). The
  ClientHello offers the hybrid post-quantum group `X25519MLKEM768`
  (`0x11EC`) first and plain X25519 as fallback, so PQ-preferring servers
  (Cloudflare et al.) negotiate the hybrid and the handshake completes.
  What remains at the *certificate* layer: chain signature links outside
  Ed25519 / RSA-PKCS#1v1.5-SHA256 / RSA-PSS-SHA256 / ECDSA P-256 — e.g. a
  chain containing an ECDSA P-384 link (Let's Encrypt's ISRG Root X2 path)
  fails `ATLS_CERTVAL_ERR_UNSUPPORTED` (−26) rather than a mystery
  `ATLS_ERR_PEER_EOF`. `docs/live_web.md` is the paste-back
  protocol for turning any such live failure into a receipt; handshakes
  against a local openssl s_server (the deterministic gate) pass.

### 3.10 Known limitations inherited from the TCP stack

All four original entries in this list have since landed:

- `TCP_MAX_CONNS` is 16 (raised from 8 in REALINTERNET X5), with a sliding
  send window, SACK, congestion control and TIME_WAIT (MATURITY M6,
  REALINTERNET2 Y1).
- IP fragment reassembly exists (REALINTERNET X4) and is required by X.509
  chains that span several records.
- IPv6 exists: SLAAC, `AF_INET6` sockets, AAAA family choice, dual-stack and
  TCP-over-IPv6 (REALINTERNET X7, REALINTERNET2 Y3); HTTPS-over-IPv6 was
  measured in Y4 (RES-26 closed).
- DNS caching exists (`kernel/net/dns.c`, TTL-bounded; `test_dns_cache`).

---

## 4. Test coverage

### 4.1 Host unit tests (run on every commit)

| Test | Checks | Covers |
|---|---|---|
| `test_rng` | 16 | ChaCha20 DRBG, RFC 8439 vectors, statistical (1 MiB) |
| `test_atls_hash` | 32 | SHA-256/512, HMAC, HKDF, D7 memcmp audit |
| `test_atls_aead` | 18 | ChaCha20, Poly1305, AEAD, round-trip, tamper |
| `test_atls_x25519` | 27 | RFC 7748, 1000 iterations, Wycheproof low-order |
| `test_atls_ed25519` | 17 | RFC 8032, negative cases, exact reason asserted |
| `test_atls_x509` | 61 | Real certs, truncation sweep, mutations, depth gate |
| `test_atls_tls` | 25 | Handshake vs openssl, KeyUpdate, large transfer, Finished MAC |
| `test_atls_certval` | 17 | Chain building, RSA+Ed25519+ECDSA, hostname, dates, constraints |
| `test_atls_ecdsa` | 10 | ECDSA P-256 direct verify + hostile negatives (X1) |
| `test_atls_pem` | 10 | PEM trust-store decoding (X2) |
| `test_ahttp_https` | 5 | HTTPS client end-to-end vs openssl, chain validation on/off (X2) |
| `test_ahttp` | 7 | URL parsing |

### 4.2 QEMU integration tests

| Test | Assertions | Covers |
|---|---|---|
| `test_rng` | 14 | Entropy: RDRAND detection, jitter fallback, boot sample |
| `test_crypto` | 14 | libatls in-guest smoke test |
| `test_x509` | 14 | X.509 parser in-guest |
| `test_tls` | 14 | TLS handshake in-guest vs openssl s_server |

---

## 5. Architecture decisions reference

| ID | Decision | Rationale |
|---|---|---|
| D1 | Entropy first | Crypto on guessable seed is theatre |
| D2 | Crypto in userspace | ASN.1 parser bug → killed process, not kernel panic |
| D3 | TLS 1.3 only | Smaller, no legacy vuln classes |
| D4 | One suite, one exchange, one sig | ChaCha20 (fast in C), X25519, Ed25519+RSA |
| D5 | Validation not optional | Plaintext-with-extra-steps is worse than plaintext |
| D6 | No client certs/resumption/0-RTT | No payoff for public-page browsing |
| D7 | Constant-time where it matters | audited comparison function, grep-enforced |

---

## 6. File layout

```
lib/libatls/
  include/atls/
    atls.h          — public crypto API
    tls.h           — TLS client API
    x509.h          — X.509 parser API
    certval.h       — certificate validation API
  src/
    atls_common.c   — constant-time comparison, wipe
    atls_sha256.c   — SHA-256
    atls_sha512.c   — SHA-512
    atls_hmac.c     — HMAC-SHA256
    atls_hkdf.c     — HKDF-SHA256
    atls_chacha20.c — ChaCha20
    atls_poly1305.c — Poly1305
    atls_aead.c     — AEAD_CHACHA20_POLY1305
    atls_fe.c       — field arithmetic (2^255-19)
    atls_x25519.c   — X25519
    atls_ed25519.c  — Ed25519 verification
    atls_rsa.c      — RSA PKCS#1v1.5 verification (bignum)
    atls_der.c      — DER TLV reader
    atls_x509.c     — X.509 v3 parser
    atls_certval.c  — certificate chain validation
    atls_tls_keys.c — TLS key schedule + record crypto
    atls_tls.c      — TLS 1.3 client state machine

lib/libahttp/
  include/ahttp/http.h — HTTP client API
  src/ahttp.c          — HTTP/1.1 client

kernel/
  rng.c             — ChaCha20 CSPRNG (N0)
  net/tcp.c         — TCP (N7 fixes)

etc/ssl/roots.pem   — pinned trust store

tests/unit/
  test_rng.c, test_atls_*.c, test_ahttp.c

tests/integration/cases/
  test_rng.sh, test_crypto.sh, test_x509.sh, test_tls.sh
```

---

## 6.5 The security statement (REALINTERNET_PLAN X9 / INTERNET_PLAN N9)

Read this before you trust this stack with anything.

- **This is not audited.** No independent review of the TLS layer, the
  cryptographic primitives in `libatls`, or the certificate validation has
  been performed. It is hobby/educational code.
- **No side-channel review beyond D7.** Constant-time behaviour is enforced
  where secrets are compared (no `memcmp` on secret material; a source grep
  enforces this per rule D7), but there is no cache-timing, power-analysis or
  microarchitectural review. P-256 arithmetic runs on public certificate
  data and is **not** constant-time.
- **Do not use this to protect anything valuable.** There is no key storage
  on the OS, no secure enclave, no authenticated boot, and the kernel and
  userspace are themselves a hobby kernel.
- **What it protects against (and only that):** an in-band network attacker
  who cannot break the cryptography cannot read or modify TLS 1.3 traffic to
  a host whose chain verifies against the shipped trust store. Chain
  validation (real-world roots, hostname matching, validity dates, basic
  constraints, key usage, ECDSA/Ed25519/RSA signatures) is implemented and
  tested.
- **What it does not protect against:** an attacker with a root in the trust
  store, a compromised CA, a misissued certificate (no OCSP/CRL/CT — see
  `docs/trust_store.md` §5), a broken clock (validity checking depends on the
  OS clock), a compromised host/kernel, or side channels.

## 7. What comes next

The remaining phases from `INTERNET_PLAN.md`:

- **N8 (IPv6):** delivered as the first landing by `REALINTERNET_PLAN` X7
  (link-local + NDP + ICMPv6 echo + `ping6`); the recorded follow-ups —
  SLAAC, the AF_INET6 socket family, AAAA-record family choice, dual-stack
  and TCP-over-IPv6 — all landed in `REALINTERNET2_PLAN` Y3 (RES-26 closed
  with the Y4 HTTPS-over-IPv6 measurement).
- **X9 (fit & docs):** complete — measured `gbrowser + libatls + libahttp +
  libauragui` at 380,904 bytes against `SPAWN_MAX_IMAGE` (since raised to
  16 MiB); the 4 MiB user stack has ample headroom; docs updated (see
  `docs/trust_store.md`, `docs/status.md`, `docs/plans/WEBVIEW_PLAN.md` D6).
- **Known gaps that would strengthen the stack:**
  - OCSP stapling / CRL checking (recorded exclusion — `docs/trust_store.md` §5)
  - HTTP/2 support
  - ECDSA P-384 (and other non-P-256 curve) chain-signature links in
    `certval` — the live boundary named in §3.9

---

*This document is part of INTERNET_PLAN.md phase N9, maintained through
REALINTERNET_PLAN X9 and REALINTERNET2_PLAN Y5–Y7.  Every limitation
listed here is a known gap, not a future feature.  If something is not
listed, it is either implemented or an oversight — file an issue.*
