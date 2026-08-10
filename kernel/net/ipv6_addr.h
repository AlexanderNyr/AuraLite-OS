/* ipv6_addr.h — pure IPv6 address helpers for AuraLite OS.
 *
 * REALINTERNET_PLAN phase X7 (IPv6).  This header and ipv6_addr.c are the
 * pure, host-testable core of the IPv6 work: address representation,
 * text<->binary conversion (RFC 5952), EUI-64 link-local derivation and the
 * ICMPv6 pseudo-header checksum (RFC 8200 / RFC 4443).  They have no kernel
 * dependencies (only <stdint.h>/<string.h>), so the exact same code is
 * compiled into the kernel and into tests/unit/test_ipv6_addr.c, mirroring
 * the dns_parse.c / ip_reasm.c pattern.
 */

#ifndef AURALITE_NET_IPV6_ADDR_H
#define AURALITE_NET_IPV6_ADDR_H

#include <stdint.h>

#define IPV6_ADDR_LEN 16

typedef struct ipv6_addr {
    uint8_t b[IPV6_ADDR_LEN];
} ipv6_addr_t;

/* Parse a text IPv6 address into 16 bytes.  Supports "::" compression and
 * zero or more leading-zero-stripped groups.  Returns 0 on success, -1 on a
 * malformed address.  (IPv4-embedded "::ffff:1.2.3.4" is not parsed; the
 * plan targets plain v6 addresses.) */
int ipv6_pton(const char *s, ipv6_addr_t *out);

/* Format 16 bytes as text, compressed per RFC 5952 (longest zero run, no
 * leading zeros, lowercase).  Returns the number of bytes written excluding
 * the NUL terminator, or -1 if it does not fit in buflen. */
int ipv6_ntop(const ipv6_addr_t *a, char *buf, uint32_t buflen);

/* Derive the modified-EUI-64 interface identifier and build the link-local
 * fe80::/64 address from a 6-byte MAC (the U/L bit is flipped on the first
 * octet, 0xFFFE is inserted). */
void ipv6_linklocal_from_mac(const uint8_t mac[6], ipv6_addr_t *out);

/* True if a is the unspecified address (::). */
int ipv6_is_unspecified(const ipv6_addr_t *a);

/* True if a is link-local (fe80::/10). */
int ipv6_is_linklocal(const ipv6_addr_t *a);

/* True if a is the loopback address (::1). */
int ipv6_is_loopback(const ipv6_addr_t *a);

/* Byte equality: 0 if equal, nonzero otherwise. */
int ipv6_eq(const ipv6_addr_t *a, const ipv6_addr_t *b);

/* Compute the ICMPv6 checksum over src+msg_len+next_header pseudo-header and
 * a message of msg_len bytes, per RFC 8200 s8.1 / RFC 4443 s2.3.  Returns the
 * network-order (big-endian) one's-complement checksum to store in the ICMPv6
 * header.  next_header must be 58 (ICMPv6) for the ICMPv6 checksum. */
uint16_t ipv6_checksum_pseudo(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                              uint32_t len, uint8_t next_header,
                              const void *msg, uint32_t msg_len);

#endif /* AURALITE_NET_IPV6_ADDR_H */
