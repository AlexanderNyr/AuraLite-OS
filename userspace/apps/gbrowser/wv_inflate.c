/*
 * wv_inflate.c — RFC 1951 inflate.  See wv_inflate.h.
 *
 * Stored, fixed, and dynamic blocks.  A 32 KiB sliding window lives in
 * the output buffer itself (PNG writes sequentially, so already-written
 * bytes are the window).
 */
#include "wv_inflate.h"

typedef struct {
    const uint8_t *in;
    size_t in_len, in_off;
    unsigned bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_cap, out_len;
} inf_t;

static int inf_need(inf_t *s, int n) {
    while (s->bitcnt < n) {
        if (s->in_off >= s->in_len) return 0;
        s->bitbuf |= (unsigned)s->in[s->in_off++] << s->bitcnt;
        s->bitcnt += 8;
    }
    return 1;
}

static unsigned inf_bits(inf_t *s, int n) {
    if (!inf_need(s, n)) return 0;
    unsigned v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void inf_align(inf_t *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
}

/* Canonical Huffman: codes[sym] / nbits[sym].  Decode table is a
 * first-match walk over 288 symbols — PNG rows are short, this is fine. */
#define INF_MAX_SYM 288
#define INF_MAX_BITS 15

typedef struct {
    uint16_t count[INF_MAX_BITS + 1];
    uint16_t symbol[INF_MAX_SYM];
    uint16_t nsym;
} huf_t;

static int huf_build(huf_t *h, const uint8_t *lens, int n) {
    int i;
    for (i = 0; i <= INF_MAX_BITS; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] > INF_MAX_BITS) return 0;
        h->count[lens[i]]++;
    }
    /* Over-subscribed / incomplete trees: a single unused alphabet is
     * legal (all-zero).  A tree with codes that cannot be assigned is
     * refused so a hostile IDAT cannot loop the decoder. */
    if (h->count[0] == n) { h->nsym = 0; return 1; }
    unsigned left = 1;
    for (i = 1; i <= INF_MAX_BITS; i++) {
        left <<= 1;
        if (h->count[i] > left) return 0;
        left -= h->count[i];
    }
    h->count[0] = 0;
    for (i = 0; i < n; i++)
        h->symbol[i] = lens[i];
    h->nsym = (uint16_t)n;
    return 1;
}

static int huf_decode(inf_t *s, const huf_t *h) {
    unsigned code = 0;
    int len;
    unsigned next_code[INF_MAX_BITS + 1];
    unsigned c = 0;
    int i;
    next_code[0] = 0;
    for (i = 1; i <= INF_MAX_BITS; i++) {
        c = (c + h->count[i - 1]) << 1;
        next_code[i] = c;
    }
    for (len = 1; len <= INF_MAX_BITS; len++) {
        if (!inf_need(s, 1)) return -1;
        code = (code << 1) | (s->bitbuf & 1u);
        s->bitbuf >>= 1;
        s->bitcnt--;
        if (h->count[len] == 0) continue;
        unsigned first = next_code[len];
        unsigned last = first + h->count[len];
        if (code < first || code >= last) continue;
        /* find the symbol with this length whose code equals `code` */
        unsigned walk = first;
        int sym;
        for (sym = 0; sym < h->nsym; sym++) {
            if (h->symbol[sym] != (uint16_t)len) continue;
            if (walk == code) return sym;
            walk++;
        }
        return -1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inf_put(inf_t *s, uint8_t b) {
    if (s->out_len >= s->out_cap) return 0;
    s->out[s->out_len++] = b;
    return 1;
}

static int inf_copy(inf_t *s, unsigned dist, unsigned len) {
    if (dist == 0 || dist > s->out_len) return 0;
    unsigned i;
    for (i = 0; i < len; i++) {
        if (!inf_put(s, s->out[s->out_len - dist])) return 0;
    }
    return 1;
}

static int inf_codes(inf_t *s, const huf_t *lit, const huf_t *dist) {
    for (;;) {
        int sym = huf_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (!inf_put(s, (uint8_t)sym)) return 0;
            continue;
        }
        if (sym == 256) return 1;
        if (sym > 285) return 0;
        unsigned li = (unsigned)(sym - 257);
        unsigned length = LEN_BASE[li] + inf_bits(s, LEN_EXTRA[li]);
        int ds = huf_decode(s, dist);
        if (ds < 0 || ds > 29) return 0;
        unsigned distance = DIST_BASE[ds] + inf_bits(s, DIST_EXTRA[ds]);
        if (!inf_copy(s, distance, length)) return 0;
    }
}

static int inf_fixed(inf_t *s) {
    uint8_t ll[288], dd[32];
    int i;
    for (i = 0; i <= 143; i++) ll[i] = 8;
    for (i = 144; i <= 255; i++) ll[i] = 9;
    for (i = 256; i <= 279; i++) ll[i] = 7;
    for (i = 280; i <= 287; i++) ll[i] = 8;
    for (i = 0; i < 32; i++) dd[i] = 5;
    huf_t lit, dist;
    if (!huf_build(&lit, ll, 288)) return 0;
    if (!huf_build(&dist, dd, 32)) return 0;
    return inf_codes(s, &lit, &dist);
}

static int inf_dynamic(inf_t *s) {
    unsigned hlit = inf_bits(s, 5) + 257;
    unsigned hdist = inf_bits(s, 5) + 1;
    unsigned hclen = inf_bits(s, 4) + 4;
    if (hlit > 286 || hdist > 32 || hclen > 19) return 0;
    static const uint8_t order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    uint8_t clen[19];
    unsigned i;
    for (i = 0; i < 19; i++) clen[i] = 0;
    for (i = 0; i < hclen; i++) clen[order[i]] = (uint8_t)inf_bits(s, 3);
    huf_t ch;
    if (!huf_build(&ch, clen, 19)) return 0;
    uint8_t lens[288 + 32];
    unsigned n = hlit + hdist;
    unsigned k = 0;
    while (k < n) {
        int sym = huf_decode(s, &ch);
        if (sym < 0) return 0;
        if (sym <= 15) {
            lens[k++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (k == 0) return 0;
            unsigned rep = 3 + inf_bits(s, 2);
            uint8_t prev = lens[k - 1];
            while (rep--) {
                if (k >= n) return 0;
                lens[k++] = prev;
            }
        } else if (sym == 17) {
            unsigned rep = 3 + inf_bits(s, 3);
            while (rep--) {
                if (k >= n) return 0;
                lens[k++] = 0;
            }
        } else if (sym == 18) {
            unsigned rep = 11 + inf_bits(s, 7);
            while (rep--) {
                if (k >= n) return 0;
                lens[k++] = 0;
            }
        } else {
            return 0;
        }
    }
    huf_t lit, dist;
    if (!huf_build(&lit, lens, (int)hlit)) return 0;
    if (!huf_build(&dist, lens + hlit, (int)hdist)) return 0;
    return inf_codes(s, &lit, &dist);
}

static int inf_stored(inf_t *s) {
    inf_align(s);
    if (s->in_off + 4 > s->in_len) return 0;
    unsigned len = (unsigned)s->in[s->in_off] | ((unsigned)s->in[s->in_off + 1] << 8);
    unsigned nlen = (unsigned)s->in[s->in_off + 2] | ((unsigned)s->in[s->in_off + 3] << 8);
    s->in_off += 4;
    if ((len ^ 0xFFFFu) != nlen) return 0;
    if (s->in_off + len > s->in_len) return 0;
    unsigned i;
    for (i = 0; i < len; i++) {
        if (!inf_put(s, s->in[s->in_off++])) return 0;
    }
    return 1;
}

size_t wv_inflate_raw(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;
    inf_t s;
    s.in = in; s.in_len = in_len; s.in_off = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.out_cap = out_cap; s.out_len = 0;
    for (;;) {
        unsigned bfinal = inf_bits(&s, 1);
        unsigned btype = inf_bits(&s, 2);
        int ok = 0;
        if (btype == 0) ok = inf_stored(&s);
        else if (btype == 1) ok = inf_fixed(&s);
        else if (btype == 2) ok = inf_dynamic(&s);
        else return 0;
        if (!ok) return 0;
        if (bfinal) return s.out_len;
    }
}

size_t wv_inflate_zlib(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap) {
    if (!in || in_len < 2) return 0;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8) return 0;           /* DEFLATE */
    if (((cmf << 8) + flg) % 31 != 0) return 0;
    if (flg & 0x20) return 0;                  /* preset dict: refuse */
    return wv_inflate_raw(in + 2, in_len - 2, out, out_cap);
}
