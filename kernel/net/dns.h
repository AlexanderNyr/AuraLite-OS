#ifndef AURALITE_NET_DNS_H
#define AURALITE_NET_DNS_H

#include <stdint.h>
#include "kernel/net/dns_parse.h"

/*
 * dns.h — DNS resolver: TTL cache, server failover, CNAME chasing
 * (REALINTERNET_PLAN.md phase X3).
 *
 * Wire format and cache policy live in the pure dns_parse module; this
 * module owns the live parts: server configuration (from DHCP or the
 * `dnsset` shell command via SYS_DNSCTL), the UDP query transport with
 * per-server attempts and failover to the secondary, and the kernel-side
 * cache log lines the X3 test gate greps for.
 */

#define DNS_SERVERS_MAX        4
#define DNS_LOCAL_PORT         12345
#define DNS_ATTEMPTS_PER_SERVER 2
#define DNS_QUERY_TIMEOUT_TICKS 200    /* 2 s at 100 Hz -> per attempt   */
#define DNS_TICKS_PER_SEC       100    /* PIT rate: timeouts and TTL math */

/* One cache row as reported to userspace by SYS_DNSCTL/DNSCTL_LIST. */
typedef struct {
    char     name[256];
    uint32_t ip;          /* host order; 0 when this is a negative entry */
    uint32_t ttl_left;    /* seconds until expiry at the time of listing */
    uint8_t  negative;
} dnsctl_entry_t;

#define DNSCTL_LIST        1   /* buf: dnsctl_entry_t[], returns entry count */
#define DNSCTL_FLUSH       2   /* drop all cache entries                     */
#define DNSCTL_SET_SERVERS 3   /* buf: uint32_t[] host-order IPs (len/4)     */
#define DNSCTL_GET_SERVERS 4   /* buf: uint32_t[], returns server count      */
#define DNSCTL_FORCE_TC    5   /* R9 (RES-25) TEST KNOB, named: the next UDP
                                * answer is synthesised as truncated (TC=1),
                                * so a lane can drive the REAL TCP fallback
                                * against a real server without root-bound
                                * port-53 fixtures.  One-shot. */

/* Reset the module: empty cache, default server list (QEMU SLIRP DNS). */
void dns_init(void);

/* Replace the resolver server list (DHCP option 6 or dnsset), host order.
 * n is clamped to DNS_SERVERS_MAX. */
void dns_set_servers(const uint32_t *ips_host_order, int n);

/* Copy the active server list out; returns the count. */
int dns_get_servers(uint32_t *out, int max);

/* Cache counters for the shell/SYS_DNSCTL (lock-protected snapshots). */
int dns_cache_snapshot(dnsctl_entry_t *out, int max_entries);
void dns_cache_flush(void);

/*
 * Resolve a hostname to an IPv4 address (host byte order), 0 on failure.
 * Consults the TTL cache first, then queries the configured servers in
 * order with retries, follows CNAME chains up to DNS_MAX_CNAME_DEPTH and
 * caches both positive and negative results.
 * This is the implementation behind net_dns_resolve() and SYS_DNS.
 */
uint32_t dns_resolve_ipv4(const char *hostname);

/* Y3: type-AAAA query.  Writes 16 octets on success (0), -1 on miss.
 * Not cached this phase — the A cache stays IPv4-shaped. */
int dns_resolve_aaaa(const char *hostname, uint8_t out[16]);

/* R9: arm the one-shot synthetic-TC switch (DNSCTL_FORCE_TC). */
void dns_force_tc_once(void);

/* Boot-time self-test (called from net_dns_self_test). */
void dns_self_test(void);

/* Test hook: replace the UDP transport (used by the host unit test;
 * the kernel keeps the default wire transport). */
typedef int (*dns_transport_fn)(uint32_t server_ip, const char *qname,
                                uint8_t *resp, int cap);
void dns_set_transport_for_tests(dns_transport_fn fn);

#endif /* AURALITE_NET_DNS_H */
