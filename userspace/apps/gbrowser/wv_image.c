/*
 * wv_image.c — PNG / JPEG / GIF / BMP → XRGB8888.  See wv_image.h.
 */
#include "wv_image.h"
#include "wv_inflate.h"

#include <stdlib.h>
#include <string.h>

/* ---- tiny helpers ---- */

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int clamp_i(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t pack_rgb(int r, int g, int b) {
    return ((uint32_t)clamp_i(r) << 16) |
           ((uint32_t)clamp_i(g) << 8) |
           (uint32_t)clamp_i(b);
}

static int size_ok(int w, int h) {
    if (w < 1 || h < 1 || w > WV_IMAGE_MAX_W || h > WV_IMAGE_MAX_H) return 0;
    /* w*h*4 fits in a reasonable heap allocation */
    if ((size_t)w * (size_t)h > (size_t)WV_IMAGE_MAX_W * (size_t)WV_IMAGE_MAX_H)
        return 0;
    return 1;
}

/* ---- base64 (data: URLs) ---- */

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *b64_decode(const char *s, size_t n, size_t *out_len) {
    size_t cap = n / 4 * 3 + 4;
    uint8_t *o = (uint8_t *)malloc(cap ? cap : 1);
    if (!o) return NULL;
    size_t k = 0;
    int acc = 0, bits = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) { free(o); return NULL; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k < cap) o[k++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = k;
    return o;
}

/* ---- PNG ---- */

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int png_recon(uint8_t *row, const uint8_t *prev, int nbytes, int bpp,
                     int filt) {
    int i;
    if (filt == 0) return 1;
    if (filt == 1) {
        for (i = bpp; i < nbytes; i++) row[i] = (uint8_t)(row[i] + row[i - bpp]);
        return 1;
    }
    if (filt == 2) {
        if (!prev) return 0;
        for (i = 0; i < nbytes; i++) row[i] = (uint8_t)(row[i] + prev[i]);
        return 1;
    }
    if (filt == 3) {
        for (i = 0; i < nbytes; i++) {
            int a = (i >= bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            row[i] = (uint8_t)(row[i] + ((a + b) / 2));
        }
        return 1;
    }
    if (filt == 4) {
        for (i = 0; i < nbytes; i++) {
            int a = (i >= bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            row[i] = (uint8_t)(row[i] + paeth(a, b, c));
        }
        return 1;
    }
    return 0;
}

static int decode_png(const uint8_t *d, size_t n, uint32_t **px, int *ow, int *oh) {
    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    if (n < 8 || memcmp(d, sig, 8) != 0) return -1;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256 * 3];
    uint8_t trns[256];
    int npal = 0, ntrns = 0;
    memset(trns, 255, sizeof trns);
    /* Concatenate IDAT.  Cap at 1 MiB of compressed bytes. */
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    size_t off = 8;
    while (off + 12 <= n) {
        uint32_t clen = rd32be(d + off);
        if (off + 12 + clen > n) { free(idat); return -1; }
        const uint8_t *type = d + off + 4;
        const uint8_t *data = d + off + 8;
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (clen < 13) { free(idat); return -1; }
            w = (int)rd32be(data);
            h = (int)rd32be(data + 4);
            depth = data[8];
            ctype = data[9];
            if (data[10] != 0 || data[11] != 0) { free(idat); return -1; }
            interlace = data[12];
            if (interlace != 0) { free(idat); return -1; }
            if (!size_ok(w, h)) { free(idat); return -1; }
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            if (clen % 3) { free(idat); return -1; }
            npal = (int)(clen / 3);
            if (npal > 256) npal = 256;
            memcpy(pal, data, (size_t)npal * 3);
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            ntrns = (int)clen;
            if (ntrns > 256) ntrns = 256;
            memcpy(trns, data, (size_t)ntrns);
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (idat_len + clen > 1024u * 1024u) { free(idat); return -1; }
            if (idat_len + clen > idat_cap) {
                size_t nc = idat_cap ? idat_cap * 2 : 4096;
                while (nc < idat_len + clen) nc *= 2;
                uint8_t *p = (uint8_t *)realloc(idat, nc);
                if (!p) { free(idat); return -1; }
                idat = p; idat_cap = nc;
            }
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        off += 12 + clen;
    }
    if (!idat || w < 1 || h < 1) { free(idat); return -1; }

    int chans = 0;
    if (ctype == 0) chans = 1;
    else if (ctype == 2) chans = 3;
    else if (ctype == 3) chans = 1;
    else if (ctype == 4) chans = 2;
    else if (ctype == 6) chans = 4;
    else { free(idat); return -1; }
    if (depth != 8 && !(ctype == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8))) {
        free(idat); return -1;
    }
    int bpp = (ctype == 3) ? 1 : chans;          /* bytes per pixel at depth 8 */
    if (ctype == 3 && depth < 8) bpp = 1;
    int row_raw;
    if (ctype == 3 && depth < 8)
        row_raw = (w * depth + 7) / 8;
    else
        row_raw = w * chans;
    size_t raw_need = (size_t)h * (size_t)(row_raw + 1);
    uint8_t *raw = (uint8_t *)malloc(raw_need ? raw_need : 1);
    if (!raw) { free(idat); return -1; }
    size_t got = wv_inflate_zlib(idat, idat_len, raw, raw_need);
    free(idat);
    if (got < raw_need) { free(raw); return -1; }

    uint32_t *out = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out) { free(raw); return -1; }
    uint8_t *prev = (uint8_t *)calloc((size_t)row_raw + 1, 1);
    uint8_t *cur = (uint8_t *)malloc((size_t)row_raw + 1);
    if (!prev || !cur) { free(raw); free(out); free(prev); free(cur); return -1; }

    int y;
    for (y = 0; y < h; y++) {
        const uint8_t *src = raw + (size_t)y * (size_t)(row_raw + 1);
        int filt = src[0];
        memcpy(cur, src + 1, (size_t)row_raw);
        if (!png_recon(cur, y ? prev : NULL, row_raw, bpp, filt)) {
            free(raw); free(out); free(prev); free(cur); return -1;
        }
        int x;
        for (x = 0; x < w; x++) {
            int r = 0, g = 0, b = 0, a = 255;
            if (ctype == 0) {
                r = g = b = cur[x];
            } else if (ctype == 2) {
                r = cur[x * 3]; g = cur[x * 3 + 1]; b = cur[x * 3 + 2];
            } else if (ctype == 4) {
                r = g = b = cur[x * 2]; a = cur[x * 2 + 1];
            } else if (ctype == 6) {
                r = cur[x * 4]; g = cur[x * 4 + 1];
                b = cur[x * 4 + 2]; a = cur[x * 4 + 3];
            } else { /* palette */
                int idx;
                if (depth == 8) idx = cur[x];
                else {
                    int bit = 8 - depth - (x * depth) % 8;
                    idx = (cur[(x * depth) / 8] >> bit) & ((1 << depth) - 1);
                }
                if (idx >= npal) idx = 0;
                r = pal[idx * 3]; g = pal[idx * 3 + 1]; b = pal[idx * 3 + 2];
                if (idx < ntrns) a = trns[idx];
            }
            if (a < 255) {
                /* against white paper */
                r = (r * a + 255 * (255 - a)) / 255;
                g = (g * a + 255 * (255 - a)) / 255;
                b = (b * a + 255 * (255 - a)) / 255;
            }
            out[(size_t)y * (size_t)w + (size_t)x] = pack_rgb(r, g, b);
        }
        memcpy(prev, cur, (size_t)row_raw);
    }
    free(raw); free(prev); free(cur);
    *px = out; *ow = w; *oh = h;
    return 0;
}

/* ---- BMP ---- */

static int decode_bmp(const uint8_t *d, size_t n, uint32_t **px, int *ow, int *oh) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off = rd32le(d + 10);
    uint32_t dib = rd32le(d + 14);
    if (dib < 40 || off >= n) return -1;
    int w = (int)rd32le(d + 18);
    int hsig = (int)rd32le(d + 22);
    int topdown = 0;
    int h = hsig;
    if (h < 0) { h = -h; topdown = 1; }
    uint16_t planes = rd16le(d + 26);
    uint16_t bpp = rd16le(d + 28);
    uint32_t comp = rd32le(d + 30);
    if (planes != 1 || (bpp != 24 && bpp != 32) || comp != 0) return -1;
    if (!size_ok(w, h)) return -1;
    int rowb = ((w * (int)bpp + 31) / 32) * 4;
    if (off + (size_t)rowb * (size_t)h > n) return -1;
    uint32_t *out = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out) return -1;
    int y;
    for (y = 0; y < h; y++) {
        int srcy = topdown ? y : (h - 1 - y);
        const uint8_t *row = d + off + (size_t)srcy * (size_t)rowb;
        int x;
        for (x = 0; x < w; x++) {
            int b = row[x * (bpp / 8) + 0];
            int g = row[x * (bpp / 8) + 1];
            int r = row[x * (bpp / 8) + 2];
            out[(size_t)y * (size_t)w + (size_t)x] = pack_rgb(r, g, b);
        }
    }
    *px = out; *ow = w; *oh = h;
    return 0;
}

/* ---- GIF (first frame, no animation) ---- */

static int decode_gif(const uint8_t *d, size_t n, uint32_t **px, int *ow, int *oh) {
    if (n < 13) return -1;
    if (!(d[0] == 'G' && d[1] == 'I' && d[2] == 'F' &&
          d[3] == '8' && (d[4] == '7' || d[4] == '9') && d[5] == 'a'))
        return -1;
    int gw = rd16le(d + 6);
    int gh = rd16le(d + 8);
    unsigned packed = d[10];
    int gct = (packed & 0x80) ? 1 : 0;
    int gct_sz = gct ? (1 << ((packed & 7) + 1)) : 0;
    size_t off = 13;
    uint8_t gpal[256 * 3];
    if (gct) {
        if (off + (size_t)gct_sz * 3 > n) return -1;
        memcpy(gpal, d + off, (size_t)gct_sz * 3);
        off += (size_t)gct_sz * 3;
    }
    int w = 0, h = 0, left = 0, top = 0;
    uint8_t lpal[256 * 3];
    int lct = 0, lct_sz = 0;
    int interlace = 0;
    const uint8_t *pal = gpal;
    int pal_n = gct_sz;
    /* skip extensions until an image descriptor */
    for (;;) {
        if (off >= n) return -1;
        if (d[off] == 0x2C) {
            if (off + 10 > n) return -1;
            left = rd16le(d + off + 1);
            top = rd16le(d + off + 3);
            w = rd16le(d + off + 5);
            h = rd16le(d + off + 7);
            unsigned ip = d[off + 9];
            interlace = (ip & 0x40) ? 1 : 0;
            lct = (ip & 0x80) ? 1 : 0;
            lct_sz = lct ? (1 << ((ip & 7) + 1)) : 0;
            off += 10;
            if (lct) {
                if (off + (size_t)lct_sz * 3 > n) return -1;
                memcpy(lpal, d + off, (size_t)lct_sz * 3);
                off += (size_t)lct_sz * 3;
                pal = lpal; pal_n = lct_sz;
            }
            break;
        } else if (d[off] == 0x21) {
            if (off + 2 > n) return -1;
            off += 2;
            while (off < n && d[off] != 0) {
                unsigned blk = d[off];
                if (off + 1 + blk > n) return -1;
                off += 1 + blk;
            }
            if (off < n) off++;
        } else if (d[off] == 0x3B) {
            return -1;
        } else {
            return -1;
        }
    }
    if (!size_ok(w, h) || left < 0 || top < 0) return -1;
    (void)gw; (void)gh;
    if (off >= n) return -1;
    int min_cs = d[off++];
    if (min_cs < 2 || min_cs > 8) return -1;

    /* gather LZW bytes */
    uint8_t *lz = NULL;
    size_t lz_len = 0, lz_cap = 0;
    while (off < n && d[off] != 0) {
        unsigned blk = d[off++];
        if (off + blk > n) { free(lz); return -1; }
        if (lz_len + blk > lz_cap) {
            size_t nc = lz_cap ? lz_cap * 2 : 4096;
            while (nc < lz_len + blk) nc *= 2;
            uint8_t *p = (uint8_t *)realloc(lz, nc);
            if (!p) { free(lz); return -1; }
            lz = p; lz_cap = nc;
        }
        memcpy(lz + lz_len, d + off, blk);
        lz_len += blk;
        off += blk;
    }
    if (!lz) return -1;

    /* LZW */
    int clear = 1 << min_cs;
    int endc = clear + 1;
    int codesize = min_cs + 1;
    int nextc = endc + 1;
    uint16_t prefix[4096];
    uint8_t  suffix[4096];
    uint8_t  stack[4096];
    int i;
    for (i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }

    uint8_t *idx = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!idx) { free(lz); return -1; }
    size_t npix = 0, npix_need = (size_t)w * (size_t)h;
    unsigned bitbuf = 0;
    int bitcnt = 0;
    size_t lp = 0;
    int prev = -1;

    while (npix < npix_need) {
        while (bitcnt < codesize) {
            if (lp >= lz_len) goto gif_done;
            bitbuf |= (unsigned)lz[lp++] << bitcnt;
            bitcnt += 8;
        }
        int code = (int)(bitbuf & ((1u << codesize) - 1u));
        bitbuf >>= codesize;
        bitcnt -= codesize;
        if (code == clear) {
            codesize = min_cs + 1;
            nextc = endc + 1;
            prev = -1;
            continue;
        }
        if (code == endc) break;
        int c = code;
        int sl = 0;
        if (c == nextc && prev >= 0) {
            /* KwKwK */
            int t = prev;
            while (t >= clear && sl < 4095) {
                stack[sl++] = suffix[t];
                t = prefix[t];
            }
            if (t < 0 || t >= 4096) { free(lz); free(idx); return -1; }
            stack[sl++] = suffix[t];
            {
                uint8_t firstpix = stack[sl - 1];
                stack[sl++] = firstpix;
            }
            /* fix: first pixel of prev stream is suffix[root] */
        } else if (c > nextc) {
            free(lz); free(idx); return -1;
        } else {
            while (c >= clear && sl < 4095) {
                stack[sl++] = suffix[c];
                c = prefix[c];
            }
            if (c < 0 || c >= 4096) { free(lz); free(idx); return -1; }
            stack[sl++] = suffix[c];
        }
        int first = stack[sl - 1];
        while (sl > 0 && npix < npix_need)
            idx[npix++] = stack[--sl];
        if (prev >= 0 && nextc < 4096) {
            prefix[nextc] = (uint16_t)prev;
            suffix[nextc] = (uint8_t)first;
            nextc++;
            if (nextc == (1 << codesize) && codesize < 12) codesize++;
        }
        prev = code;
    }
gif_done:
    free(lz);
    uint32_t *out = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out) { free(idx); return -1; }
    /* deinterlace into dest; if not interlaced, 1:1 */
    static const int start[4] = { 0, 4, 2, 1 };
    static const int step[4] = { 8, 8, 4, 2 };
    if (!interlace) {
        size_t p;
        for (p = 0; p < npix_need; p++) {
            int ix = idx[p];
            if (ix >= pal_n) ix = 0;
            out[p] = pack_rgb(pal[ix * 3], pal[ix * 3 + 1], pal[ix * 3 + 2]);
        }
    } else {
        int pass, yy = 0;
        size_t p = 0;
        for (pass = 0; pass < 4; pass++) {
            for (yy = start[pass]; yy < h; yy += step[pass]) {
                int x;
                for (x = 0; x < w && p < npix_need; x++, p++) {
                    int ix = idx[p];
                    if (ix >= pal_n) ix = 0;
                    out[(size_t)yy * (size_t)w + (size_t)x] =
                        pack_rgb(pal[ix * 3], pal[ix * 3 + 1], pal[ix * 3 + 2]);
                }
            }
        }
    }
    free(idx);
    *px = out; *ow = w; *oh = h;
    return 0;
}

/* ---- baseline JPEG (SOF0) ---- */

typedef struct {
    uint8_t bits[17];
    uint8_t huffval[256];
    int maxcode[18];
    int mincode[18];
    int valptr[18];
} jhuff_t;

typedef struct {
    const uint8_t *p;
    size_t n, i;
    unsigned bits;
    int nbit;
    int err;
    int16_t qt[4][64];
    jhuff_t dc[4], ac[4];
    int w, h;
    int ncomp;
    int cid[3], hsf[3], vsf[3], qt_id[3], dc_id[3], ac_id[3];
    int mcu_w, mcu_h, mx, my;
} jpeg_t;

static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static int jpg_getb(jpeg_t *j) {
    if (j->i >= j->n) { j->err = 1; return 0; }
    return j->p[j->i++];
}

static int jpg_need(jpeg_t *j, int n) {
    while (j->nbit < n) {
        int b = jpg_getb(j);
        if (j->err) return 0;
        if (b == 0xFF) {
            int n0 = jpg_getb(j);
            if (n0 != 0) { j->err = 1; return 0; }
        }
        j->bits = (j->bits << 8) | (unsigned)b;
        j->nbit += 8;
    }
    return 1;
}

static int jpg_bits(jpeg_t *j, int n) {
    if (n == 0) return 0;
    if (!jpg_need(j, n)) return 0;
    int v = (int)((j->bits >> (j->nbit - n)) & ((1 << n) - 1));
    j->nbit -= n;
    return v;
}

static int jpg_huff_build(jhuff_t *h, const uint8_t *bits, const uint8_t *val, int nval) {
    memcpy(h->bits, bits, 17);
    if (nval > 256) return 0;
    memcpy(h->huffval, val, (size_t)nval);
    int p = 0, code = 0, i;
    for (i = 1; i <= 16; i++) {
        h->valptr[i] = p;
        h->mincode[i] = code;
        p += bits[i];
        code += bits[i];
        h->maxcode[i] = code - 1;
        code <<= 1;
        if (bits[i] && h->maxcode[i] < h->mincode[i] - 1) return 0;
    }
    h->maxcode[17] = 0x1FFFF;
    return 1;
}

static int jpg_huff_dec(jpeg_t *j, const jhuff_t *h) {
    int code = jpg_bits(j, 1);
    int l;
    for (l = 1; l <= 16; l++) {
        if (code <= h->maxcode[l]) {
            int idx = h->valptr[l] + (code - h->mincode[l]);
            if (idx < 0 || idx > 255) { j->err = 1; return 0; }
            return h->huffval[idx];
        }
        code = (code << 1) | jpg_bits(j, 1);
        if (j->err) return 0;
    }
    j->err = 1;
    return 0;
}

static int jpg_extend(int v, int t) {
    int vt = 1 << (t - 1);
    if (t > 0 && v < vt) v += (int)((~0u) << (unsigned)t) + 1;
    return v;
}

static void idct8(int *b) {
    /* Integer inverse DCT, LLM-ish, good enough for a hobby browser. */
    static const int c1 = 251, c2 = 237, c3 = 213, c4 = 181, c5 = 142, c6 = 98, c7 = 50;
    int y, x;
    int t[64];
    for (y = 0; y < 8; y++) {
        int *s = b + y * 8;
        int s0 = s[0], s1 = s[1], s2 = s[2], s3 = s[3];
        int s4 = s[4], s5 = s[5], s6 = s[6], s7 = s[7];
        int p0 = (s0 + s4) * c4;
        int p1 = (s0 - s4) * c4;
        int p2 = s2 * c2 + s6 * c6;
        int p3 = s2 * c6 - s6 * c2;
        int p4 = s1 * c1 + s7 * c7;
        int p5 = s1 * c7 - s7 * c1;
        int p6 = s5 * c5 + s3 * c3;
        int p7 = s5 * c3 - s3 * c5;
        int a0 = p0 + p2, a1 = p1 + p3, a2 = p1 - p3, a3 = p0 - p2;
        int a4 = p4 + p6, a5 = p5 + p7, a6 = p5 - p7, a7 = p4 - p6;
        t[y * 8 + 0] = (a0 + a4) >> 8;
        t[y * 8 + 7] = (a0 - a4) >> 8;
        t[y * 8 + 1] = (a1 + a5) >> 8;
        t[y * 8 + 6] = (a1 - a5) >> 8;
        t[y * 8 + 2] = (a2 + a6) >> 8;
        t[y * 8 + 5] = (a2 - a6) >> 8;
        t[y * 8 + 3] = (a3 + a7) >> 8;
        t[y * 8 + 4] = (a3 - a7) >> 8;
    }
    for (x = 0; x < 8; x++) {
        int s0 = t[x], s1 = t[8 + x], s2 = t[16 + x], s3 = t[24 + x];
        int s4 = t[32 + x], s5 = t[40 + x], s6 = t[48 + x], s7 = t[56 + x];
        int p0 = (s0 + s4) * c4;
        int p1 = (s0 - s4) * c4;
        int p2 = s2 * c2 + s6 * c6;
        int p3 = s2 * c6 - s6 * c2;
        int p4 = s1 * c1 + s7 * c7;
        int p5 = s1 * c7 - s7 * c1;
        int p6 = s5 * c5 + s3 * c3;
        int p7 = s5 * c3 - s3 * c5;
        int a0 = p0 + p2, a1 = p1 + p3, a2 = p1 - p3, a3 = p0 - p2;
        int a4 = p4 + p6, a5 = p5 + p7, a6 = p5 - p7, a7 = p4 - p6;
        b[x]      = (a0 + a4) >> 8;
        b[56 + x] = (a0 - a4) >> 8;
        b[8 + x]  = (a1 + a5) >> 8;
        b[48 + x] = (a1 - a5) >> 8;
        b[16 + x] = (a2 + a6) >> 8;
        b[40 + x] = (a2 - a6) >> 8;
        b[24 + x] = (a3 + a7) >> 8;
        b[32 + x] = (a3 - a7) >> 8;
    }
}

static int jpg_block(jpeg_t *j, int dc_id, int ac_id, int qid, int *pred, int *out) {
    int t = jpg_huff_dec(j, &j->dc[dc_id]);
    if (j->err) return 0;
    int diff = jpg_extend(jpg_bits(j, t), t);
    *pred += diff;
    int zz[64];
    memset(zz, 0, sizeof zz);
    zz[0] = *pred * j->qt[qid][0];
    int k = 1;
    while (k < 64) {
        int rs = jpg_huff_dec(j, &j->ac[ac_id]);
        if (j->err) return 0;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k >= 64) break;
        zz[k] = jpg_extend(jpg_bits(j, s), s) * j->qt[qid][k];
        k++;
    }
    int blk[64];
    for (k = 0; k < 64; k++) blk[ZZ[k]] = zz[k];
    idct8(blk);
    for (k = 0; k < 64; k++) {
        int v = (blk[k] >> 3) + 128;
        out[k] = clamp_i(v);
    }
    return 1;
}

static int decode_jpeg(const uint8_t *d, size_t n, uint32_t **px, int *ow, int *oh) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    jpeg_t j;
    memset(&j, 0, sizeof j);
    j.p = d; j.n = n; j.i = 2;
    int have_sof = 0, have_sos = 0;
    while (j.i + 4 <= n) {
        if (j.p[j.i] != 0xFF) return -1;
        while (j.i < n && j.p[j.i] == 0xFF) j.i++;
        if (j.i >= n) return -1;
        int m = j.p[j.i++];
        if (m == 0xD9) break;
        if (m == 0xD8) continue;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (j.i + 2 > n) return -1;
        int len = (j.p[j.i] << 8) | j.p[j.i + 1];
        if (len < 2 || j.i + len > n) return -1;
        const uint8_t *pl = j.p + j.i + 2;
        int plen = len - 2;
        j.i += len;
        if (m == 0xC0) { /* SOF0 */
            if (plen < 9 || pl[0] != 8) return -1;
            j.h = (pl[1] << 8) | pl[2];
            j.w = (pl[3] << 8) | pl[4];
            j.ncomp = pl[5];
            if (j.ncomp != 1 && j.ncomp != 3) return -1;
            if (!size_ok(j.w, j.h)) return -1;
            if (plen < 6 + 3 * j.ncomp) return -1;
            int c, maxh = 1, maxv = 1;
            for (c = 0; c < j.ncomp; c++) {
                j.cid[c] = pl[6 + 3 * c];
                j.hsf[c] = pl[7 + 3 * c] >> 4;
                j.vsf[c] = pl[7 + 3 * c] & 15;
                j.qt_id[c] = pl[8 + 3 * c];
                if (j.hsf[c] < 1 || j.vsf[c] < 1 || j.qt_id[c] > 3) return -1;
                if (j.hsf[c] > maxh) maxh = j.hsf[c];
                if (j.vsf[c] > maxv) maxv = j.vsf[c];
            }
            j.mcu_w = 8 * maxh;
            j.mcu_h = 8 * maxv;
            j.mx = (j.w + j.mcu_w - 1) / j.mcu_w;
            j.my = (j.h + j.mcu_h - 1) / j.mcu_h;
            have_sof = 1;
        } else if (m == 0xDB) { /* DQT */
            int p = 0;
            while (p < plen) {
                int pq = pl[p] >> 4, tq = pl[p] & 15;
                p++;
                if (tq > 3 || pq != 0) return -1;
                if (p + 64 > plen) return -1;
                int k;
                for (k = 0; k < 64; k++) j.qt[tq][k] = pl[p++];
            }
        } else if (m == 0xC4) { /* DHT */
            int p = 0;
            while (p < plen) {
                int tc = pl[p] >> 4, th = pl[p] & 15;
                p++;
                if (tc > 1 || th > 3 || p + 16 > plen) return -1;
                uint8_t bits[17];
                bits[0] = 0;
                int nv = 0, i;
                for (i = 1; i <= 16; i++) { bits[i] = pl[p++]; nv += bits[i]; }
                if (nv > 256 || p + nv > plen) return -1;
                if (tc == 0) {
                    if (!jpg_huff_build(&j.dc[th], bits, pl + p, nv)) return -1;
                } else {
                    if (!jpg_huff_build(&j.ac[th], bits, pl + p, nv)) return -1;
                }
                p += nv;
            }
        } else if (m == 0xDA) { /* SOS */
            if (!have_sof || plen < 6) return -1;
            int ns = pl[0];
            if (ns != j.ncomp) return -1;
            int c;
            for (c = 0; c < ns; c++) {
                int id = pl[1 + 2 * c];
                int sel = pl[2 + 2 * c];
                int k;
                for (k = 0; k < j.ncomp; k++) if (j.cid[k] == id) {
                    j.dc_id[k] = sel >> 4;
                    j.ac_id[k] = sel & 15;
                }
            }
            have_sos = 1;
            /* entropy data starts at current j.i */
            break;
        }
    }
    if (!have_sof || !have_sos) return -1;

    uint32_t *out = (uint32_t *)malloc((size_t)j.w * (size_t)j.h * 4);
    if (!out) return -1;
    memset(out, 0xFF, (size_t)j.w * (size_t)j.h * 4);
    int pred[3] = { 0, 0, 0 };
    int my, mx;
    int yblk[4][64], cbblk[64], crblk[64];
    for (my = 0; my < j.my; my++) {
        for (mx = 0; mx < j.mx; mx++) {
            int c;
            for (c = 0; c < j.ncomp; c++) {
                int by, bx;
                for (by = 0; by < j.vsf[c]; by++) {
                    for (bx = 0; bx < j.hsf[c]; bx++) {
                        int *dst;
                        if (c == 0) dst = yblk[by * j.hsf[c] + bx];
                        else if (c == 1) dst = cbblk;
                        else dst = crblk;
                        if (!jpg_block(&j, j.dc_id[c], j.ac_id[c], j.qt_id[c],
                                       &pred[c], dst)) {
                            free(out); return -1;
                        }
                    }
                }
            }
            int yy, xx;
            for (yy = 0; yy < j.mcu_h; yy++) {
                int py = my * j.mcu_h + yy;
                if (py >= j.h) break;
                for (xx = 0; xx < j.mcu_w; xx++) {
                    int px_ = mx * j.mcu_w + xx;
                    if (px_ >= j.w) break;
                    int yv, cb = 128, cr = 128;
                    int yb = (yy * j.vsf[0] / j.mcu_h * 8);
                    /* sample Y from the right 8x8 */
                    int ysi = (yy * j.vsf[0]) / j.mcu_h;
                    int xs = (xx * j.hsf[0]) / j.mcu_w;
                    int ybi = ysi;
                    if (ybi >= j.vsf[0]) ybi = j.vsf[0] - 1;
                    int xbi = xs;
                    if (xbi >= j.hsf[0]) xbi = j.hsf[0] - 1;
                    int ly = yy - ybi * 8;
                    int lx = xx - xbi * 8;
                    /* Better: pixel in MCU → which Y block */
                    int yblk_r = yy / (j.mcu_h / j.vsf[0]);
                    int yblk_c = xx / (j.mcu_w / j.hsf[0]);
                    if (yblk_r >= j.vsf[0]) yblk_r = j.vsf[0] - 1;
                    if (yblk_c >= j.hsf[0]) yblk_c = j.hsf[0] - 1;
                    int yrr = yy % (j.mcu_h / j.vsf[0]);
                    int ycc = xx % (j.mcu_w / j.hsf[0]);
                    /* scale into 8x8 */
                    int subh = j.mcu_h / j.vsf[0];
                    int subw = j.mcu_w / j.hsf[0];
                    yrr = (yy % subh) * 8 / subh;
                    ycc = (xx % subw) * 8 / subw;
                    yv = yblk[yblk_r * j.hsf[0] + yblk_c][yrr * 8 + ycc];
                    if (j.ncomp == 3) {
                        int cbr = yy * 8 / j.mcu_h;
                        int cbc = xx * 8 / j.mcu_w;
                        cb = cbblk[cbr * 8 + cbc];
                        cr = crblk[cbr * 8 + cbc];
                    }
                    int r = yv + ((91881 * (cr - 128)) >> 16);
                    int g = yv - ((22554 * (cb - 128) + 46802 * (cr - 128)) >> 16);
                    int b = yv + ((116130 * (cb - 128)) >> 16);
                    out[(size_t)py * (size_t)j.w + (size_t)px_] = pack_rgb(r, g, b);
                    (void)ly; (void)lx; (void)yb;
                }
            }
        }
    }
    *px = out; *ow = j.w; *oh = j.h;
    return 0;
}

/* ---- dispatch ---- */

int wv_image_decode(const uint8_t *data, size_t n,
                    uint32_t **px, int *w, int *h) {
    if (!data || !px || !w || !h || n < 4) return -1;
    *px = NULL; *w = 0; *h = 0;
    if (n >= 8 && data[0] == 137 && data[1] == 80) return decode_png(data, n, px, w, h);
    if (n >= 2 && data[0] == 0xFF && data[1] == 0xD8) return decode_jpeg(data, n, px, w, h);
    if (n >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return decode_gif(data, n, px, w, h);
    if (n >= 2 && data[0] == 'B' && data[1] == 'M') return decode_bmp(data, n, px, w, h);
    return -1;
}

int wv_image_decode_data_url(const char *url, uint32_t **px, int *w, int *h) {
    if (!url || strncmp(url, "data:", 5) != 0) return -1;
    const char *comma = strchr(url, ',');
    if (!comma) return -1;
    int is_b64 = 0;
    const char *p;
    for (p = url + 5; p < comma; p++) {
        if (p[0] == 'b' && strncmp(p, "base64", 6) == 0) { is_b64 = 1; break; }
    }
    const char *payload = comma + 1;
    size_t plen = strlen(payload);
    uint8_t *raw;
    size_t rlen = 0;
    if (is_b64) {
        raw = b64_decode(payload, plen, &rlen);
        if (!raw) return -1;
    } else {
        raw = (uint8_t *)malloc(plen + 1);
        if (!raw) return -1;
        memcpy(raw, payload, plen);
        rlen = plen;
    }
    int rc = wv_image_decode(raw, rlen, px, w, h);
    free(raw);
    return rc;
}
