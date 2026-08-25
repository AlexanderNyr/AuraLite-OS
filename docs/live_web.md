# Live-web protocol — REALINTERNET2 Y7 (metal_receipts style)

D5: this is a **user-run** protocol, not a CI gate.  CI stays on
`openssl s_server -groups X25519MLKEM768` (Y6).  `pending-user` is
a status, not a failure.

The X9 sentence this series picked up: a fetch against Cloudflare
ended in `ATLS_ERR_PEER_EOF` because the peer wanted
`X25519MLKEM768` and the ClientHello offered only X25519.  Y6
closed that gap in CI.  This file is how a person proves it on
the public web.

## How to run it

Boot `release/auralite.iso` (QEMU user-net or metal with a working
NIC + DNS).  At the shell:

```
run http https://www.ietf.org/
```

`/http` loads `/etc/ssl/roots.pem` (seventeen public roots + the
GTS WE2 pin; see [`trust_store.md`](trust_store.md)) and talks TLS 1.3 with
ChaCha20-Poly1305 and the Y6 ClientHello (0x11EC first).

## The receipt slots

Paste the line(s) back with the machine named.

| # | Receipt | Command | Line(s) to paste | Machine / value |
|---|---------|---------|------------------|-----------------|
| 1 | Hybrid group on a PQ-preferring host | `run http https://www.ietf.org/` | `[tls] group=X25519MLKEM768` | pending-user |
| 2 | Application data (bonus) | same | `--- Response: 200, N bytes body ---` | pending-user |
| 3 | The X9 host (optional) | `run http https://www.cloudflare.com/` | `[tls] group=X25519MLKEM768` — HTTP 200 is **not** required (see CATCH) | pending-user |

## Host-side rehearsal (not a substitute)

Measured 2026-08-24 against OpenSSL 3.5.6 on the build host:

| Host | Group + cipher (forced) | CertificateVerify | Shipped-root match |
|------|-------------------------|-------------------|--------------------|
| `www.ietf.org` | `X25519MLKEM768` + `TLS_CHACHA20_POLY1305_SHA256` | `ecdsa_secp256r1_sha256` | ISRG Root X1 (via X2) |
| `www.cloudflare.com` | same | `ecdsa_secp256r1_sha256` | **no** — GTS / GlobalSign |
| `example.com` | same | (CF / SSL.com chain) | **no** — SSL.com / AAA |
| `rust-lang.org` | same | `rsa_pss_rsae_sha256` | ISRG Root X1 — CV now verified (RES-53 DONE) |

Pick slot 1's host because it is the intersection of: hybrid,
ChaCha20, P-256 SHA-256 CV, and a root we actually ship.

## CATCH, named

1. **Trust store is seventeen entries** (sixteen public roots +
   the GTS WE2 intermediate pin — see [`trust_store.md`](trust_store.md)).
   A host that still chains to something we do not carry prints
   `root not in trust store` (X8), not a generic TLS error.
   Packed TLS 1.3 records (Google/Cloudflare) are reassembled
   without destroying the current handshake message.  RSA-4096
   verify no longer overflows.  SAN matching keeps 64 dNSNames
   so `google.com` is not lost behind `*.google.com`.  A TLS
   application record larger than the 4 KiB HTTP reader (wttr.in
   ships the weather in one ~8 KiB record) keeps the leftover
   instead of dropping it (`AHTTP_ERR_RESPONSE` / `-6`).
2. **RSA-PSS is verified** (`rsa_pss_rsae_sha256`, EMSA-PSS SHA-256,
   MGF1, saltLen=32).  RES-53 is DONE.  A host whose leaf is RSA
   (rust-lang.org) no longer dies at CertificateVerify.
3. **Not CI.** QEMU/SLIRP can reach the internet on a networked
   host, but the public web is not deterministic.  Do not add a
   `test_live_web.sh` that dials a real name.

## What a NULL check can prove

The string `[tls] group=` is emitted by `libahttp` after every
handshake attempt.  Local fixtures already exercise it:
`test_https6` (pinned `-groups X25519`, D4) and
`test_x25519mlkem` (tlstest prints `[tls] PASS: X25519MLKEM768`).
Those prove the printer and the group, not the public web.
