/*
 * test_dns.c — host unit tests for REALINTERNET_PLAN phase X3 (DNS
 * reliability).
 *
 * Compiles the REAL modules — kernel/net/dns_parse.c (pure parser + cache
 * core) and kernel/net/dns.c (resolver with failover/CNAME/caching) — with
 * -DAURALITE_DNS_HOST_TEST, stubbing the kernel environment (kprintf, clock,
 * spinlock, rng, UDP).  The clock is a global the tests advance manually, so
 * TTL expiry is fully deterministic; the resolver's transport is scripted
 * per test, so server failover and CNAME re-queries run without a network.
 *
 * Scenarios cover the X3 gate: cache HIT/MISS/expiry, negative caching,
 * primary-blackhole failover to the secondary, in-packet and cross-packet
 * CNAME chains, truncation (loud failure), and wire-level attack shapes
 * (compression loops, out-of-range pointers, ID mismatch).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "kernel/net/dns_parse.h"
#include "kernel/net/dns.h"
#include "kernel/lib/spinlock.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---------------- kernel environment stubs ---------------- */

static uint64_t g_now = 0;                       /* ticks, 100 per second */
uint64_t timer_get_ticks(void) { return g_now; }
static void advance_sec(uint64_t s) { g_now += s * 100; }

void kprintf(const char *fmt, ...) { (void)fmt; }
int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void spinlock_init(spinlock_t *l)    { (void)l; }
void spinlock_acquire(spinlock_t *l) { (void)l; }
void spinlock_release(spinlock_t *l) { (void)l; }

int      rng_available(void) { return 0; }
uint64_t rng_u64(void)       { return 0; }

/* R9: TCP stubs for the fallback carriage -- linked, and exercised
 * only as a REFUSAL on the host (tcp_open fails), which is exactly
 * what a truncated answer with no live TCP stack should meet.  The
 * end-to-end fallback is the guest case's job (test_dns_tcp). */
int tcp_open(uint32_t dst_ip, uint16_t dst_port) {
    (void)dst_ip; (void)dst_port;
    return -1;
}
int tcp_send_h(int h, const void *data, uint32_t len) {
    (void)h; (void)data; (void)len;
    return -1;
}
int tcp_recv_h(int h, void *buf, uint32_t bufsize) {
    (void)h; (void)buf; (void)bufsize;
    return -1;
}
int tcp_close_h(int h) {
    (void)h;
    return -1;
}

/* UDP stubs: only linked, never called — dns_set_transport_for_tests()
 * replaces the wire transport in every resolver test. */
int net_udp_sendto(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const void *data, uint32_t data_len) {
    (void)dst_ip; (void)dst_port; (void)src_port; (void)data; (void)data_len;
    return -1;
}
int net_udp_recvfrom(uint16_t local_port, uint32_t *src_ip, uint16_t *src_port,
                     void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    (void)local_port; (void)src_ip; (void)src_port; (void)buf;
    (void)bufsize; (void)timeout_ticks;
    return -1;
}

/* ---------------- DNS message builders ---------------- */

static int hdr(uint8_t *b, uint16_t id, uint16_t flags,
               uint16_t qd, uint16_t an, uint16_t ns) {
    b[0] = id >> 8; b[1] = id & 0xFF;
    b[2] = flags >> 8; b[3] = flags & 0xFF;
    b[4] = qd >> 8; b[5] = qd & 0xFF;
    b[6] = an >> 8; b[7] = an & 0xFF;
    b[8] = ns >> 8; b[9] = ns & 0xFF;
    b[10] = 0; b[11] = 0;
    return 12;
}

static int question(uint8_t *b, int off, const char *name) {
    int n = dns_encode_name(name, b + off, 512 - off);
    off += n;
    b[off++] = 0; b[off++] = 1;   /* A  */
    b[off++] = 0; b[off++] = 1;   /* IN */
    return off;
}

/* RR header with a compression pointer to the question name (offset 12). */
static int rr_hdr_ptr(uint8_t *b, int off, uint16_t type,
                      uint32_t ttl, uint16_t rdlen) {
    b[off++] = 0xC0; b[off++] = 0x0C;
    b[off++] = type >> 8; b[off++] = type & 0xFF;
    b[off++] = 0; b[off++] = 1;   /* IN */
    b[off++] = (ttl >> 24) & 0xFF; b[off++] = (ttl >> 16) & 0xFF;
    b[off++] = (ttl >> 8) & 0xFF;  b[off++] = ttl & 0xFF;
    b[off++] = rdlen >> 8; b[off++] = rdlen & 0xFF;
    return off;
}

#define IP(a, b_, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b_) << 16) | \
                         ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---------------- 1. name encoder ---------------- */

static void t_encode(void) {
    uint8_t b[300];
    int n = dns_encode_name("Example.COM", b, sizeof(b));
    CHECK_EQ(n, 13);
    CHECK(b[0] == 7 && memcmp(b + 1, "example", 7) == 0);
    CHECK(b[8] == 3 && memcmp(b + 9, "com", 3) == 0 && b[12] == 0);
    CHECK_EQ(dns_encode_name("", b, sizeof(b)), -1);
    CHECK_EQ(dns_encode_name("a..b", b, sizeof(b)), -1);
    CHECK_EQ(dns_encode_name(NULL, b, sizeof(b)), -1);
    char longlbl[80]; memset(longlbl, 'x', 66); longlbl[66] = 0;
    CHECK_EQ(dns_encode_name(longlbl, b, sizeof(b)), -1);
    CHECK_EQ(dns_encode_name("example.com", b, 8), -1);   /* tiny buffer */
}

/* ---------------- 2. parser: plain A ---------------- */

static void t_parse_a(void) {
    uint8_t m[512];
    int o = hdr(m, 0xBEEF, 0x8180, 1, 1, 0);          /* resp, RD|RA, NOERROR */
    o = question(m, o, "example.com");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_A, 300, 4);
    m[o++] = 93; m[o++] = 184; m[o++] = 216; m[o++] = 34;

    dns_result_t r;
    CHECK_EQ(dns_parse_response(m, o, "example.com", 0xBEEF, &r),
             DNS_PARSE_ANSWER);
    CHECK_EQ(r.a_ip, IP(93, 184, 216, 34));
    CHECK_EQ(r.a_ttl, 300);
    CHECK_EQ(dns_parse_response(m, o, "EXAMPLE.COM", 0xBEEF, &r),
             DNS_PARSE_ANSWER);                          /* case-insensitive */
    CHECK_EQ(dns_parse_response(m, o, "example.com", 0xDEAD, &r),
             DNS_PARSE_BAD);                             /* wrong ID */
    m[2] = 0x01;                                          /* QR cleared */
    CHECK_EQ(dns_parse_response(m, o, "example.com", 0xBEEF, &r),
             DNS_PARSE_BAD);
    m[2] = 0x83; m[3] = 0x80;                             /* TC set (0x0200: high byte) */
    CHECK_EQ(dns_parse_response(m, o, "example.com", 0xBEEF, &r),
             DNS_PARSE_TRUNCATED);
    CHECK_EQ(dns_parse_response(m, 8, "example.com", 0xBEEF, &r),
             DNS_PARSE_BAD);
}

/* ---------------- 3. parser: CNAME chains ---------------- */

static void t_parse_cname_chain(void) {
    uint8_t m[512];
    /* www.example.com -> web.cdn.net (ttl 60) -> A 1.2.3.4 (ttl 120) */
    int o = hdr(m, 0x1111, 0x8180, 1, 3, 0);
    o = question(m, o, "www.example.com");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_CNAME, 60, 0);         /* rdlen patched */
    int cname_rdlen_at = o - 2;
    int tgt_mark = o;
    o += dns_encode_name("web.cdn.net", m + o, 512 - o);
    m[cname_rdlen_at] = 0; m[cname_rdlen_at + 1] = (uint8_t)(o - tgt_mark);
    /* second CNAME: web.cdn.net (uncompressed owner) -> edge.cdn.net */
    o += dns_encode_name("web.cdn.net", m + o, 512 - o);
    m[o++] = 0; m[o++] = 5; m[o++] = 0; m[o++] = 1;
    m[o++] = 0; m[o++] = 0; m[o++] = 0; m[o++] = 45;      /* ttl 45 */
    m[o++] = 0; m[o++] = 0;                               /* rdlen patched */
    int rdlen2_at = o - 2;
    int tgt2 = o;
    o += dns_encode_name("edge.cdn.net", m + o, 512 - o);
    m[rdlen2_at] = 0; m[rdlen2_at + 1] = (uint8_t)(o - tgt2);
    /* A for edge.cdn.net via pointer to owner2's rdata name */
    o += dns_encode_name("edge.cdn.net", m + o, 512 - o);
    m[o++] = 0; m[o++] = 1; m[o++] = 0; m[o++] = 1;
    m[o++] = 0; m[o++] = 0; m[o++] = 0; m[o++] = 120;     /* ttl 120 */
    m[o++] = 0; m[o++] = 4;
    m[o++] = 1; m[o++] = 2; m[o++] = 3; m[o++] = 4;

    dns_result_t r;
    CHECK_EQ(dns_parse_response(m, o, "www.example.com", 0x1111, &r),
             DNS_PARSE_ANSWER);
    CHECK_EQ(r.a_ip, IP(1, 2, 3, 4));
    CHECK_EQ(r.a_ttl, 45);                                /* min(60,45,120) */
}

static void t_parse_cname_no_a(void) {
    uint8_t m[512];
    int o = hdr(m, 0x2222, 0x8180, 1, 1, 0);
    o = question(m, o, "alias.example.org");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_CNAME, 90, 0);
    int rdlen_at = o - 2, tgt = o;
    o += dns_encode_name("real.example.org", m + o, 512 - o);
    m[rdlen_at] = 0; m[rdlen_at + 1] = (uint8_t)(o - tgt);

    dns_result_t r;
    CHECK_EQ(dns_parse_response(m, o, "alias.example.org", 0x2222, &r),
             DNS_PARSE_CNAME);
    CHECK(strcmp(r.cname_target, "real.example.org") == 0);
}

/* ---------------- 4. parser: hostile wires ---------------- */

static void t_parse_attacks(void) {
    uint8_t m[512];
    dns_result_t r;
    /* compression loop: owner name points at itself */
    int o = hdr(m, 0x3333, 0x8180, 0, 1, 0);
    m[o++] = 0xC0; m[o++] = 12;                          /* -> offset 12 */
    memset(m + o, 0, 10);
    o += 12;
    CHECK_EQ(dns_parse_response(m, o, "x", 0x3333, &r), DNS_PARSE_BAD);
    /* out-of-range pointer */
    o = hdr(m, 0x3334, 0x8180, 0, 1, 0);
    m[o++] = 0xC0; m[o++] = 0xF0;                        /* -> offset 240 */
    memset(m + o, 0, 10);
    o += 12;
    CHECK_EQ(dns_parse_response(m, o, "x", 0x3334, &r), DNS_PARSE_BAD);
    /* truncated RR (rdlen runs past the buffer) */
    o = hdr(m, 0x3335, 0x8180, 0, 1, 0);
    o = rr_hdr_ptr(m, o, DNS_RTYPE_A, 60, 500);
    o += 4;                                              /* only 4 of 500 */
    CHECK_EQ(dns_parse_response(m, o, "x", 0x3335, &r), DNS_PARSE_BAD);
}

/* ---------------- 5. parser: NXDOMAIN / NODATA with SOA ---------------- */

static void t_parse_negative(void) {
    uint8_t m[512];
    /* NXDOMAIN, authority SOA ttl=1800 minimum=300 -> neg_ttl 300 */
    int o = hdr(m, 0x4444, 0x8183, 1, 0, 1);
    o = question(m, o, "no-such-host.example");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_SOA, 1800, 20);
    memset(m + o, 0, 20);
    m[o + 16] = 0; m[o + 17] = 0; m[o + 18] = 1; m[o + 19] = 44;  /* minimum=300 */
    o += 20;

    dns_result_t r;
    CHECK_EQ(dns_parse_response(m, o, "no-such-host.example", 0x4444, &r),
             DNS_PARSE_NXDOMAIN);
    CHECK_EQ(r.neg_ttl, 300);

    /* NODATA: NOERROR, no answers, SOA ttl=120 minimum=600 -> neg_ttl 120 */
    o = hdr(m, 0x4445, 0x8180, 1, 0, 1);
    o = question(m, o, "empty.example");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_SOA, 120, 20);
    memset(m + o, 0, 20);
    m[o + 16] = 0; m[o + 17] = 0; m[o + 18] = 2; m[o + 19] = 88;  /* minimum=600 */
    o += 20;
    CHECK_EQ(dns_parse_response(m, o, "empty.example", 0x4445, &r),
             DNS_PARSE_NODATA);
    CHECK_EQ(r.neg_ttl, 120);
}

/* ---------------- 6. cache core (deterministic clock) ---------------- */

static void t_cache_basic(void) {
    dns_cache_t c; dns_cache_reset(&c);
    uint32_t ip = 0; int neg = 0; uint32_t ttl = 0;

    CHECK_EQ(dns_cache_lookup(&c, "example.com", g_now, 100, &ip, &neg, &ttl), 0);
    dns_cache_insert(&c, "Example.COM", IP(1, 2, 3, 4), 0, 300, g_now, 100);
    CHECK_EQ(dns_cache_count(&c), 1);
    CHECK_EQ(dns_cache_lookup(&c, "example.com", g_now, 100, &ip, &neg, &ttl), 1);
    CHECK_EQ(ip, IP(1, 2, 3, 4));
    CHECK_EQ(neg, 0);
    CHECK_EQ(ttl, 300);
    /* case-insensitive both ways */
    CHECK_EQ(dns_cache_lookup(&c, "EXAMPLE.COM", g_now, 100, &ip, &neg, &ttl), 1);
    /* TTL ticks down */
    advance_sec(120);
    CHECK_EQ(dns_cache_lookup(&c, "example.com", g_now, 100, &ip, &neg, &ttl), 1);
    CHECK_EQ(ttl, 180);
    /* expiry: gate scenario — present-but-expired is re-queried */
    advance_sec(181);
    CHECK_EQ(dns_cache_lookup(&c, "example.com", g_now, 100, &ip, &neg, &ttl), -1);
    CHECK_EQ(dns_cache_count(&c), 0);
}

static void t_cache_policy(void) {
    dns_cache_t c; dns_cache_reset(&c);
    /* TTL 0 is never stored (RFC 2181) */
    dns_cache_insert(&c, "zero.ttl", IP(5, 6, 7, 8), 0, 0, g_now, 100);
    CHECK_EQ(dns_cache_count(&c), 0);
    /* TTL is capped */
    dns_cache_insert(&c, "huge.ttl", IP(5, 6, 7, 8), 0, 99999999, g_now, 100);
    uint32_t ttl = 0;
    CHECK_EQ(dns_cache_lookup(&c, "huge.ttl", g_now, 100, 0, 0, &ttl), 1);
    CHECK_EQ(ttl, DNS_TTL_CAP);
    /* negative entry round-trip */
    dns_cache_insert(&c, "absent.name", 0, 1, 60, g_now, 100);
    uint32_t ip = 1; int neg = 0;
    CHECK_EQ(dns_cache_lookup(&c, "absent.name", g_now, 100, &ip, &neg, &ttl), 1);
    CHECK_EQ(neg, 1);
    CHECK_EQ(ip, 0);
    /* replacement of an existing name refreshes the value */
    dns_cache_insert(&c, "absent.name", IP(9, 9, 9, 9), 0, 100, g_now, 100);
    CHECK_EQ(dns_cache_count(&c), 2);
    CHECK_EQ(dns_cache_lookup(&c, "absent.name", g_now, 100, &ip, &neg, &ttl), 1);
    CHECK_EQ(ip, IP(9, 9, 9, 9));
    CHECK_EQ(neg, 0);
}

static void t_cache_lru(void) {
    dns_cache_t c; dns_cache_reset(&c);
    char nm[64];
    for (int i = 0; i < DNS_CACHE_MAX; i++) {
        snprintf(nm, sizeof(nm), "host%d.example", i);
        dns_cache_insert(&c, nm, IP(10, 0, 0, i), 0, 1000, g_now, 100);
        g_now += 10;                                     /* distinct LRU ages */
    }
    CHECK_EQ(dns_cache_count(&c), DNS_CACHE_MAX);
    /* touch host0 so host1 becomes oldest-unused */
    CHECK_EQ(dns_cache_lookup(&c, "host0.example", g_now, 100, 0, 0, 0), 1);
    dns_cache_insert(&c, "newcomer.example", IP(1, 1, 1, 1), 0, 1000, g_now, 100);
    CHECK_EQ(dns_cache_count(&c), DNS_CACHE_MAX);
    CHECK_EQ(dns_cache_lookup(&c, "host0.example", g_now, 100, 0, 0, 0), 1);
    CHECK_EQ(dns_cache_lookup(&c, "host1.example", g_now, 100, 0, 0, 0), 0);
    CHECK_EQ(dns_cache_lookup(&c, "newcomer.example", g_now, 100, 0, 0, 0), 1);
}

/* ---------------- 7. resolver, scripted transport ---------------- */

#define MAX_CALLS 8
static struct {
    uint32_t want_server;      /* 0 = any */
    char     want_qname[DNS_MAX_NAME + 1];
    int      retval;           /* -1 timeout/silent, else response length */
    uint8_t  resp[1024];
} script[MAX_CALLS];
static int script_len, script_pos;
static uint32_t seen_servers[MAX_CALLS];

static int scripted_transport(uint32_t server, const char *qname,
                              uint8_t *resp, int cap) {
    if (script_pos >= script_len) { failed++;
        printf("  FAIL: unexpected extra query to %u.%u.%u.%u for '%s'\n",
               server >> 24, (server >> 16) & 0xFF, (server >> 8) & 0xFF,
               server & 0xFF, qname);
        return -1;
    }
    int i = script_pos++;
    if (i < MAX_CALLS) seen_servers[i] = server;
    if (script[i].want_qname[0] &&
        strcmp(script[i].want_qname, qname) != 0) {
        printf("  FAIL: query for '%s', expected '%s'\n",
               qname, script[i].want_qname);
        failed++;
    }
    if (script[i].retval < 0) return -1;
    if (script[i].retval > cap) { failed++; return -1; }
    memcpy(resp, script[i].resp, (size_t)script[i].retval);
    return script[i].retval;
}

static void script_reset(void) {
    memset(script, 0, sizeof(script));
    memset(seen_servers, 0, sizeof(seen_servers));
    script_len = script_pos = 0;
}

static void add_response(const char *qname, const uint8_t *msg, int len) {
    int i = script_len++;
    strcpy(script[i].want_qname, qname);
    script[i].retval = len;
    memcpy(script[i].resp, msg, (size_t)len);
}

static void add_silent(const char *qname) {
    int i = script_len++;
    strcpy(script[i].want_qname, qname);
    script[i].retval = -1;
}

static int build_simple_a(uint8_t *m, uint16_t flags,
                          const char *qname, uint32_t ip, uint32_t ttl) {
    int o = hdr(m, 0, flags, 1, 1, 0);
    o = question(m, o, qname);
    o = rr_hdr_ptr(m, o, DNS_RTYPE_A, ttl, 4);
    m[o++] = (ip >> 24) & 0xFF; m[o++] = (ip >> 16) & 0xFF;
    m[o++] = (ip >> 8) & 0xFF;  m[o++] = ip & 0xFF;
    return o;
}

static void t_resolve_cache_hit(void) {
    script_reset();
    dns_init();
    g_now = 100000;
    uint8_t m[512];
    int n = build_simple_a(m, 0x8180, "example.com", IP(93, 184, 216, 34), 300);
    add_response("example.com", m, n);
    dns_set_transport_for_tests(scripted_transport);

    uint32_t ip1 = dns_resolve_ipv4("example.com");
    CHECK_EQ(ip1, IP(93, 184, 216, 34));
    CHECK_EQ(script_pos, 1);
    uint32_t ip2 = dns_resolve_ipv4("example.com");   /* served from cache */
    CHECK_EQ(ip2, ip1);
    CHECK_EQ(script_pos, 1);                          /* no new query */

    advance_sec(301);                                  /* TTL expires */
    script_reset();
    n = build_simple_a(m, 0x8180, "example.com", IP(93, 184, 216, 35), 300);
    add_response("example.com", m, n);
    uint32_t ip3 = dns_resolve_ipv4("example.com");   /* re-queried */
    CHECK_EQ(script_pos, 1);
    CHECK_EQ(ip3, IP(93, 184, 216, 35));
}

static void t_resolve_failover(void) {
    script_reset();
    dns_init();
    uint32_t servers[2] = { IP(10, 0, 2, 3), IP(10, 0, 2, 4) };
    dns_set_servers(servers, 2);
    uint8_t m[512];
    int n = build_simple_a(m, 0x8180, "failover.example", IP(7, 7, 7, 7), 60);
    add_silent("failover.example");            /* primary: 2 silent attempts */
    add_silent("failover.example");
    add_response("failover.example", m, n);    /* secondary answers */
    dns_set_transport_for_tests(scripted_transport);

    uint32_t ip = dns_resolve_ipv4("failover.example");
    CHECK_EQ(ip, IP(7, 7, 7, 7));
    CHECK_EQ(script_pos, 3);
    CHECK_EQ(seen_servers[0], IP(10, 0, 2, 3));
    CHECK_EQ(seen_servers[1], IP(10, 0, 2, 3));
    CHECK_EQ(seen_servers[2], IP(10, 0, 2, 4));
}

static void t_resolve_cname_requery(void) {
    script_reset();
    dns_init();
    /* response 1: CNAME only (no A) -> resolver re-queries the target */
    uint8_t m1[512];
    int o = hdr(m1, 0, 0x8180, 1, 1, 0);
    o = question(m1, o, "www.alias.test");
    o = rr_hdr_ptr(m1, o, DNS_RTYPE_CNAME, 90, 0);
    int rdlen_at = o - 2, tgt = o;
    o += dns_encode_name("real.target.test", m1 + o, 512 - o);
    m1[rdlen_at] = 0; m1[rdlen_at + 1] = (uint8_t)(o - tgt);
    add_response("www.alias.test", m1, o);

    uint8_t m2[512];
    int n2 = build_simple_a(m2, 0x8180, "real.target.test", IP(8, 8, 4, 4), 200);
    add_response("real.target.test", m2, n2);
    dns_set_transport_for_tests(scripted_transport);

    uint32_t ip = dns_resolve_ipv4("www.alias.test");
    CHECK_EQ(ip, IP(8, 8, 4, 4));
    CHECK_EQ(script_pos, 2);                    /* CNAME drove a second query */
}

static void t_resolve_negative(void) {
    script_reset();
    dns_init();
    /* NXDOMAIN with SOA (neg_ttl 300) */
    uint8_t m[512];
    int o = hdr(m, 0, 0x8183, 1, 0, 1);
    o = question(m, o, "no-such.test");
    o = rr_hdr_ptr(m, o, DNS_RTYPE_SOA, 1800, 20);
    memset(m + o, 0, 20);
    m[o + 19] = 44;                                              /* minimum 300 */
    o += 20;
    add_response("no-such.test", m, o);
    dns_set_transport_for_tests(scripted_transport);

    CHECK_EQ(dns_resolve_ipv4("no-such.test"), 0);
    CHECK_EQ(script_pos, 1);
    CHECK_EQ(dns_resolve_ipv4("no-such.test"), 0);   /* negative cache HIT */
    CHECK_EQ(script_pos, 1);                         /* still one query */
}

static void t_resolve_truncated_and_dead(void) {
    script_reset();
    dns_init();
    uint8_t m[512];
    int n = build_simple_a(m, 0x8380, "big.test", IP(1, 1, 1, 1), 60);  /* TC=0x0200 */
    add_response("big.test", m, n);
    dns_set_transport_for_tests(scripted_transport);
    CHECK_EQ(dns_resolve_ipv4("big.test"), 0);       /* loud, no wrong answer */

    script_reset();
    dns_init();
    add_silent("dead.test");
    add_silent("dead.test");
    CHECK_EQ(dns_resolve_ipv4("dead.test"), 0);      /* all servers silent */
    CHECK_EQ(script_pos, 2);                         /* 1 server x 2 attempts */
}

int main(void) {
    RUN(t_encode);
    RUN(t_parse_a);
    RUN(t_parse_cname_chain);
    RUN(t_parse_cname_no_a);
    RUN(t_parse_attacks);
    RUN(t_parse_negative);
    RUN(t_cache_basic);
    RUN(t_cache_policy);
    RUN(t_cache_lru);
    RUN(t_resolve_cache_hit);
    RUN(t_resolve_failover);
    RUN(t_resolve_cname_requery);
    RUN(t_resolve_negative);
    RUN(t_resolve_truncated_and_dead);

    printf("test_dns: %d/%d scenarios passed\n", passed, tn);
    if (failed == 0) { printf("PASS: 0 failures\n"); return 0; }
    printf("FAIL: %d check(s) failed\n", failed);
    return 1;
}
