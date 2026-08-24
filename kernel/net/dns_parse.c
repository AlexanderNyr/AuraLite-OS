/*
 * dns_parse.c — pure DNS wire-format parser and TTL cache core.
 * No kernel dependencies; see dns_parse.h for the contract.
 */

#include "kernel/net/dns_parse.h"

#ifdef AURALITE_DNS_HOST_TEST
#include <string.h>
#else
#include "kernel/lib/string.h"
#endif

#define DNS_MAX_JUMPS 128   /* compression-pointer budget (loop refusal) */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static char lc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (lc(*a) != lc(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int dns_encode_name(const char *name, uint8_t *out, int out_cap) {
    if (!name || !out || !*name) return -1;
    int pos = 0;
    const char *seg = name;
    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - seg);
        if (len > 63 || len == 0) return -1;
        if (pos + 1 + len >= out_cap) return -1;
        out[pos++] = (uint8_t)len;
        for (int i = 0; i < len; i++) out[pos++] = (uint8_t)lc(seg[i]);
        seg = dot;
        if (*seg == '.') seg++;
    }
    if (pos + 1 >= out_cap) return -1;
    out[pos++] = 0;
    if (pos > DNS_MAX_NAME + 1) return -1;
    return pos;
}

/*
 * Read a (possibly compressed) DNS name at `pos` and render it dotted into
 * `out` (out_cap).  Returns 0 on success and stores the offset just past the
 * name's on-wire extent at the starting level into *end_pos (where a caller
 * finds the following fields).  Returns -1 on truncation, an out-of-range
 * pointer, a pointer loop (budget exhausted), or an overlong name.
 */
static int dns_name_read(const uint8_t *msg, int len, int pos,
                         char *out, int out_cap, int *end_pos) {
    int o = 0;
    int jumps = 0;
    int first_end = -1;
    int cur = pos;
    if (out_cap <= 0) return -1;
    for (;;) {
        if (cur >= len) return -1;
        uint8_t b = msg[cur];
        if ((b & 0xC0) == 0xC0) {
            if (cur + 1 >= len) return -1;
            uint16_t ptr = (uint16_t)(((b & 0x3F) << 8) | msg[cur + 1]);
            if (ptr >= (uint16_t)len) return -1;
            if (first_end < 0) first_end = cur + 2;
            if (++jumps > DNS_MAX_JUMPS) return -1;
            cur = ptr;
            continue;
        }
        if (b == 0) {
            if (o >= out_cap) return -1;
            out[o++] = 0;
            if (first_end < 0) first_end = cur + 1;
            if (o - 1 > DNS_MAX_NAME) return -1;
            *end_pos = first_end;
            return 0;
        }
        int llen = b;
        if (cur + 1 + llen > len) return -1;
        if (o > 0) {
            if (o >= out_cap) return -1;
            out[o++] = '.';
        }
        if (o + llen >= out_cap) return -1;
        for (int i = 0; i < llen; i++) out[o++] = lc((char)msg[cur + 1 + i]);
        cur += 1 + llen;
    }
}

static void copy_lower(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = lc(src[i]);
    dst[i] = 0;
}

/* RFC 2308: negative TTL = min(SOA.ttl, SOA.minimum).  0 when no SOA. */
static uint32_t find_soa_neg_ttl(const uint8_t *msg, int len, int pos, int ns_count) {
    for (int i = 0; i < ns_count; i++) {
        char owner[DNS_MAX_NAME + 1];
        int end;
        if (dns_name_read(msg, len, pos, owner, sizeof(owner), &end) != 0) return 0;
        pos = end;
        if (pos + 10 > len) return 0;
        uint16_t type  = rd16(msg + pos);
        uint16_t klass = rd16(msg + pos + 2);
        uint32_t ttl   = rd32(msg + pos + 4);
        uint16_t rdlen = rd16(msg + pos + 8);
        if (pos + 10 + rdlen > len) return 0;
        if (type == DNS_RTYPE_SOA && klass == 1 && rdlen >= 20) {
            uint32_t minimum = rd32(msg + pos + 10 + rdlen - 4);
            return ttl < minimum ? ttl : minimum;
        }
        pos += 10 + rdlen;
    }
    return 0;
}

int dns_parse_response(const uint8_t *msg, int len, const char *qname,
                       uint16_t expect_id, dns_result_t *out) {
    if (!msg || len < 12 || !qname || !out) return DNS_PARSE_BAD;
    memset(out, 0, sizeof(*out));

    if (rd16(msg + 0) != expect_id) return DNS_PARSE_BAD;
    uint16_t flags = rd16(msg + 2);
    if (!(flags & 0x8000))          return DNS_PARSE_BAD;   /* QR: response */
    if (flags & 0x0200)             return DNS_PARSE_TRUNCATED;
    uint16_t rcode = (uint16_t)(flags & 0x000F);
    uint16_t qd = rd16(msg + 4), an = rd16(msg + 6), ns = rd16(msg + 8);

    int pos = 12;
    for (int i = 0; i < qd; i++) {
        char qn[DNS_MAX_NAME + 1];
        int end;
        if (dns_name_read(msg, len, pos, qn, sizeof(qn), &end) != 0)
            return DNS_PARSE_BAD;
        pos = end + 4;   /* QTYPE + QCLASS */
        if (pos > len) return DNS_PARSE_BAD;
    }

    char expected[DNS_MAX_NAME + 1];
    copy_lower(expected, sizeof(expected), qname);

    uint32_t chain_min = 0xFFFFFFFFu;
    int chained = 0;
    int auth_pos = pos;   /* recomputed as we walk; authority start after answers */

    for (int i = 0; i < an; i++) {
        char owner[DNS_MAX_NAME + 1];
        int end;
        if (dns_name_read(msg, len, pos, owner, sizeof(owner), &end) != 0)
            return DNS_PARSE_BAD;
        pos = end;
        if (pos + 10 > len) return DNS_PARSE_BAD;
        uint16_t type  = rd16(msg + pos);
        uint16_t klass = rd16(msg + pos + 2);
        uint32_t ttl   = rd32(msg + pos + 4);
        uint16_t rdlen = rd16(msg + pos + 8);
        int rdata = pos + 10;
        if (rdata + rdlen > len) return DNS_PARSE_BAD;

        if (klass == 1 && type == DNS_RTYPE_CNAME && ci_equal(owner, expected)) {
            char target[DNS_MAX_NAME + 1];
            int tend;
            if (dns_name_read(msg, len, rdata, target, sizeof(target), &tend) != 0)
                return DNS_PARSE_BAD;
            if (ttl < chain_min) chain_min = ttl;
            copy_lower(expected, sizeof(expected), target);
            chained = 1;
        } else if (klass == 1 && type == DNS_RTYPE_A && rdlen == 4 &&
                   ci_equal(owner, expected)) {
            out->a_ip  = rd32(msg + rdata);
            uint32_t m = chained ? chain_min : ttl;
            out->a_ttl = ttl < m ? ttl : m;
            /* keep walking: answers are done for our chain, but the buffer
             * must stay consistent for the authority offset below */
            chained = 0;   /* A terminates the chase */
            /* authoritative answer found; record authority start after loop */
            pos = rdata + rdlen;
            auth_pos = pos;
            /* terminal A wins immediately — later RRs cannot be CNAMEs
             * belonging to this chain anymore */
            if (rcode == DNS_RCODE_NOERROR) {
                out->neg_ttl = 0;
                return DNS_PARSE_ANSWER;
            }
            return DNS_PARSE_ANSWER;
        }
        pos = rdata + rdlen;
        auth_pos = pos;
    }

    if (rcode == DNS_RCODE_NXDOMAIN) {
        out->neg_ttl = find_soa_neg_ttl(msg, len, auth_pos, ns);
        return DNS_PARSE_NXDOMAIN;
    }
    if (chained) {
        copy_lower(out->cname_target, sizeof(out->cname_target), expected);
        return DNS_PARSE_CNAME;
    }
    out->neg_ttl = find_soa_neg_ttl(msg, len, auth_pos, ns);
    return DNS_PARSE_NODATA;
}

int dns_parse_aaaa(const uint8_t *msg, int len, const char *qname,
                   uint16_t expect_id, uint8_t out_aaaa[16], uint32_t *out_ttl) {
    if (!msg || len < 12 || !qname || !out_aaaa) return DNS_PARSE_BAD;
    if (rd16(msg + 0) != expect_id) return DNS_PARSE_BAD;
    uint16_t flags = rd16(msg + 2);
    if (!(flags & 0x8000)) return DNS_PARSE_BAD;
    if (flags & 0x0200)    return DNS_PARSE_TRUNCATED;
    uint16_t rcode = (uint16_t)(flags & 0x000F);
    uint16_t qd = rd16(msg + 4), an = rd16(msg + 6);
    int pos = 12;
    int i;
    char expected[DNS_MAX_NAME + 1];

    for (i = 0; i < qd; i++) {
        char qn[DNS_MAX_NAME + 1];
        int end;
        if (dns_name_read(msg, len, pos, qn, sizeof(qn), &end) != 0)
            return DNS_PARSE_BAD;
        pos = end + 4;
        if (pos > len) return DNS_PARSE_BAD;
    }
    copy_lower(expected, sizeof(expected), qname);

    for (i = 0; i < an; i++) {
        char owner[DNS_MAX_NAME + 1];
        int end;
        uint16_t type, klass, rdlen;
        uint32_t ttl;
        int rdata;
        if (dns_name_read(msg, len, pos, owner, sizeof(owner), &end) != 0)
            return DNS_PARSE_BAD;
        pos = end;
        if (pos + 10 > len) return DNS_PARSE_BAD;
        type  = rd16(msg + pos);
        klass = rd16(msg + pos + 2);
        ttl   = rd32(msg + pos + 4);
        rdlen = rd16(msg + pos + 8);
        rdata = pos + 10;
        if (rdata + rdlen > len) return DNS_PARSE_BAD;
        if (klass == 1 && type == DNS_RTYPE_AAAA && rdlen == 16 &&
            ci_equal(owner, expected)) {
            int k;
            for (k = 0; k < 16; k++) out_aaaa[k] = msg[rdata + k];
            if (out_ttl) *out_ttl = ttl;
            return DNS_PARSE_ANSWER;
        }
        pos = rdata + rdlen;
    }
    if (rcode == DNS_RCODE_NXDOMAIN) return DNS_PARSE_NXDOMAIN;
    return DNS_PARSE_NODATA;
}

/* ---- cache core ---- */

void dns_cache_reset(dns_cache_t *c) {
    memset(c, 0, sizeof(*c));
}

static int cache_find(dns_cache_t *c, const char *key) {
    for (int i = 0; i < DNS_CACHE_MAX; i++)
        if (c->e[i].used && ci_equal(c->e[i].name, key)) return i;
    return -1;
}

int dns_cache_lookup(dns_cache_t *c, const char *name,
                     uint64_t now_ticks, uint32_t ticks_per_sec,
                     uint32_t *out_ip, int *out_neg, uint32_t *out_ttl_left) {
    char key[DNS_MAX_NAME + 1];
    copy_lower(key, sizeof(key), name);
    int i = cache_find(c, key);
    if (i < 0) return 0;
    dns_cache_entry_t *e = &c->e[i];
    if (now_ticks >= e->expires_at) {
        e->used = 0;
        return -1;
    }
    e->last_used = ++c->seq;
    if (out_ip)       *out_ip = e->ip;
    if (out_neg)      *out_neg = e->negative;
    if (out_ttl_left && ticks_per_sec)
        *out_ttl_left = (uint32_t)((e->expires_at - now_ticks) / ticks_per_sec);
    return 1;
}

void dns_cache_insert(dns_cache_t *c, const char *name, uint32_t ip,
                      int negative, uint32_t ttl_sec,
                      uint64_t now_ticks, uint32_t ticks_per_sec) {
    if (!name || !*name) return;
    if (ttl_sec == 0) return;                    /* RFC 2181: do not cache */
    if (ttl_sec > DNS_TTL_CAP) ttl_sec = DNS_TTL_CAP;

    char key[DNS_MAX_NAME + 1];
    copy_lower(key, sizeof(key), name);

    /* Drop expired entries first: their slots are preferred. */
    for (int i = 0; i < DNS_CACHE_MAX; i++)
        if (c->e[i].used && now_ticks >= c->e[i].expires_at) c->e[i].used = 0;

    int slot = cache_find(c, key);
    if (slot < 0) {
        slot = -1;
        for (int i = 0; i < DNS_CACHE_MAX; i++)
            if (!c->e[i].used) { slot = i; break; }
        if (slot < 0) {                         /* full: evict LRU */
            uint64_t oldest = ~0ULL;
            for (int i = 0; i < DNS_CACHE_MAX; i++)
                if (c->e[i].last_used < oldest) {
                    oldest = c->e[i].last_used;
                    slot = i;
                }
        }
    }

    dns_cache_entry_t *e = &c->e[slot];
    memset(e, 0, sizeof(*e));
    int i = 0;
    for (; key[i] && i < DNS_MAX_NAME; i++) e->name[i] = key[i];
    e->name[i] = 0;
    e->ip        = ip;
    e->negative  = (uint8_t)(negative ? 1 : 0);
    e->used      = 1;
    e->expires_at = now_ticks + (uint64_t)ttl_sec * ticks_per_sec;
    e->last_used  = ++c->seq;
}

int dns_cache_count(const dns_cache_t *c) {
    int n = 0;
    for (int i = 0; i < DNS_CACHE_MAX; i++) n += c->e[i].used ? 1 : 0;
    return n;
}

const dns_cache_entry_t *dns_cache_entry(const dns_cache_t *c, int idx) {
    if (idx < 0 || idx >= DNS_CACHE_MAX || !c->e[idx].used) return 0; /* NULL */
    return &c->e[idx];
}
