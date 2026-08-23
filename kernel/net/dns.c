/*
 * dns.c — DNS resolver: TTL cache, secondary-server failover, CNAME chains.
 * REALINTERNET_PLAN.md phase X3 ("DNS reliability").
 *
 * Parse and cache policy are pure code in dns_parse.c; this file wires that
 * to the kernel: UDP transport, server list, spinlock, serial log lines.
 *
 * Log-line conventions the X3 gate (and tests/integration/cases) greps for:
 *   [dns] cache MISS 'name' — querying
 *   [dns] cache HIT 'name' -> a.b.c.d (ttl Ns left)
 *   [dns] cache EXPIRED 'name' — re-querying
 *   [dns] server a.b.c.d no response — trying secondary x.y.z.t
 *   [dns] 'alias' is a CNAME for 'target' — following
 *   [dns] PASS: 'name' -> a.b.c.d
 *   [dns] FAIL: 'name' unresolved
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/net/dns.h"
#include "kernel/net/dns_parse.h"
#include "kernel/net/tcp.h"
#include "kernel/net/net.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "drivers/timer/pit.h"
#include "kernel/rng.h"

/* QEMU SLIRP's built-in DNS proxy: default (and DHCP-free fallback). */
#define DNS_DEFAULT_SERVER 0x0A000203u   /* 10.0.2.3, host order */

static uint32_t    g_servers[DNS_SERVERS_MAX];
static int         g_nservers = 0;
static dns_cache_t g_cache;
static spinlock_t  g_dns_lock = SPINLOCK_UNLOCKED;

static const char *ip4(uint32_t ip, char *b) {   /* host order -> "a.b.c.d" */
    ksnprintf(b, 16, "%u.%u.%u.%u",
              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return b;
}

void dns_init(void) {
    spinlock_acquire(&g_dns_lock);
    g_nservers = 1;
    g_servers[0] = DNS_DEFAULT_SERVER;
    dns_cache_reset(&g_cache);
    spinlock_release(&g_dns_lock);
    kprintf("[dns] init: cache %d entries, up to %d servers, default %s\n",
            DNS_CACHE_MAX, DNS_SERVERS_MAX, ip4(DNS_DEFAULT_SERVER,
            (char[16]){0}));
}

void dns_set_servers(const uint32_t *ips, int n) {
    if (!ips || n <= 0) return;
    if (n > DNS_SERVERS_MAX) n = DNS_SERVERS_MAX;
    spinlock_acquire(&g_dns_lock);
    for (int i = 0; i < n; i++) g_servers[i] = ips[i];
    g_nservers = n;
    spinlock_release(&g_dns_lock);
    char b[16];
    for (int i = 0; i < n; i++)
        kprintf("[dns] server %d: %s\n", i + 1, ip4(ips[i], b));
}

int dns_get_servers(uint32_t *out, int max) {
    if (!out || max <= 0) return 0;
    spinlock_acquire(&g_dns_lock);
    int n = g_nservers < max ? g_nservers : max;
    for (int i = 0; i < n; i++) out[i] = g_servers[i];
    spinlock_release(&g_dns_lock);
    return n;
}

int dns_cache_snapshot(dnsctl_entry_t *out, int max_entries) {
    if (!out || max_entries <= 0) return 0;
    uint64_t now = timer_get_ticks();
    int n = 0;
    spinlock_acquire(&g_dns_lock);
    for (int i = 0; i < DNS_CACHE_MAX && n < max_entries; i++) {
        const dns_cache_entry_t *e = dns_cache_entry(&g_cache, i);
        if (!e) continue;
        memcpy(out[n].name, e->name, sizeof(out[n].name));
        out[n].ip       = e->ip;
        out[n].negative = e->negative;
        out[n].ttl_left = (uint32_t)((e->expires_at - now) / DNS_TICKS_PER_SEC);
        n++;
    }
    spinlock_release(&g_dns_lock);
    return n;
}

void dns_cache_flush(void) {
    spinlock_acquire(&g_dns_lock);
    dns_cache_reset(&g_cache);
    spinlock_release(&g_dns_lock);
    kprintf("[dns] cache flushed\n");
}

/* ---------------- transport ---------------- */

static uint16_t dns_next_id(void) {
    static uint16_t seq = 0xA500;
    uint64_t r = rng_available() ? rng_u64() : 0;
    uint64_t t = timer_get_ticks();
    seq = (uint16_t)(seq + 0x9E37u);
    return (uint16_t)((uint16_t)r ^ (uint16_t)t ^ seq);
}

/* R9: one-shot TC injection (DNSCTL_FORCE_TC) -- the UDP leg is
 * synthesised, the TCP fallback it triggers is REAL wire. */
static volatile int g_force_tc;
void dns_force_tc_once(void) { g_force_tc = 1; }

/* Build the type-A question into query[]; returns the length or -1.
 * Shared by the UDP transport and the R9 TCP fallback (RES-25) --
 * one builder, two carriages, so the fallback can never ask a
 * subtly different question. */
static int dns_build_query(const char *qname, uint8_t *query, int cap,
                           uint16_t *out_id) {
    if (cap < 12) return -1;
    memset(query, 0, (size_t)cap);
    uint16_t id = dns_next_id();
    query[0] = (uint8_t)(id >> 8); query[1] = (uint8_t)id;
    query[2] = 0x01; query[3] = 0x00;              /* RD */
    query[4] = 0;    query[5] = 1;                 /* QDCOUNT=1 */
    int name_len = dns_encode_name(qname, query + 12, cap - 12);
    if (name_len < 0) return -1;
    int q_off = 12 + name_len;
    if (q_off + 4 > cap) return -1;
    query[q_off]     = 0; query[q_off + 1] = DNS_RTYPE_A;   /* type A   */
    query[q_off + 2] = 0; query[q_off + 3] = 1;             /* class IN */
    *out_id = id;
    return q_off + 4;
}

/*
 * One DNS query round against one server: build the type-A question, send it
 * from DNS_LOCAL_PORT, wait (bounded) for the reply from that server:53.
 * Returns the response length, or -1 on send failure/timeout.
 */
static int dns_wire_query(uint32_t server_ip, const char *qname,
                          uint8_t *resp, int cap) {
    uint8_t query[512];
    uint16_t id = 0;
    int query_len = dns_build_query(qname, query, (int)sizeof(query), &id);
    if (query_len < 0) return -1;

    if (g_force_tc) {
        /* The armed knob: answer OURSELVES with a bare truncated
         * header (QR|RD|RA|TC, zero counts) carrying the real ID.
         * The resolver takes the same TRUNCATED branch a >512B
         * answer forces, and the TCP retry that follows is real. */
        g_force_tc = 0;
        memset(resp, 0, 12);
        resp[0] = (uint8_t)(id >> 8); resp[1] = (uint8_t)id;
        resp[2] = 0x83; resp[3] = 0x80;
        kprintf("[dns] FORCE_TC: synthesising a truncated UDP answer "
                "(test knob, one shot)\n");
        return 12;
    }

    if (net_udp_sendto(server_ip, 53, DNS_LOCAL_PORT, query, query_len) != 0)
        return -1;

    uint32_t got_ip = 0;
    uint16_t got_port = 0;
    int n = net_udp_recvfrom(DNS_LOCAL_PORT, &got_ip, &got_port,
                             resp, (uint32_t)cap, DNS_QUERY_TIMEOUT_TICKS);
    if (n < 0) return -1;
    if (got_ip != server_ip || got_port != 53) return -1;

    /* Reject off-path answers before parsing: the ID must be ours. */
    if (n >= 2) {
        uint16_t rid = (uint16_t)((resp[0] << 8) | resp[1]);
        if (rid != id) return -1;
    }
    return n;
}

/* R9 (ledger RES-25): the TCP carriage for truncated answers.  RFC
 * 1035 s4.2.2: two-byte length prefix each way, same message format.
 * A fresh ID is used (a TC retry is a new transaction) and checked.
 * Returns the response length, or -1. */
static int dns_wire_query_tcp(uint32_t server_ip, const char *qname,
                              uint8_t *resp, int cap) {
    uint8_t query[512];
    uint16_t id = 0;
    int query_len = dns_build_query(qname, query, (int)sizeof(query), &id);
    if (query_len < 0) return -1;

    tcp_handle_t h = tcp_open(server_ip, 53);
    if (h < 0) {
        kprintf("[dns] TCP connect to server:53 failed\n");
        return -1;
    }
    uint8_t pfx[2] = { (uint8_t)(query_len >> 8), (uint8_t)query_len };
    if (tcp_send_h(h, pfx, 2) < 0 ||
        tcp_send_h(h, query, (uint32_t)query_len) < 0) {
        tcp_close_h(h);
        return -1;
    }

    /* Collect the length prefix, then exactly that many bytes (a
     * segment boundary may fall anywhere -- accumulate, do not
     * assume one recv = one message).  tcp_recv_h is non-blocking
     * (0 = nothing yet), so the loop is bounded by wall clock. */
    uint8_t acc[2 + 1024];
    int have = 0;
    int want = 2;
    uint64_t deadline = timer_get_ticks() + DNS_QUERY_TIMEOUT_TICKS * 2;
    while (have < want) {
        int n = tcp_recv_h(h, acc + have, (uint32_t)(sizeof(acc) - have));
        if (n < 0)
            break;
        if (n == 0) {
            if (timer_get_ticks() > deadline)
                break;
            continue;
        }
        have += n;
        if (want == 2 && have >= 2) {
            int mlen = (acc[0] << 8) | acc[1];
            if (mlen <= 0 || mlen > (int)sizeof(acc) - 2)
                break;
            want = 2 + mlen;
        }
    }
    tcp_close_h(h);
    if (want == 2 || have < want)
        return -1;
    int mlen = want - 2;
    if (mlen > cap)
        return -1;                       /* caller's buffer decides */
    if (mlen >= 2) {
        uint16_t rid = (uint16_t)((acc[2] << 8) | acc[3]);
        if (rid != id)
            return -1;
    }
    memcpy(resp, acc + 2, (size_t)mlen);
    return mlen;
}

static dns_transport_fn g_transport = dns_wire_query;

void dns_set_transport_for_tests(dns_transport_fn fn) {
    g_transport = fn ? fn : dns_wire_query;
}

/* ---------------- resolver ---------------- */

uint32_t dns_resolve_ipv4(const char *hostname) {
    if (!hostname || !*hostname) return 0;

    char name[DNS_MAX_NAME + 1];
    int i = 0;
    for (; hostname[i] && i < DNS_MAX_NAME; i++) {
        char c = hostname[i];
        name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    name[i] = 0;
    if (i > 0 && name[i - 1] == '.') name[i - 1] = 0;   /* root dot */
    if (!name[0]) return 0;

    /* 1) cache */
    uint32_t ip = 0, ttl_left = 0;
    int neg = 0;
    spinlock_acquire(&g_dns_lock);
    int hit = dns_cache_lookup(&g_cache, name, timer_get_ticks(),
                               DNS_TICKS_PER_SEC, &ip, &neg, &ttl_left);
    spinlock_release(&g_dns_lock);
    char b[16];
    if (hit == 1) {
        if (neg) {
            kprintf("[dns] cache HIT '%s' (negative, ttl %us left)\n", name, ttl_left);
        } else {
            kprintf("[dns] cache HIT '%s' -> %s (ttl %us left)\n",
                    name, ip4(ip, b), ttl_left);
        }
        return ip;
    }
    if (hit == -1) {
        kprintf("[dns] cache EXPIRED '%s' — re-querying\n", name);
    } else {
        kprintf("[dns] cache MISS '%s' — querying\n", name);
    }

    /* 2) wire: servers in order, attempts each, CNAME chase bounded */
    uint8_t resp[1024];
    char current[DNS_MAX_NAME + 1];
    memcpy(current, name, (size_t)i + 1);

    for (int depth = 0; depth < DNS_MAX_CNAME_DEPTH; depth++) {
        int got = -1;
        uint32_t used_server = 0;
        int nsrv;
        uint32_t srv[DNS_SERVERS_MAX];
        nsrv = dns_get_servers(srv, DNS_SERVERS_MAX);

        for (int s = 0; s < nsrv && got < 0; s++) {
            for (int attempt = 0; attempt < DNS_ATTEMPTS_PER_SERVER; attempt++) {
                got = g_transport(srv[s], current, resp, (int)sizeof(resp));
                if (got >= 0) break;
                kprintf("[dns] server %s no response (attempt %d/%d)\n",
                        ip4(srv[s], b), attempt + 1, DNS_ATTEMPTS_PER_SERVER);
            }
            if (got < 0 && s + 1 < nsrv) {
                char b2[16];
                kprintf("[dns] server %s failed — trying secondary %s\n",
                        ip4(srv[s], b), ip4(srv[s + 1], b2));
            }
            if (got >= 0) used_server = srv[s];
        }


        if (got < 0) {
            kprintf("[dns] FAIL: '%s' unresolved (all %d server(s) silent)\n",
                    name, nsrv);
            return 0;
        }

        /* The transport already rejected foreign IDs; parse with any ID. */
        uint16_t rid = (uint16_t)((resp[0] << 8) | resp[1]);
        dns_result_t r;
        int pr = dns_parse_response(resp, got, current, rid, &r);

        /* R9 (RES-25): a TC answer re-asks the SAME server over TCP
         * before the switch judges the result -- the fallback the X3
         * landing deferred with a name. */
        if (pr == DNS_PARSE_TRUNCATED) {
            kprintf("[dns] '%s' reply truncated (TC) — retrying over "
                    "TCP (RFC 1035 s4.2.2)\n", current);
            got = dns_wire_query_tcp(used_server, current, resp,
                                     (int)sizeof(resp));
            if (got <= 0) {
                kprintf("[dns] FAIL: TCP fallback carried no answer\n");
                return 0;
            }
            kprintf("[dns] PASS: TCP fallback answer, %d bytes\n", got);
            rid = (uint16_t)((resp[0] << 8) | resp[1]);
            pr = dns_parse_response(resp, got, current, rid, &r);
        }

        switch (pr) {
        case DNS_PARSE_ANSWER:
            spinlock_acquire(&g_dns_lock);
            dns_cache_insert(&g_cache, current, r.a_ip, 0, r.a_ttl,
                             timer_get_ticks(), DNS_TICKS_PER_SEC);
            spinlock_release(&g_dns_lock);
            kprintf("[dns] inserted '%s' -> %s (TTL %us)\n",
                    current, ip4(r.a_ip, b), r.a_ttl);
            kprintf("[dns] PASS: '%s' -> %s\n", name, ip4(r.a_ip, b));
            return r.a_ip;

        case DNS_PARSE_CNAME:
            kprintf("[dns] '%s' is a CNAME for '%s' — following\n",
                    current, r.cname_target);
            memcpy(current, r.cname_target, sizeof(current));
            current[DNS_MAX_NAME] = 0;
            continue;   /* next depth iteration queries the target */

        case DNS_PARSE_NXDOMAIN:
        case DNS_PARSE_NODATA: {
            uint32_t nttl = r.neg_ttl ? r.neg_ttl : DNS_NEG_DEFAULT_TTL;
            spinlock_acquire(&g_dns_lock);
            dns_cache_insert(&g_cache, current, 0, 1, nttl,
                             timer_get_ticks(), DNS_TICKS_PER_SEC);
            spinlock_release(&g_dns_lock);
            kprintf("[dns] negative-cache '%s' (%s, TTL %us)\n", current,
                    pr == DNS_PARSE_NXDOMAIN ? "NXDOMAIN" : "NODATA", nttl);
            kprintf("[dns] FAIL: '%s' does not resolve\n", name);
            return 0;
        }

        case DNS_PARSE_TRUNCATED:
            kprintf("[dns] '%s' STILL truncated over TCP — malformed "
                    "server, failing loudly per D7\n", current);
            return 0;

        default:
            kprintf("[dns] malformed response for '%s' — treating as failure\n",
                    current);
            return 0;
        }
    }

    kprintf("[dns] FAIL: '%s' CNAME chain exceeds %d levels\n",
            name, DNS_MAX_CNAME_DEPTH);
    return 0;
}

void dns_self_test(void) {
    kprintf("[dns] self-test: resolving 'example.com' (expect cache MISS)...\n");
    uint32_t ip1 = dns_resolve_ipv4("example.com");
    if (ip1 == 0) {
        kprintf("[dns] FAIL: could not resolve example.com\n");
        return;
    }
    kprintf("[dns] self-test: resolving again (expect cache HIT)...\n");
    uint32_t ip2 = dns_resolve_ipv4("example.com");
    if (ip2 == ip1) {
        kprintf("[dns] PASS: example.com -> %s (cache verified)\n", ip4(ip1,
                (char[16]){0}));
    } else {
        kprintf("[dns] FAIL: cached answer mismatch\n");
    }
}

/* ---- net.h compatibility entry points (Phase 13 API, kept stable) ---- */

uint32_t net_dns_resolve(const char *hostname) {
    return dns_resolve_ipv4(hostname);
}

void net_dns_self_test(void) {
    kprintf("[net] dns self-test: resolving 'example.com'...\n");
    dns_self_test();
}
