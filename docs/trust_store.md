# AuraLite OS Trust Store — Provenance and Lifecycle

**REALINTERNET_PLAN phase X8.** This document records *what* roots are shipped,
*where they came from*, *when they expire*, and *how the store is kept working*
as roots rotate in the modern ~90-day, automated certificate world.

The shipped store is a single PEM file, `/etc/ssl/roots.pem`, packaged into the
initrd by the build (`make iso` copies `etc/ssl/roots.pem` → initrd
`/etc/ssl/roots.pem`). It is consumed by `libahttp`/`libatls` via
`ahttp_load_trust_roots()` for HTTPS chain validation.

## 1. The decision: (b) rebuild-and-reship, with a dated provenance file

The two options the plan offers:

- **(a) A small, signed, in-image trust-store update fetched over the very TLS
  it protects.** Rejected: this is a bootstrap trust problem — verifying the
  update needs a key you must already trust, and for a hobby OS it adds a
  networked update path and a signing/verification pipeline whose failure modes
  are worse than the problem it solves.
- **(b) A documented rebuild-and-reship process with a dated provenance file.**
  **Chosen.** The store is a reviewed, static file; when a root nears expiry or
  a chain moves to a root we do not carry, the fix is to edit `etc/ssl/roots.pem`
  and rebuild the ISO — with this document updated in the same commit (rule D5),
  so the provenance is never stale.

The consequence is explicit: **the image is not self-healing.** A root that
expires, or a chain that moves to a root we do not carry, fails the handshake.
The failure is *diagnosable* rather than mysterious, because (1) the handshake
now reports "root not in trust store" as its own message (not a generic TLS
error), and (2) `trustinfo` prints the expiry of every shipped root. Rotating
roots are a *reship*, not a runtime event.

## 2. Shipped roots and their expiry

Dated **2026-08-10**. Sources are the issuers' published root-certificate
bundle; each was fetched and verified against the issuer's official
distribution. SHA-256 fingerprints are included so the bytes in this repo can
be audited against the source.

| Common name | Issuer | SHA-256 fingerprint | not-before | **not-after** | Source |
|---|---|---|---|---|---|
| DigiCert Global Root CA | self-signed | `43:48:A0:E9:44:4C:78:CB:26:5E:05:8D:5E:89:44:B4:D8:4F:96:62:BD:26:DB:25:7F:89:34:A4:43:C7:01:61` | 2006-11-10 | **2031-11-10** | DigiCert (C=US, O=DigiCert Inc) |
| DigiCert Global Root G3 | self-signed | `31:AD:66:48:F8:10:41:38:C7:38:F3:9E:A4:32:01:33:39:3E:3A:18:CC:02:29:6E:F9:7C:2A:C9:EF:67:31:D0` | 2013-08-01 | **2038-01-15** | DigiCert (EC P-384) |
| ISRG Root X1 | self-signed | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` | 2015-06-04 | **2035-06-04** | Internet Security Research Group (Let's Encrypt) |

Earliest expiry: **DigiCert Global Root CA on 2031-11-10**. That is the natural
"rotate the store" checkpoint.

## 3. Rotation procedure (the chosen path, end to end)

1. When a shipped root is within a comfortable margin of its not-after (or the
   plan flags a rotation), obtain the replacement root(s) from the issuer's
   official bundle and **verify** them (compare the SHA-256 fingerprint to the
   issuer's published value, and check the certificate is self-signed and
   unexpired).
2. Update `etc/ssl/roots.pem` (add/remove blocks) **and** this table in the
   same commit.
3. Run the trust-store test gates: `make test-unit` (atls suites incl. the
   "root not in trust store" negative) and the guest `trustinfo` check.
4. Rebuild and reship the ISO.

## 4. Runtime visibility

- `trustinfo` (in `/apps/trustinfo`) reads `/etc/ssl/roots.pem` and prints
  every root's common name and not-after expiry, so a user can see the store
  at a glance.
- A chain whose root is not in the store is refused with the distinct message
  `root not in trust store` (see `ATLS_CERTVAL_ERR_UNKNOWN_ROOT`), not a
  generic handshake failure — so a trust-store gap reads as a trust-store
  issue, not a TLS bug.

## 5. Revocation: OCSP / CRL / Certificate Transparency — excluded

Revocation checking (OCSP and CRL) is **explicitly out of scope**, consistent
with `INTERNET_PLAN` §6 and `REALINTERNET_PLAN` X8: it is a networked protocol
of its own, needs a clock and network trust model of its own, and a hobby OS
has no way to do it honestly today. Certificate Transparency is likewise not
enforced. This is a recorded exclusion, not an oversight: a revoked leaf will
be accepted until its (short) validity window ends. The docs' security
statement (`docs/tls.md`) reflects this.
