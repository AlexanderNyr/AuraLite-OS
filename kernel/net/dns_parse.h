#ifndef AURALITE_NET_DNS_PARSE_H
#define AURALITE_NET_DNS_PARSE_H

#include <stdint.h>
#include <stddef.h>

/*
 * dns_parse.h — pure DNS wire-format parser and TTL cache core
 * (REALINTERNET_PLAN.md phase X3).
 *
 * Everything in this module is free of kernel dependencies: no kprintf, no
 * heap, no timer — all state is caller-provided and time is passed in
 * explicitly.  That is what lets tests/unit/test_dns.c compile this exact
 * code on the host and exercise every branch deterministically (crafted
 * responses, scripted expiry), while kernel/net/dns.c wires the same code
 * to the live UDP transport.
 */

#define DNS_MAX_NAME        253     /* RFC 1035 fully-qualified name cap     */
#define DNS_CACHE_MAX       16      /* cache slots (positive + negative)     */
#define DNS_TTL_CAP         86400u  /* never honour a TTL beyond 1 day       */
#define DNS_NEG_DEFAULT_TTL 60u     /* negative TTL when no SOA is present   */
#define DNS_MAX_CNAME_DEPTH 4       /* alias-chain chase budget              */

/* DNS types/rcodes the resolver distinguishes. */
#define DNS_RTYPE_A         1
#define DNS_RTYPE_CNAME     5
#define DNS_RTYPE_SOA       6
#define DNS_RCODE_NOERROR   0
#define DNS_RCODE_NXDOMAIN  3

/*
 * Encode a dotted hostname ("example.com") into DNS label format.
 * Returns the encoded length (>0), or -1 when the name is malformed
 * (overlong label/name, empty label, NULL).
 */
int dns_encode_name(const char *name, uint8_t *out, int out_cap);

/* Parse outcomes for dns_parse_response(). */
enum {
    DNS_PARSE_ANSWER    =  1,   /* a_ip/a_ttl valid (terminal A found)      */
    DNS_PARSE_CNAME     =  2,   /* alias chain, no A in packet: re-query
                                   cname_target (bounded by caller)          */
    DNS_PARSE_NXDOMAIN  =  3,   /* name does not exist; neg_ttl may be set   */
    DNS_PARSE_NODATA    =  4,   /* name exists, no A here; neg_ttl may be set*/
    DNS_PARSE_TRUNCATED =  5,   /* TC bit set: caller may retry over TCP
                                   (X3 defers TCP fallback; see plan D4)     */
    DNS_PARSE_BAD       = -1    /* malformed, not our answer, ID mismatch    */
};

typedef struct {
    uint32_t a_ip;                       /* host order terminal A address     */
    uint32_t a_ttl;                      /* min TTL along the chain to the A  */
    uint32_t neg_ttl;                    /* RFC 2308 negative TTL (SOA) or 0  */
    char     cname_target[DNS_MAX_NAME + 1]; /* deepest alias, DNS_PARSE_CNAME */
} dns_result_t;

/*
 * Parse a DNS response that answers a type-A question for `qname`
 * (dotted, any case; the query name as sent), following in-packet CNAME
 * chains to the terminal A record.  `expect_id` must match the header ID
 * (off-path rejection).  All name walks are bounds-checked with a pointer
 * jump budget, so compression loops and out-of-buffer pointers are refused.
 * Fills *out on non-negative returns; returns one of the DNS_PARSE_* codes.
 */
int dns_parse_response(const uint8_t *msg, int len, const char *qname,
                       uint16_t expect_id, dns_result_t *out);

/* ---- cache core (pure: caller supplies the clock) ---- */

typedef struct {
    char     name[DNS_MAX_NAME + 1];  /* normalised (lowercase) query name   */
    uint32_t ip;                      /* host order; 0 when negative         */
    uint8_t  used;
    uint8_t  negative;                /* NXDOMAIN/NODATA entry               */
    uint64_t expires_at;              /* ticks                               */
    uint64_t last_used;               /* LRU sequence                        */
} dns_cache_entry_t;

typedef struct {
    dns_cache_entry_t e[DNS_CACHE_MAX];
    uint64_t          seq;            /* LRU clock                           */
} dns_cache_t;

void dns_cache_reset(dns_cache_t *c);

/*
 * Look up `name` (case-insensitive normalised match).
 *   1  hit           — *out_ip / *out_neg / *out_ttl_left filled
 *   0  miss          — name not present
 *  -1  present but expired (entry evicted; caller logs "EXPIRED, re-query")
 */
int dns_cache_lookup(dns_cache_t *c, const char *name,
                     uint64_t now_ticks, uint32_t ticks_per_sec,
                     uint32_t *out_ip, int *out_neg, uint32_t *out_ttl_left);

/*
 * Insert/replace an entry.  TTL is capped at DNS_TTL_CAP; TTL 0 means
 * "do not cache" (RFC 2181) and is a no-op.  Evicts expired entries first,
 * then the LRU slot.
 */
void dns_cache_insert(dns_cache_t *c, const char *name, uint32_t ip,
                      int negative, uint32_t ttl_sec,
                      uint64_t now_ticks, uint32_t ticks_per_sec);

int dns_cache_count(const dns_cache_t *c);
const dns_cache_entry_t *dns_cache_entry(const dns_cache_t *c, int idx);

#endif /* AURALITE_NET_DNS_PARSE_H */
