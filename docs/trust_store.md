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

Dated **2026-08-24**. Sources are the issuers' published root-certificate
bundle (Debian `ca-certificates`); each SHA-256 fingerprint is included so
the bytes in this repo can be audited against the source.

| Common name | Issuer | SHA-256 fingerprint | not-before | **not-after** | Source |
|---|---|---|---|---|---|
| DigiCert Global Root CA | self-signed | `43:48:A0:E9:44:4C:78:CB:26:5E:05:8D:5E:89:44:B4:D8:4F:96:62:BD:26:DB:25:7F:89:34:A4:43:C7:01:61` | 2006-11-10 | **2031-11-10** | DigiCert |
| DigiCert Global Root G2 | self-signed | `CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F` | 2013-08-01 | **2038-01-15** | DigiCert |
| DigiCert Global Root G3 | self-signed | `31:AD:66:48:F8:10:41:38:C7:38:F3:9E:A4:32:01:33:39:3E:3A:18:CC:02:29:6E:F9:7C:2A:C9:EF:67:31:D0` | 2013-08-01 | **2038-01-15** | DigiCert (EC P-384) |
| ISRG Root X1 | self-signed | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` | 2015-06-04 | **2035-06-04** | Let's Encrypt |
| ISRG Root X2 | self-signed | `69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70` | 2020-09-04 | **2040-09-17** | Let's Encrypt (ECDSA) |
| GTS Root R1 | self-signed | `D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF` | 2016-06-22 | **2036-06-22** | Google Trust Services |
| GTS Root R2 | self-signed | `8D:25:CD:97:22:9D:BF:70:35:6B:DA:4E:B3:CC:73:40:31:E2:4C:F0:0F:AF:CF:D3:2D:C7:6E:B5:84:1C:7E:A8` | 2016-06-22 | **2036-06-22** | Google Trust Services |
| GTS Root R3 | self-signed | `34:D8:A7:3E:E2:08:D9:BC:DB:0D:95:65:20:93:4B:4E:40:E6:94:82:59:6E:8B:6F:73:C8:42:6B:01:0A:6F:48` | 2016-06-22 | **2036-06-22** | Google Trust Services |
| GTS Root R4 | self-signed | `34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D` | 2016-06-22 | **2036-06-22** | Google Trust Services |
| GlobalSign Root CA - R3 | self-signed | `CB:B5:22:D7:B7:F1:27:AD:6A:01:13:86:5B:DF:1C:D4:10:2E:7D:07:59:AF:63:5A:7C:F4:72:0D:C9:63:C5:3B` | 2009-03-18 | **2029-03-18** | GlobalSign |
| GlobalSign Root CA - R6 | self-signed | `2C:AB:EA:FE:37:D0:6C:A2:2A:BA:73:91:C0:03:3D:25:98:29:52:C4:53:64:73:49:76:3A:3A:B5:AD:6C:CF:69` | 2014-12-10 | **2034-12-10** | GlobalSign |
| SSL.com Root CA RSA | self-signed | `85:66:6A:56:2E:E0:BE:5C:E9:25:C1:D8:89:0A:6F:76:A8:7E:C1:6D:4D:7D:5F:29:EA:74:19:CF:20:12:3B:69` | 2016-02-12 | **2041-02-12** | SSL.com |
| SSL.com Root CA ECC | self-signed | `34:17:BB:06:CC:60:07:DA:1B:96:1C:92:0B:8A:B4:CE:3F:AD:82:0E:4A:A3:0B:9A:CB:C4:A7:4E:BD:CE:BC:65` | 2016-02-12 | **2041-02-12** | SSL.com |
| Amazon Root CA 1 | self-signed | `8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E` | 2015-05-26 | **2038-01-17** | Amazon |
| Starfield Services Root G2 | self-signed | `56:8D:69:05:A2:C8:87:08:A4:B3:02:51:90:ED:CF:ED:B1:97:4A:60:6A:13:C6:E5:29:0F:CB:2A:E6:3E:DA:B5` | 2009-09-01 | **2037-12-31** | Amazon / Starfield |
| USERTrust RSA CA | self-signed | `E7:93:C9:B0:2F:D8:AA:13:E2:1C:31:22:8A:CC:B0:81:19:64:3B:74:9C:89:89:64:B1:74:6D:46:C3:D4:CB:D2` | 2010-02-01 | **2038-01-18** | Sectigo / USERTrust |
| GTS WE2 (intermediate pin) | GTS Root R4 | `9C:3F:2F:D1:1C:57:D7:C6:49:AD:5A:09:32:C0:F0:D2:97:56:F6:A0:A1:C7:4C:43:E1:E8:9A:62:D6:4C:D3:20` | 2023-12-13 | **2029-02-20** | Google Trust Services — pinned because R4 signs WE2 with ECDSA-SHA384/P-384, which libatls cannot verify yet. Leaf→WE2 is P-256 SHA-256. |

Seventeen entries. Earliest *root* expiry: **DigiCert Global Root CA on 2031-11-10**.
The WE2 pin expires **2029-02-20** — rotate or land P-384 before then.

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
statement (`../tls.md`) reflects this.
