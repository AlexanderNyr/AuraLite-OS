/* advapi32.c — W32APP_PLAN.md phase W32A-9: the registry and security engine.
 *
 * 37 REAL ledger symbols + 9 FAIL-CLEAN finals + RegFlushKey (the fsync
 * point), all documented in w32/include/w32/advapi32.h and docs/win32.md.
 *
 * THE HIVE (format W32HIVE1, ours — D5).  One file, whole-write, CRC'd:
 *
 *   offset  size  field
 *        0     8  magic "W32HIVE1"          (a torn write fails this first)
 *        8     4  flags (0)
 *       12     8  sequence number (u64le, bumps on every persisted write)
 *       20     4  payload_len (u32le)
 *       24     4  payload CRC32 (IEEE 802.3, u32le)
 *       28   ...  payload: the key tree
 *
 *   payload := u32 root_count, then root_count node records:
 *   node := u16 name_len, name_len UTF-16LE units, u64 mtime (FILETIME),
 *           u32 value_count,
 *           value_count x (u16 name_len, name, u32 type, u32 data_len, data),
 *           u32 sub_count, sub_count x node
 *
 * Writes never truncate first: the header's payload_len bounds the parse,
 * so a shorter tree over a longer old file leaves a stale tail the parser
 * ignores, and death mid-write leaves either a short file (magic/CRC/
 * length mismatch -> ERROR_FILE_CORRUPT on the next open, never a
 * half-read) or the previous intact contents.  An empty (0-byte) file
 * parses as fresh: the only way our writer produces one is a crash before
 * the first-ever write, where there is no old data to lose.
 *
 * Location: /disk/w32hive when /disk exists (diskfs, survives reboot),
 * else /tmp/w32hive (tmpfs, volatile) with the volatility logged at first
 * use.  Single writer session: the hive loads at the first registry call
 * in a process and writes through on every mutation; a later process
 * re-reads the file.
 */

#include "w32/advapi32.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/random.h>

#include "atls/atls.h"

/* ---- limits (documented in docs/win32.md) ---------------------------- */

#define REG_MAX_NAME        255u     /* key name, UTF-16 units            */
#define REG_MAX_VALUE_NAME  16383u   /* value name, UTF-16 units          */
#define REG_MAX_DATA        (1u << 20) /* 1 MiB of value data             */
#define REG_MAX_SUBKEYS     512
#define REG_MAX_VALUES      1024
#define REG_MAX_DEPTH       32
#define REG_MAX_PATH        1024     /* full path, UTF-16 units           */
#define REG_KEY_HANDLES     64
#define REG_CRYPTO_PROVS    16
#define REG_CRYPTO_HASHES   64
#define REG_HASH_MAX_DATA   (4u << 20) /* buffered hash input cap (4 MiB) */

#define HIVE_MAGIC          "W32HIVE1"
#define HIVE_HEADER_SIZE    28

/* ---- the once-note (the FAIL-CLEAN voice; see w32_stubs_gen.c) -------- */

static void note_once(int *flag, const char *what) {
    if (*flag) return;
    *flag = 1;
    printf("w32: %s\n", what);
}

/* ---- CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) ------------------- */

static uint32_t crc32_table[256];
static int crc32_ready;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    if (!crc32_ready) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- the key tree ------------------------------------------------------ */

typedef struct reg_value {
    uint16_t *name;            /* NUL-terminated, may be "" (the default) */
    size_t    name_units;      /* without the NUL                         */
    uint32_t  type;
    uint8_t  *data;
    uint32_t  len;
} reg_value_t;

typedef struct reg_key {
    uint16_t     *name;        /* NUL-terminated                          */
    size_t        name_units;
    uint64_t      mtime;       /* FILETIME of last write                  */
    reg_value_t  *values;
    size_t        nvalues;
    struct reg_key **subs;
    size_t        nsubs;
} reg_key_t;

static reg_key_t *hkcu_root;   /* the real HKEY_CURRENT_USER tree         */
static reg_key_t *hklm_root;   /* the real HKEY_LOCAL_MACHINE tree        */

static uint64_t hive_seq;
static int      hive_loaded;
static int      hive_corrupt;      /* latched: every Reg call refuses      */
static int      hive_missing_ok;   /* fresh-start paths already logged     */

/* Host-test seam: exact path override (see advapi32.h). */
const char *w32_advapi_hive_override;

static void reg_free_key(reg_key_t *k) {
    if (!k) return;
    for (size_t i = 0; i < k->nsubs; i++) reg_free_key(k->subs[i]);
    free(k->subs);
    for (size_t i = 0; i < k->nvalues; i++) {
        free(k->values[i].name);
        free(k->values[i].data);
    }
    free(k->values);
    free(k->name);
    free(k);
}

static uint16_t *w16_dup(const uint16_t *s, size_t units) {
    uint16_t *d = malloc((units + 1) * sizeof(uint16_t));
    if (!d) return NULL;
    memcpy(d, s, units * sizeof(uint16_t));
    d[units] = 0;
    return d;
}

/* ASCII-letter-only case folding (the documented folding scope). */
static uint16_t fold16(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c - 'A' + 'a');
    return c;
}

static int name_eq(const uint16_t *a, size_t an, const uint16_t *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (fold16(a[i]) != fold16(b[i])) return 0;
    return 1;
}

static uint64_t now_filetime(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)(ts.tv_nsec / 100)
         + 116444736000000000ull;
}

static reg_key_t *key_new(const uint16_t *name, size_t units) {
    reg_key_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->name = w16_dup(name, units);
    if (!k->name) { free(k); return NULL; }
    k->name_units = units;
    k->mtime = now_filetime();
    return k;
}

static reg_key_t *key_find_child(const reg_key_t *k, const uint16_t *name,
                                 size_t units) {
    for (size_t i = 0; i < k->nsubs; i++)
        if (name_eq(k->subs[i]->name, k->subs[i]->name_units, name, units))
            return k->subs[i];
    return NULL;
}

static reg_key_t *key_add_child(reg_key_t *k, const uint16_t *name, size_t units) {
    if (k->nsubs >= REG_MAX_SUBKEYS) return NULL;
    reg_key_t *c = key_new(name, units);
    if (!c) return NULL;
    reg_key_t **grown = realloc(k->subs, (k->nsubs + 1) * sizeof *grown);
    if (!grown) { reg_free_key(c); return NULL; }
    k->subs = grown;
    k->subs[k->nsubs++] = c;
    k->mtime = now_filetime();
    return c;
}

static void key_remove_child(reg_key_t *k, reg_key_t *child) {
    for (size_t i = 0; i < k->nsubs; i++) {
        if (k->subs[i] == child) {
            reg_free_key(child);
            memmove(&k->subs[i], &k->subs[i + 1],
                    (k->nsubs - i - 1) * sizeof *k->subs);
            k->nsubs--;
            k->mtime = now_filetime();
            return;
        }
    }
}

static reg_value_t *key_find_value(const reg_key_t *k, const uint16_t *name,
                                   size_t units) {
    for (size_t i = 0; i < k->nvalues; i++)
        if (name_eq(k->values[i].name, k->values[i].name_units, name, units))
            return &k->values[i];
    return NULL;
}

/* Replace or insert a value; takes ownership of nothing (copies). */
static int key_set_value(reg_key_t *k, const uint16_t *name, size_t units,
                         uint32_t type, const uint8_t *data, uint32_t len) {
    reg_value_t *v = key_find_value(k, name, units);
    if (!v) {
        if (k->nvalues >= REG_MAX_VALUES) return -1;
        reg_value_t *grown = realloc(k->values, (k->nvalues + 1) * sizeof *grown);
        if (!grown) return -1;
        k->values = grown;
        v = &k->values[k->nvalues];
        memset(v, 0, sizeof *v);
        v->name = w16_dup(name, units);
        if (!v->name) return -1;
        v->name_units = units;
        k->nvalues++;
    } else {
        free(v->data);
        v->data = NULL;
    }
    if (len) {
        v->data = malloc(len);
        if (!v->data) return -1;
        memcpy(v->data, data, len);
    } else {
        v->data = NULL;
    }
    v->type = type;
    v->len = len;
    k->mtime = now_filetime();
    return 0;
}

static void key_delete_value(reg_key_t *k, reg_value_t *v) {
    free(v->name);
    free(v->data);
    memmove(v, v + 1, (k->nvalues - (size_t)(v - k->values) - 1) * sizeof *v);
    k->nvalues--;
    k->mtime = now_filetime();
}

/* ---- serialize ---------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t   len, cap;
    int      bad;
} buf_t;

static void buf_put(buf_t *b, const void *src, size_t n) {
    if (b->bad) return;
    if (b->len + n > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 4096;
        while (want < b->len + n) want *= 2;
        uint8_t *grown = realloc(b->p, want);
        if (!grown) { b->bad = 1; return; }
        b->p = grown;
        b->cap = want;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void buf_u16(buf_t *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    buf_put(b, t, 2);
}
static void buf_u32(buf_t *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    buf_put(b, t, 4);
}
static void buf_u64(buf_t *b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    buf_put(b, t, 8);
}

static void serialize_key(buf_t *b, const reg_key_t *k) {
    buf_u16(b, (uint16_t)k->name_units);
    buf_put(b, k->name, k->name_units * sizeof(uint16_t));
    buf_u64(b, k->mtime);
    buf_u32(b, (uint32_t)k->nvalues);
    for (size_t i = 0; i < k->nvalues; i++) {
        const reg_value_t *v = &k->values[i];
        buf_u16(b, (uint16_t)v->name_units);
        buf_put(b, v->name, v->name_units * sizeof(uint16_t));
        buf_u32(b, v->type);
        buf_u32(b, v->len);
        if (v->len) buf_put(b, v->data, v->len);
    }
    buf_u32(b, (uint32_t)k->nsubs);
    for (size_t i = 0; i < k->nsubs; i++) serialize_key(b, k->subs[i]);
}

/* ---- parse --------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t         len, off;
    int            bad;
    int            depth;
    size_t         keys, values;
} rd_t;

static int rd_get(rd_t *r, void *dst, size_t n) {
    if (r->bad || r->off + n > r->len) { r->bad = 1; return -1; }
    memcpy(dst, r->p + r->off, n);
    r->off += n;
    return 0;
}
static uint16_t rd_u16(rd_t *r) {
    uint8_t t[2];
    if (rd_get(r, t, 2)) return 0;
    return (uint16_t)(t[0] | ((uint16_t)t[1] << 8));
}
static uint32_t rd_u32(rd_t *r) {
    uint8_t t[4];
    if (rd_get(r, t, 4)) return 0;
    return (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
           ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
}
static uint64_t rd_u64(rd_t *r) {
    uint64_t v = 0;
    uint8_t t[8];
    if (rd_get(r, t, 8)) return 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | t[i];
    return v;
}

static reg_key_t *parse_key(rd_t *r) {
    if (r->bad || r->depth >= REG_MAX_DEPTH) { r->bad = 1; return NULL; }
    r->depth++;
    uint16_t nl = rd_u16(r);
    if (r->bad || nl > REG_MAX_NAME) { r->bad = 1; r->depth--; return NULL; }
    uint16_t *nm = malloc(((size_t)nl + 1) * sizeof(uint16_t));
    if (!nm || rd_get(r, nm, (size_t)nl * 2)) { free(nm); r->bad = 1; r->depth--; return NULL; }
    nm[nl] = 0;
    reg_key_t *k = calloc(1, sizeof *k);
    if (!k) { free(nm); r->bad = 1; r->depth--; return NULL; }
    k->name = nm;
    k->name_units = nl;
    k->mtime = rd_u64(r);
    uint32_t nv = rd_u32(r);
    if (r->bad || nv > REG_MAX_VALUES || r->values + nv > REG_MAX_VALUES * 64) {
        r->bad = 1; r->depth--; reg_free_key(k); return NULL;
    }
    for (uint32_t i = 0; i < nv && !r->bad; i++) {
        uint16_t vnl = rd_u16(r);
        if (vnl > REG_MAX_VALUE_NAME) { r->bad = 1; break; }
        uint16_t *vn = malloc(((size_t)vnl + 1) * sizeof(uint16_t));
        if (!vn || rd_get(r, vn, (size_t)vnl * 2)) { free(vn); r->bad = 1; break; }
        vn[vnl] = 0;
        uint32_t type = rd_u32(r);
        uint32_t dlen = rd_u32(r);
        if (dlen > REG_MAX_DATA) { free(vn); r->bad = 1; break; }
        uint8_t *dd = NULL;
        if (dlen) {
            dd = malloc(dlen);
            if (!dd || rd_get(r, dd, dlen)) { free(vn); free(dd); r->bad = 1; break; }
        }
        reg_value_t *grown = realloc(k->values, (k->nvalues + 1) * sizeof *grown);
        if (!grown) { free(vn); free(dd); r->bad = 1; break; }
        k->values = grown;
        k->values[k->nvalues].name = vn;
        k->values[k->nvalues].name_units = vnl;
        k->values[k->nvalues].type = type;
        k->values[k->nvalues].data = dd;
        k->values[k->nvalues].len = dlen;
        k->nvalues++;
        r->values++;
    }
    if (!r->bad) {
        uint32_t ns = rd_u32(r);
        if (ns > REG_MAX_SUBKEYS) r->bad = 1;
        for (uint32_t i = 0; i < ns && !r->bad; i++) {
            reg_key_t *c = parse_key(r);
            if (!c) { r->bad = 1; break; }
            reg_key_t **grown = realloc(k->subs, (k->nsubs + 1) * sizeof *grown);
            if (!grown) { reg_free_key(c); r->bad = 1; break; }
            k->subs = grown;
            k->subs[k->nsubs++] = c;
            r->keys++;
        }
    }
    r->depth--;
    if (r->bad) { reg_free_key(k); return NULL; }
    return k;
}

/* ---- hive location + load + save ---------------------------------------- */

static const char *hive_path(void) {
    if (w32_advapi_hive_override) return w32_advapi_hive_override;
    struct stat st;
    if (stat("/disk", &st) == 0) return "/disk/w32hive";
    static int noted;
    if (!noted) {
        noted = 1;
        printf("w32: [advapi32] registry hive: /disk absent -- using "
               "/tmp/w32hive (volatile: settings do not survive a reboot)\n");
    }
    return "/tmp/w32hive";
}

static void seed_fresh_hive(void) {
    /* The documented read-mostly HKLM seed.  Nothing under HKCU: an
     * application's first write creates its own tree. */
    static const uint16_t sw[]   = { 'S','o','f','t','w','a','r','e' };
    static const uint16_t al[]   = { 'A','u','r','a','L','i','t','e' };
    static const uint16_t cv[]   = { 'C','u','r','r','e','n','t','V','e','r','s','i','o','n' };
    static const uint16_t v1[]   = { 'P','r','o','d','u','c','t','N','a','m','e' };
    static const uint16_t v2[]   = { 'H','i','v','e','F','o','r','m','a','t' };
    static const uint16_t v3[]   = { 'C','u','r','r','e','n','t','V','e','r','s','i','o','n' };
    static const char *s1 = "AuraLite OS (w32 personality)";
    static const char *s2 = "W32HIVE1";
    static const char *s3 = "0.0.1";
    reg_key_t *a = key_add_child(hklm_root, sw, 8);
    if (!a) return;
    reg_key_t *b = key_add_child(a, al, 8);
    if (!b) return;
    reg_key_t *c = key_add_child(b, cv, 14);
    if (!c) return;
    uint16_t w[64];
    size_t n = strlen(s1); for (size_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)s1[i];
    key_set_value(c, v1, 11, W32_REG_SZ, (const uint8_t *)w, (uint32_t)(n * 2 + 2));
    n = strlen(s2); for (size_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)s2[i];
    key_set_value(c, v2, 10, W32_REG_SZ, (const uint8_t *)w, (uint32_t)(n * 2 + 2));
    n = strlen(s3); for (size_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)s3[i];
    key_set_value(c, v3, 14, W32_REG_SZ, (const uint8_t *)w, (uint32_t)(n * 2 + 2));
}

static void hive_load(void) {
    hive_loaded = 1;
    hkcu_root = key_new((const uint16_t[]){ 'H','K','C','U' }, 4);
    hklm_root = key_new((const uint16_t[]){ 'H','K','L','M' }, 4);
    if (!hkcu_root || !hklm_root) { hive_corrupt = 1; return; }

    const char *path = hive_path();
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        seed_fresh_hive();          /* missing: fresh, silent (normal) */
        return;
    }
    uint8_t hdr[HIVE_HEADER_SIZE];
    ssize_t got = read(fd, hdr, sizeof hdr);
    if (got == 0) {                 /* empty file: fresh (see header comment) */
        close(fd);
        seed_fresh_hive();
        return;
    }
    if (got != (ssize_t)sizeof hdr) {
        close(fd);
        printf("w32: [advapi32] hive %s: truncated header (torn write) -- "
               "refusing to open\n", path);
        hive_corrupt = 1;
        return;
    }
    if (memcmp(hdr, HIVE_MAGIC, 8) != 0) {
        close(fd);
        printf("w32: [advapi32] hive %s: bad magic (torn write) -- "
               "refusing to open\n", path);
        hive_corrupt = 1;
        return;
    }
    uint64_t seq = 0;
    for (int i = 7; i >= 0; i--) seq = (seq << 8) | hdr[12 + i];
    uint32_t plen = 0, pcrc = 0;
    for (int i = 3; i >= 0; i--) plen = (plen << 8) | hdr[20 + i];
    for (int i = 3; i >= 0; i--) pcrc = (pcrc << 8) | hdr[24 + i];
    if (plen == 0 || plen > (32u << 20)) {
        close(fd);
        printf("w32: [advapi32] hive %s: implausible payload length %u -- "
               "refusing to open\n", path, plen);
        hive_corrupt = 1;
        return;
    }
    uint8_t *payload = malloc(plen);
    if (!payload) { close(fd); hive_corrupt = 1; return; }
    ssize_t rdn = read(fd, payload, plen);
    close(fd);
    if (rdn != (ssize_t)plen) {
        free(payload);
        printf("w32: [advapi32] hive %s: short payload (torn write) -- "
               "refusing to open\n", path);
        hive_corrupt = 1;
        return;
    }
    if (crc32_buf(payload, plen) != pcrc) {
        free(payload);
        printf("w32: [advapi32] hive %s: payload CRC mismatch (torn write) -- "
               "refusing to open\n", path);
        hive_corrupt = 1;
        return;
    }

    rd_t r = { payload, plen, 0, 0, 0, 0, 0 };
    uint32_t roots = rd_u32(&r);
    if (roots != 2) r.bad = 1;
    reg_key_t *cu = roots == 2 ? parse_key(&r) : NULL;
    reg_key_t *lm = !r.bad ? parse_key(&r) : NULL;
    free(payload);
    if (!r.bad && cu && lm) {
        reg_free_key(hkcu_root);
        reg_free_key(hklm_root);
        hkcu_root = cu;
        hklm_root = lm;
        hive_seq = seq;
        return;                     /* the good path */
    }
    if (cu) reg_free_key(cu);
    if (lm) reg_free_key(lm);
    printf("w32: [advapi32] hive %s: unparseable tree (torn write) -- "
           "refusing to open\n", path);
    hive_corrupt = 1;
}

static int fsync_noted;
static int hive_save(void) {
    buf_t b = {0};
    buf_u32(&b, 2);
    serialize_key(&b, hkcu_root);
    serialize_key(&b, hklm_root);
    if (b.bad) { free(b.p); return -1; }

    size_t total = HIVE_HEADER_SIZE + b.len;
    uint8_t *file = malloc(total);
    if (!file) { free(b.p); return -1; }
    memcpy(file, HIVE_MAGIC, 8);
    uint32_t flags = 0;
    memcpy(file + 8, &flags, 4);
    uint64_t seq = ++hive_seq;
    for (int i = 0; i < 8; i++) file[12 + i] = (uint8_t)(seq >> (8 * i));
    uint32_t plen = (uint32_t)b.len;
    memcpy(file + 20, &plen, 4);
    uint32_t pcrc = crc32_buf(b.p, b.len);
    memcpy(file + 24, &pcrc, 4);
    memcpy(file + HIVE_HEADER_SIZE, b.p, b.len);
    free(b.p);

    /* No O_TRUNC and no ftruncate: the header's payload_len bounds the
     * parse, so a stale tail is ignored and a torn write can never
     * present itself as valid data (the CRC is over exactly plen). */
    const char *path = hive_path();
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        printf("w32: [advapi32] hive %s: cannot open for write (errno %d)\n",
               path, errno);
        free(file);
        return -1;
    }
    size_t off = 0;
    while (off < total) {
        ssize_t w = write(fd, file + off, total - off);
        if (w <= 0) {
            printf("w32: [advapi32] hive %s: write failed (errno %d)\n",
                   path, errno);
            close(fd);
            free(file);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 && !fsync_noted) {
        fsync_noted = 1;
        printf("w32: [advapi32] hive fsync failed (errno %d); relying on "
               "the kernel's writeback\n", errno);
    }
    close(fd);
    free(file);
    return 0;
}

static void hive_ensure_loaded(void) {
    if (!hive_loaded) hive_load();
}


/* ---- open-key handles -----------------------------------------------------
 * A handle is (root id, subpath), not a tree pointer: deletion can free a
 * node under a live handle, and re-walking a short path on every call makes
 * that a non-event instead of a use-after-free. */

enum { ROOT_HKCU = 1, ROOT_HKLM = 2, ROOT_HKCR = 3 };

typedef struct {
    int      used;
    int      root;
    uint16_t path[REG_MAX_PATH + 1];
    size_t   units;               /* subpath under the root, no NUL        */
} reg_handle_t;

static reg_handle_t reg_handles[REG_KEY_HANDLES];

static int root_of(W32_HKEY key, int *root, const uint16_t **path, size_t *units) {
    uintptr_t v = (uintptr_t)key;
    if (v >= 0x80000000u && v <= 0x80000002u) {
        /* predefined order: HKCR=0x80000000, HKCU=0x...01, HKLM=0x...02 */
        *root = (v == 0x80000000u) ? ROOT_HKCR
              : (v == 0x80000001u) ? ROOT_HKCU : ROOT_HKLM;
        *path = NULL; *units = 0;
        return 0;
    }
    for (int i = 0; i < REG_KEY_HANDLES; i++) {
        if (reg_handles[i].used && (W32_HKEY)(uintptr_t)&reg_handles[i] == key) {
            *root = reg_handles[i].root;
            *path = reg_handles[i].path;
            *units = reg_handles[i].units;
            return 0;
        }
    }
    return -1;
}

/* Split "a\b\c" into components; rejects empty components. */
typedef struct {
    const uint16_t *p;
    size_t n;
} comp_t;

static int split_path(const uint16_t *path, size_t units, comp_t *out, int max) {
    if (units > REG_MAX_PATH) return -1;
    int n = 0;
    size_t i = 0;
    while (i < units) {
        size_t start = i;
        while (i < units && path[i] != '\\') i++;
        if (i == start) return -1;             /* "" or "\\" component */
        if (n == max) return -1;
        out[n].p = path + start;
        out[n].n = i - start;
        n++;
        if (i < units) i++;                    /* skip the separator */
    }
    return n;
}

/* Walk a component list from a real root.  create=1 builds missing keys. */
static reg_key_t *walk_from(reg_key_t *root, const comp_t *c, int n, int create) {
    reg_key_t *k = root;
    for (int i = 0; i < n; i++) {
        reg_key_t *next = key_find_child(k, c[i].p, c[i].n);
        if (!next) {
            if (!create) return NULL;
            next = key_add_child(k, c[i].p, c[i].n);
            if (!next) return NULL;
        }
        k = next;
    }
    return k;
}

/* HKCR: the two backing paths under the real trees. */
static reg_key_t *classes_side(reg_key_t *root, const comp_t *c, int n, int create) {
    /* path = Software\Classes\<components> */
    comp_t full[REG_MAX_DEPTH + 4];
    static const uint16_t sw[] = { 'S','o','f','t','w','a','r','e' };
    static const uint16_t cl[] = { 'C','l','a','s','s','e','s' };
    if (n + 2 > (int)(sizeof full / sizeof full[0])) return NULL;
    full[0].p = sw; full[0].n = 8;
    full[1].p = cl; full[1].n = 7;
    for (int i = 0; i < n; i++) full[2 + i] = c[i];
    return walk_from(root, full, n + 2, create);
}

static reg_key_t *resolve_for_read(int root, const comp_t *c, int n) {
    if (root == ROOT_HKCU) return walk_from(hkcu_root, c, n, 0);
    if (root == ROOT_HKLM) return walk_from(hklm_root, c, n, 0);
    /* HKCR merge view: HKCU wins, HKLM answers when HKCU is silent. */
    reg_key_t *k = classes_side(hkcu_root, c, n, 0);
    if (k) return k;
    return classes_side(hklm_root, c, n, 0);
}

/* Deep-merged snapshot of an HKCR path (for enumeration and info). */
static reg_key_t *hkcr_merge(const comp_t *c, int n) {
    reg_key_t *cu = classes_side(hkcu_root, c, n, 0);
    reg_key_t *lm = classes_side(hklm_root, c, n, 0);
    if (!cu) {
        if (!lm) return NULL;
        /* clone the HKLM side (values + subs, shallow enough: merged enum
         * only reads one level deep, but be complete anyway) */
        reg_key_t *snap = key_new(lm->name, lm->name_units);
        if (!snap) return NULL;
        for (size_t i = 0; i < lm->nvalues; i++)
            key_set_value(snap, lm->values[i].name, lm->values[i].name_units,
                          lm->values[i].type, lm->values[i].data, lm->values[i].len);
        for (size_t i = 0; i < lm->nsubs; i++) {
            comp_t one = { lm->subs[i]->name, lm->subs[i]->name_units };
            comp_t rest[REG_MAX_DEPTH];
            for (int j = 0; j < n; j++) rest[j] = c[j];
            rest[n] = one;
            reg_key_t *sub = hkcr_merge(rest, n + 1);
            if (sub) {
                reg_key_t **g = realloc(snap->subs, (snap->nsubs + 1) * sizeof *g);
                if (g) { snap->subs = g; snap->subs[snap->nsubs++] = sub; }
                else reg_free_key(sub);
            }
        }
        return snap;
    }
    /* HKCU side exists: use it as the base, overlay nothing (HKCU wins);
     * add HKLM-only subkeys for the merged enumeration. */
    reg_key_t *snap = key_new(cu->name, cu->name_units);
    if (!snap) return NULL;
    snap->mtime = cu->mtime;
    for (size_t i = 0; i < cu->nvalues; i++)
        key_set_value(snap, cu->values[i].name, cu->values[i].name_units,
                      cu->values[i].type, cu->values[i].data, cu->values[i].len);
    for (size_t i = 0; i < cu->nsubs; i++) {
        comp_t rest[REG_MAX_DEPTH];
        for (int j = 0; j < n; j++) rest[j] = c[j];
        rest[n].p = cu->subs[i]->name; rest[n].n = cu->subs[i]->name_units;
        reg_key_t *sub = hkcr_merge(rest, n + 1);
        if (sub) {
            reg_key_t **g = realloc(snap->subs, (snap->nsubs + 1) * sizeof *g);
            if (g) { snap->subs = g; snap->subs[snap->nsubs++] = sub; }
            else reg_free_key(sub);
        }
    }
    if (lm) {
        for (size_t i = 0; i < lm->nsubs; i++) {
            int dup = 0;
            for (size_t j = 0; j < snap->nsubs; j++)
                if (name_eq(snap->subs[j]->name, snap->subs[j]->name_units,
                            lm->subs[i]->name, lm->subs[i]->name_units)) { dup = 1; break; }
            if (dup) continue;
            comp_t rest[REG_MAX_DEPTH];
            for (int j = 0; j < n; j++) rest[j] = c[j];
            rest[n].p = lm->subs[i]->name; rest[n].n = lm->subs[i]->name_units;
            reg_key_t *sub = hkcr_merge(rest, n + 1);
            if (sub) {
                reg_key_t **g = realloc(snap->subs, (snap->nsubs + 1) * sizeof *g);
                if (g) { snap->subs = g; snap->subs[snap->nsubs++] = sub; }
                else reg_free_key(sub);
            }
        }
    }
    return snap;
}

/* The write target for a handle: HKCU and writable-HKLM walk their own
 * tree; HKCR writes land in HKCU\Software\Classes (the documented
 * behaviour).  Returns NULL + *err when policy refuses. */
static reg_key_t *resolve_for_write(int root, const comp_t *c, int n, int create,
                                    int *err) {
    *err = 0;
    if (root == ROOT_HKCU) return walk_from(hkcu_root, c, n, create);
    if (root == ROOT_HKLM) {
        /* read-mostly: only the \Software subtree is writable. */
        static const uint16_t sw[] = { 's','o','f','t','w','a','r','e' };
        if (n == 0 || c[0].n != 8 || !name_eq(c[0].p, 8, sw, 8)) {
            *err = W32_ERROR_ACCESS_DENIED;
            return NULL;
        }
        return walk_from(hklm_root, c, n, create);
    }
    /* ROOT_HKCR: write into the HKCU half. */
    return classes_side(hkcu_root, c, n, create);
}

static W32_HKEY handle_alloc(int root, const uint16_t *path, size_t units) {
    for (int i = 0; i < REG_KEY_HANDLES; i++) {
        if (!reg_handles[i].used) {
            reg_handles[i].used = 1;
            reg_handles[i].root = root;
            if (units) memcpy(reg_handles[i].path, path, units * sizeof(uint16_t));
            reg_handles[i].path[units] = 0;
            reg_handles[i].units = units;
            return (W32_HKEY)(uintptr_t)&reg_handles[i];
        }
    }
    return NULL;
}

/* Append subkey to a base path into out; returns units or -1. */
static int path_join(const uint16_t *base, size_t base_n,
                     const uint16_t *sub, size_t sub_n,
                     uint16_t *out, size_t out_cap) {
    size_t need = base_n + (base_n ? 1 : 0) + sub_n;
    if (need > out_cap) return -1;
    size_t o = 0;
    if (base_n) { memcpy(out, base, base_n * 2); o = base_n; out[o++] = '\\'; }
    if (sub_n) { memcpy(out + o, sub, sub_n * 2); o += sub_n; }
    return (int)need;
}

/* ---- the registry API ------------------------------------------------------ */

static W32_LONG reg_open_common(W32_HKEY key, const uint16_t *subkey, W32_HKEY *out) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (!out) return W32_ERROR_INVALID_PARAMETER;
    *out = NULL;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    size_t sub_n = subkey ? w32_utf16_len(subkey, REG_MAX_PATH + 1) : 0;
    if (subkey && sub_n > REG_MAX_PATH) return W32_ERROR_INVALID_PARAMETER;
    uint16_t joined[REG_MAX_PATH + 1];
    int jn = path_join(base, base_n, subkey, sub_n, joined, REG_MAX_PATH + 1);
    if (jn < 0) return W32_ERROR_INVALID_PARAMETER;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(joined, (size_t)jn, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_PARAMETER;

    if (!resolve_for_read(root, c, n)) return W32_ERROR_FILE_NOT_FOUND;
    W32_HKEY h = handle_alloc(root, joined, (size_t)jn);
    if (!h) return W32_ERROR_NOT_ENOUGH_MEMORY;
    *out = h;
    return W32_ERROR_SUCCESS;
}

W32_LONG W32ABI RegOpenKeyExW(W32_HKEY key, const uint16_t *subkey,
                              W32_ULONG reserved, W32_ULONG sam, W32_HKEY *out) {
    (void)reserved; (void)sam;         /* single-user: no access model */
    return reg_open_common(key, subkey, out);
}

W32_LONG W32ABI RegOpenKeyExA(W32_HKEY key, const char *subkey,
                              W32_ULONG reserved, W32_ULONG sam, W32_HKEY *out) {
    (void)reserved; (void)sam;
    if (subkey && strlen(subkey) > REG_MAX_PATH)
        return W32_ERROR_INVALID_PARAMETER;
    uint16_t w[REG_MAX_PATH + 1];
    if (subkey) w32_utf8z_to_utf16(subkey, w, REG_MAX_PATH + 1);
    return reg_open_common(key, subkey ? w : NULL, out);
}

static W32_LONG reg_create_common(W32_HKEY key, const uint16_t *subkey,
                                  W32_HKEY *out, W32_DWORD *disposition) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (!out || !subkey) return W32_ERROR_INVALID_PARAMETER;
    *out = NULL;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    size_t sub_n = w32_utf16_len(subkey, REG_MAX_PATH + 1);
    if (sub_n == 0 || sub_n > REG_MAX_PATH) return W32_ERROR_INVALID_PARAMETER;
    uint16_t joined[REG_MAX_PATH + 1];
    int jn = path_join(base, base_n, subkey, sub_n, joined, REG_MAX_PATH + 1);
    if (jn < 0) return W32_ERROR_INVALID_PARAMETER;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(joined, (size_t)jn, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_PARAMETER;

    int existed = resolve_for_read(root, c, n) != NULL;
    int err = 0;
    reg_key_t *k = resolve_for_write(root, c, n, 1, &err);
    if (!k) return err ? (W32_LONG)err : (W32_LONG)W32_ERROR_NOT_ENOUGH_MEMORY;
    if (hive_save() != 0) return W32_ERROR_REGISTRY_IO_FAILED;

    W32_HKEY h = handle_alloc(root, joined, (size_t)jn);
    if (!h) return W32_ERROR_NOT_ENOUGH_MEMORY;
    *out = h;
    if (disposition)
        *disposition = existed ? W32_REG_OPENED_EXISTING_KEY
                               : W32_REG_CREATED_NEW_KEY;
    return W32_ERROR_SUCCESS;
}

W32_LONG W32ABI RegCreateKeyExW(W32_HKEY key, const uint16_t *subkey,
                                W32_ULONG reserved, const uint16_t *cls,
                                W32_DWORD options, W32_ULONG sam,
                                void *security, W32_HKEY *out,
                                W32_DWORD *disposition) {
    (void)reserved; (void)cls; (void)options; (void)sam; (void)security;
    return reg_create_common(key, subkey, out, disposition);
}

W32_LONG W32ABI RegCreateKeyExA(W32_HKEY key, const char *subkey,
                                W32_ULONG reserved, const uint16_t *cls,
                                W32_DWORD options, W32_ULONG sam,
                                void *security, W32_HKEY *out,
                                W32_DWORD *disposition) {
    (void)reserved; (void)cls; (void)options; (void)sam; (void)security;
    if (!subkey || strlen(subkey) > REG_MAX_PATH)
        return W32_ERROR_INVALID_PARAMETER;
    uint16_t w[REG_MAX_PATH + 1];
    w32_utf8z_to_utf16(subkey, w, REG_MAX_PATH + 1);
    return reg_create_common(key, w, out, disposition);
}

W32_LONG W32ABI RegCloseKey(W32_HKEY key) {
    for (int i = 0; i < REG_KEY_HANDLES; i++) {
        if (reg_handles[i].used && (W32_HKEY)(uintptr_t)&reg_handles[i] == key) {
            reg_handles[i].used = 0;
            return W32_ERROR_SUCCESS;
        }
    }
    /* Predefined keys close successfully (the documented no-op). */
    uintptr_t v = (uintptr_t)key;
    if (v >= 0x80000000u && v <= 0x80000002u) return W32_ERROR_SUCCESS;
    return W32_ERROR_INVALID_HANDLE;
}

/* ---- values: query / set / delete ------------------------------------------ */

static W32_LONG reg_query_common(W32_HKEY key, const uint16_t *name,
                                 W32_DWORD *type, uint8_t *data, W32_DWORD *len) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(base, base_n, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_HANDLE;   /* stale/dangling path */

    reg_key_t *k = resolve_for_read(root, c, n);
    if (!k) return W32_ERROR_FILE_NOT_FOUND;

    const uint16_t empty = 0;
    const uint16_t *vn = name ? name : &empty;
    size_t vnl = name ? w32_utf16_len(name, REG_MAX_VALUE_NAME + 1) : 0;
    if (name && vnl > REG_MAX_VALUE_NAME) return W32_ERROR_INVALID_PARAMETER;
    reg_value_t *v = key_find_value(k, vn, vnl);
    if (!v) return W32_ERROR_FILE_NOT_FOUND;

    if (type) *type = v->type;
    if (!len) return W32_ERROR_SUCCESS;
    if (!data) { *len = v->len; return W32_ERROR_SUCCESS; }
    if (*len < v->len) { *len = v->len; return W32_ERROR_MORE_DATA; }
    if (v->len) memcpy(data, v->data, v->len);
    *len = v->len;
    return W32_ERROR_SUCCESS;
}

W32_LONG W32ABI RegQueryValueExW(W32_HKEY key, const uint16_t *name,
                                 W32_ULONG *reserved, W32_DWORD *type,
                                 uint8_t *data, W32_DWORD *len) {
    (void)reserved;
    return reg_query_common(key, name, type, data, len);
}

W32_LONG W32ABI RegQueryValueExA(W32_HKEY key, const char *name,
                                 W32_ULONG *reserved, W32_DWORD *type,
                                 uint8_t *data, W32_DWORD *len) {
    (void)reserved;
    if (name && strlen(name) > REG_MAX_VALUE_NAME)
        return W32_ERROR_INVALID_PARAMETER;
    uint16_t w[REG_MAX_VALUE_NAME + 1];
    const uint16_t *pw = NULL;
    if (name) {
        w32_utf8z_to_utf16(name, w, REG_MAX_VALUE_NAME + 1);
        pw = w;
    }
    return reg_query_common(key, pw, type, data, len);
}

static W32_LONG reg_set_common(W32_HKEY key, const uint16_t *name,
                               W32_DWORD type, const uint8_t *data,
                               W32_DWORD len) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (len > REG_MAX_DATA) return W32_ERROR_NOT_ENOUGH_MEMORY;
    if (!data && len) return W32_ERROR_INVALID_PARAMETER;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(base, base_n, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_HANDLE;

    int err = 0;
    reg_key_t *k = resolve_for_write(root, c, n, 1, &err);
    if (!k) return err ? (W32_LONG)err : (W32_LONG)W32_ERROR_NOT_ENOUGH_MEMORY;

    const uint16_t empty = 0;
    const uint16_t *vn = name ? name : &empty;
    size_t vnl = name ? w32_utf16_len(name, REG_MAX_VALUE_NAME + 1) : 0;
    if (name && vnl > REG_MAX_VALUE_NAME) return W32_ERROR_INVALID_PARAMETER;

    if (key_set_value(k, vn, vnl, type, data, len) != 0)
        return W32_ERROR_NOT_ENOUGH_MEMORY;
    if (hive_save() != 0) return W32_ERROR_REGISTRY_IO_FAILED;
    return W32_ERROR_SUCCESS;
}

W32_LONG W32ABI RegSetValueExW(W32_HKEY key, const uint16_t *name,
                               W32_ULONG reserved, W32_DWORD type,
                               const uint8_t *data, W32_DWORD len) {
    (void)reserved;
    return reg_set_common(key, name, type, data, len);
}

W32_LONG W32ABI RegSetValueExA(W32_HKEY key, const char *name,
                               W32_ULONG reserved, W32_DWORD type,
                               const uint8_t *data, W32_DWORD len) {
    (void)reserved;
    if (name && strlen(name) > REG_MAX_VALUE_NAME)
        return W32_ERROR_INVALID_PARAMETER;
    uint16_t w[REG_MAX_VALUE_NAME + 1];
    const uint16_t *pw = NULL;
    if (name) {
        w32_utf8z_to_utf16(name, w, REG_MAX_VALUE_NAME + 1);
        pw = w;
    }
    return reg_set_common(key, pw, type, data, len);
}

W32_LONG W32ABI RegDeleteValueW(W32_HKEY key, const uint16_t *name) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (!name) return W32_ERROR_INVALID_PARAMETER;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(base, base_n, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_HANDLE;

    int err = 0;
    reg_key_t *k = resolve_for_write(root, c, n, 0, &err);
    if (!k) return err ? (W32_LONG)err : (W32_LONG)W32_ERROR_FILE_NOT_FOUND;

    size_t vnl = w32_utf16_len(name, REG_MAX_VALUE_NAME + 1);
    if (vnl > REG_MAX_VALUE_NAME) return W32_ERROR_INVALID_PARAMETER;
    reg_value_t *v = key_find_value(k, name, vnl);
    if (!v) return W32_ERROR_FILE_NOT_FOUND;
    key_delete_value(k, v);
    if (hive_save() != 0) return W32_ERROR_REGISTRY_IO_FAILED;
    return W32_ERROR_SUCCESS;
}

/* ---- keys: delete ------------------------------------------------------------ */

static W32_LONG reg_delete_common(W32_HKEY key, const uint16_t *subkey,
                                  int refuse_nonempty) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (!subkey) return W32_ERROR_INVALID_PARAMETER;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    size_t sub_n = w32_utf16_len(subkey, REG_MAX_PATH + 1);
    if (sub_n == 0 || sub_n > REG_MAX_PATH) return W32_ERROR_INVALID_PARAMETER;
    uint16_t joined[REG_MAX_PATH + 1];
    int jn = path_join(base, base_n, subkey, sub_n, joined, REG_MAX_PATH + 1);
    if (jn < 0) return W32_ERROR_INVALID_PARAMETER;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(joined, (size_t)jn, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_PARAMETER;

    /* The PARENT must be walkable with write intent (policy check). */
    int err = 0;
    reg_key_t *parent;
    if (root == ROOT_HKCR) {
        parent = classes_side(hkcu_root, c, n - 1, 0);
        if (!parent) return W32_ERROR_FILE_NOT_FOUND;
    } else {
        parent = resolve_for_write(root, c, n - 1, 0, &err);
        if (!parent) return err ? (W32_LONG)err : (W32_LONG)W32_ERROR_FILE_NOT_FOUND;
    }
    reg_key_t *child = key_find_child(parent, c[n - 1].p, c[n - 1].n);
    if (!child) return W32_ERROR_FILE_NOT_FOUND;
    if (refuse_nonempty && child->nsubs) return W32_ERROR_ACCESS_DENIED;
    key_remove_child(parent, child);
    if (hive_save() != 0) return W32_ERROR_REGISTRY_IO_FAILED;
    return W32_ERROR_SUCCESS;
}

/* The documented contract (winreg.h): "The subkey to be deleted must not
 * have subkeys" -- recursive delete is RegDeleteTree's job, not this one.
 * A key with subkeys is refused with ERROR_ACCESS_DENIED. */
W32_LONG W32ABI RegDeleteKeyW(W32_HKEY key, const uint16_t *subkey) {
    return reg_delete_common(key, subkey, 1);
}

W32_LONG W32ABI RegDeleteKeyA(W32_HKEY key, const char *subkey) {
    if (!subkey || strlen(subkey) > REG_MAX_PATH)
        return W32_ERROR_INVALID_PARAMETER;
    uint16_t w[REG_MAX_PATH + 1];
    w32_utf8z_to_utf16(subkey, w, REG_MAX_PATH + 1);
    return reg_delete_common(key, w, 1);
}

W32_LONG W32ABI RegDeleteKeyExW(W32_HKEY key, const uint16_t *subkey,
                                W32_ULONG sam, W32_ULONG reserved) {
    (void)sam; (void)reserved;      /* the view-specific flags: same tree */
    return reg_delete_common(key, subkey, 1);
}

/* ---- enumeration (insertion order -- documented) ------------------------------ */

static W32_LONG reg_enum_common(W32_HKEY key, W32_DWORD index,
                                uint16_t *name, W32_DWORD *cap,
                                uint64_t *mtime_out) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (!name || !cap) return W32_ERROR_INVALID_PARAMETER;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(base, base_n, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_HANDLE;

    reg_key_t *snap = NULL;
    const reg_key_t *k;
    if (root == ROOT_HKCR) {
        snap = hkcr_merge(c, n);
        k = snap;
        if (!k) return W32_ERROR_FILE_NOT_FOUND;
    } else {
        k = resolve_for_read(root, c, n);
        if (!k) return W32_ERROR_FILE_NOT_FOUND;
    }
    W32_LONG rc = W32_ERROR_SUCCESS;
    if (index >= k->nsubs) {
        rc = W32_ERROR_NO_MORE_ITEMS;
    } else if (*cap < k->subs[index]->name_units + 1) {
        *cap = (W32_DWORD)(k->subs[index]->name_units + 1);
        rc = W32_ERROR_MORE_DATA;
    } else {
        memcpy(name, k->subs[index]->name,
               (k->subs[index]->name_units + 1) * sizeof(uint16_t));
        *cap = (W32_DWORD)k->subs[index]->name_units;
        if (mtime_out) *mtime_out = k->subs[index]->mtime;
    }
    if (snap) reg_free_key(snap);
    return rc;
}

W32_LONG W32ABI RegEnumKeyExW(W32_HKEY key, W32_DWORD index, uint16_t *name,
                              W32_DWORD *cap, W32_ULONG *reserved,
                              uint16_t *cls, W32_DWORD *cls_cap,
                              void *ft_last_write) {
    (void)reserved;
    if (cls) cls[0] = 0;                       /* no key classes, documented */
    if (cls_cap) *cls_cap = 0;
    uint64_t mtime = 0;
    W32_LONG rc = reg_enum_common(key, index, name, cap, &mtime);
    if (rc == W32_ERROR_SUCCESS && ft_last_write) {
        uint32_t lo = (uint32_t)mtime, hi = (uint32_t)(mtime >> 32);
        memcpy(ft_last_write, &lo, 4);
        memcpy((uint8_t *)ft_last_write + 4, &hi, 4);
    }
    return rc;
}

W32_LONG W32ABI RegEnumKeyA(W32_HKEY key, W32_DWORD index, char *name,
                            W32_DWORD cap) {
    uint16_t w[REG_MAX_NAME + 1];
    W32_DWORD wcap = REG_MAX_NAME;
    W32_LONG rc = reg_enum_common(key, index, w, &wcap, NULL);
    if (rc != W32_ERROR_SUCCESS) return rc;
    size_t need = 0;
    w32_utf16_to_utf8(w, wcap + 1, NULL, 0, &need);   /* measure: bytes+1 */
    if (need + 1 > cap) return W32_ERROR_MORE_DATA;
    w32_utf16z_to_utf8(w, name, (int)cap);
    return W32_ERROR_SUCCESS;
}

/* ---- key info ------------------------------------------------------------------ */

W32_LONG W32ABI RegQueryInfoKeyW(W32_HKEY key, uint16_t *cls, W32_DWORD *cls_cap,
                                 W32_ULONG *reserved, W32_DWORD *subkeys,
                                 W32_DWORD *max_subkey_len, W32_DWORD *max_class_len,
                                 W32_DWORD *values, W32_DWORD *max_value_name_len,
                                 W32_DWORD *max_value_len, void *security_desc_len,
                                 void *ft_last_write) {
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    (void)reserved;
    if (cls) cls[0] = 0;
    if (cls_cap) *cls_cap = 0;
    int root;
    const uint16_t *base; size_t base_n;
    if (root_of(key, &root, &base, &base_n) != 0)
        return W32_ERROR_INVALID_HANDLE;

    comp_t c[REG_MAX_DEPTH];
    int n = split_path(base, base_n, c, REG_MAX_DEPTH);
    if (n < 0) return W32_ERROR_INVALID_HANDLE;

    reg_key_t *snap = NULL;
    const reg_key_t *k;
    if (root == ROOT_HKCR) {
        snap = hkcr_merge(c, n);
        k = snap;
        if (!k) return W32_ERROR_FILE_NOT_FOUND;
    } else {
        k = resolve_for_read(root, c, n);
        if (!k) return W32_ERROR_FILE_NOT_FOUND;
    }

    W32_DWORD nsub = (W32_DWORD)k->nsubs, maxsub = 0;
    for (size_t i = 0; i < k->nsubs; i++)
        if (k->subs[i]->name_units > maxsub) maxsub = (W32_DWORD)k->subs[i]->name_units;
    W32_DWORD nval = (W32_DWORD)k->nvalues, maxvn = 0, maxvl = 0;
    for (size_t i = 0; i < k->nvalues; i++) {
        if (k->values[i].name_units > maxvn) maxvn = (W32_DWORD)k->values[i].name_units;
        if (k->values[i].len > maxvl) maxvl = k->values[i].len;
    }
    if (subkeys) *subkeys = nsub;
    if (max_subkey_len) *max_subkey_len = maxsub;
    if (max_class_len) *max_class_len = 0;
    if (values) *values = nval;
    if (max_value_name_len) *max_value_name_len = maxvn;
    if (max_value_len) *max_value_len = maxvl;
    if (security_desc_len) {
        uint32_t zero = 0;
        memcpy(security_desc_len, &zero, 4);   /* owner-only: no SD size */
    }
    if (ft_last_write) {
        uint32_t lo = (uint32_t)k->mtime, hi = (uint32_t)(k->mtime >> 32);
        memcpy(ft_last_write, &lo, 4);
        memcpy((uint8_t *)ft_last_write + 4, &hi, 4);
    }
    if (snap) reg_free_key(snap);
    return W32_ERROR_SUCCESS;
}

/* ---- RegGetValueW -------------------------------------------------------------- */

/* Expand %VAR% from the process environment (unknown names left intact --
 * documented).  Returns the expanded length in units INCLUDING the NUL, or
 * -1 on allocation failure. */
static int expand_env(const uint16_t *src, size_t units, uint16_t **out) {
    size_t cap = units * 2 + 8;
    uint16_t *dst = malloc(cap * sizeof(uint16_t));
    if (!dst) return -1;
    size_t o = 0;
    for (size_t i = 0; i < units && src[i]; ) {
        if (src[i] == '%' && i + 1 < units) {
            size_t end = i + 1;
            while (end < units && src[end] != '%') end++;
            if (end < units && end > i + 1) {
                char name[256];
                size_t nl = end - i - 1;
                if (nl < sizeof name) {
                    for (size_t j = 0; j < nl; j++)
                        name[j] = (char)src[i + 1 + j];
                    name[nl] = 0;
                    const char *val = getenv(name);
                    if (val) {
                        size_t vl = strlen(val);
                        if (o + vl + 2 > cap) {
                            cap = (o + vl + 2) * 2;
                            uint16_t *g = realloc(dst, cap * sizeof(uint16_t));
                            if (!g) { free(dst); return -1; }
                            dst = g;
                        }
                        for (size_t j = 0; j < vl; j++) dst[o++] = (uint16_t)(unsigned char)val[j];
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        if (o + 2 > cap) {
            cap = (o + 2) * 2;
            uint16_t *g = realloc(dst, cap * sizeof(uint16_t));
            if (!g) { free(dst); return -1; }
            dst = g;
        }
        dst[o++] = src[i++];
    }
    dst[o] = 0;
    *out = dst;
    return (int)(o + 1);
}

W32_LONG W32ABI RegGetValueW(W32_HKEY hkey, const uint16_t *subkey,
                             const uint16_t *value, W32_DWORD flags,
                             W32_DWORD *type, void *data, W32_DWORD *len) {
    /* Open (read-only) the subkey if given, then query with coercion.
     * One open, one size query, at most one data query -- the reopen dance
     * an earlier draft had was both wrong-keyed and unreadable. */
    W32_HKEY target = hkey;
    if (subkey && subkey[0]) {
        W32_LONG orc = reg_open_common(hkey, subkey, &target);
        if (orc != W32_ERROR_SUCCESS) return orc;
    }
    W32_DWORD vtype = 0, vlen = 0;
    W32_LONG rc = reg_query_common(target, value, &vtype, NULL, &vlen);
    if (rc == W32_ERROR_SUCCESS) {
        int want_sz  = (flags & W32_RRF_RT_REG_SZ) != 0;
        int want_bin = (flags & W32_RRF_RT_REG_BINARY) != 0;
        int want_dw  = (flags & W32_RRF_RT_REG_DWORD) != 0;
        if (want_sz || want_bin || want_dw) {
            int ok = (want_sz && (vtype == W32_REG_SZ || vtype == W32_REG_EXPAND_SZ))
                   || (want_bin && vtype == W32_REG_BINARY)
                   || (want_dw && vtype == W32_REG_DWORD);
            if (!ok) rc = W32_ERROR_UNSUPPORTED_TYPE;
            else if (vtype == W32_REG_EXPAND_SZ && want_sz &&
                     (flags & W32_RRF_NOEXPAND))
                rc = W32_ERROR_INVALID_PARAMETER;
        }
    }
    if (rc == W32_ERROR_SUCCESS &&
        vtype == W32_REG_EXPAND_SZ && (flags & W32_RRF_RT_REG_SZ) &&
        !(flags & W32_RRF_NOEXPAND)) {
        /* the documented expand-on-read direction. */
        uint8_t *tmp = malloc(vlen ? vlen : 2);
        if (!tmp) rc = W32_ERROR_NOT_ENOUGH_MEMORY;
        else {
            W32_DWORD got = vlen;
            rc = reg_query_common(target, value, NULL, tmp, &got);
            if (rc == W32_ERROR_SUCCESS) {
                uint16_t *expanded = NULL;
                int elen = expand_env((const uint16_t *)tmp, got / 2, &expanded);
                if (elen < 0) rc = W32_ERROR_NOT_ENOUGH_MEMORY;
                else {
                    uint32_t need = (uint32_t)(elen * 2);
                    if (type) *type = W32_REG_SZ;
                    if (!data) { if (len) *len = need; }
                    else if (len && *len < need) { *len = need; rc = W32_ERROR_MORE_DATA; }
                    else { memcpy(data, expanded, need); if (len) *len = need; }
                    free(expanded);
                }
            }
            free(tmp);
        }
    } else if (rc == W32_ERROR_SUCCESS) {
        if (type) *type = vtype;
        if (!data) { if (len) *len = vlen; }
        else {
            if (len && *len < vlen) { *len = vlen; rc = W32_ERROR_MORE_DATA; }
            else {
                W32_DWORD got = len ? *len : 0;
                rc = reg_query_common(target, value, NULL, (uint8_t *)data, &got);
                if (rc == W32_ERROR_SUCCESS && len) *len = got;
            }
        }
    }
    if (subkey && subkey[0] && target) RegCloseKey(target);
    return rc;
}

/* ---- flush --------------------------------------------------------------------- */

W32_LONG W32ABI RegFlushKey(W32_HKEY key) {
    (void)key;                    /* flush is whole-hive, not per-key */
    hive_ensure_loaded();
    if (hive_corrupt) return W32_ERROR_FILE_CORRUPT;
    if (hive_save() != 0) return W32_ERROR_REGISTRY_IO_FAILED;
    return W32_ERROR_SUCCESS;
}

/* ---- identity ---------------------------------------------------------------
 * The single-user machine: one session, one name ("user", documented --
 * there is no account database to ask), one SID model. */

W32_BOOL W32ABI GetUserNameW(uint16_t *buf, W32_ULONG *len) {
    static const uint16_t name[] = { 'u','s','e','r',0 };
    const size_t need = 5;                       /* chars incl. the NUL */
    if (!buf || !len) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    if (*len < need) {
        *len = (W32_ULONG)need;
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buf, name, need * sizeof(uint16_t));
    *len = (W32_ULONG)need;
    return 1;
}

W32_BOOL W32ABI GetUserNameA(char *buf, W32_ULONG *len) {
    if (!buf || !len) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    if (*len < 5) {
        *len = 5;
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buf, "user", 5);
    *len = 5;
    return 1;
}

/* IsTextUnicode: NOT reimplemented here.  The engine has lived in
 * kernel32_loc.c since the base kernel32 surface; the ledger binds
 * it under ADVAPI32 (the historical forwarder pattern), so the bind
 * table entry is the whole story.  Host coverage: test_w32_a9 links
 * kernel32_loc.c and drives it with the same vectors as the fixture. */

/* ---- SIDs -------------------------------------------------------------------- */

W32_DWORD W32ABI GetLengthSid(const void *sid) {
    const W32_SID *s = sid;
    if (!s || s->revision != W32_SID_REVISION || s->sub_count > 8) return 0;
    return (W32_DWORD)(2 + 6 + 4u * s->sub_count);
}

W32_BOOL W32ABI CopySid(W32_DWORD len, void *dst, const void *src) {
    W32_DWORD need = GetLengthSid(src);
    if (!need || !dst) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    if (len < need) { w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, src, need);
    return 1;
}

W32_BOOL W32ABI EqualSid(const void *a, const void *b) {
    W32_DWORD la = GetLengthSid(a), lb = GetLengthSid(b);
    if (!la || !lb || la != lb) return 0;
    return memcmp(a, b, la) == 0;
}

W32_BOOL W32ABI AllocateAndInitializeSid(const W32_SID_IDENTIFIER_AUTHORITY *auth,
                                         uint8_t count,
                                         W32_DWORD s0, W32_DWORD s1, W32_DWORD s2,
                                         W32_DWORD s3, W32_DWORD s4, W32_DWORD s5,
                                         W32_DWORD s6, W32_DWORD s7,
                                         void **out_sid) {
    if (!auth || !out_sid || count > 8) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    W32_SID *sid = malloc(sizeof *sid);      /* the fixed sub[8] tail is
                                              * always allocated; only the
                                              * first sub_count entries are
                                              * ever read (GetLengthSid) */
    if (!sid) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    sid->revision = W32_SID_REVISION;
    sid->sub_count = count;
    memcpy(sid->authority, auth->value, 6);
    W32_DWORD subs[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    for (int i = 0; i < 8; i++) sid->sub[i] = subs[i];
    *out_sid = sid;
    return 1;
}

void *W32ABI FreeSid(void *sid) {
    free(sid);
    return NULL;
}

W32_BOOL W32ABI CheckTokenMembership(W32_HANDLE token, const void *sid,
                                     W32_BOOL *is_member) {
    if (!is_member) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    *is_member = 0;
    if (token) {
        /* There is exactly one token -- the process's own -- and no way to
         * hold a handle to another (OpenProcessToken refuses).  A non-NULL
         * handle therefore cannot name a real token. */
        w32_set_last_error(W32_ERROR_NO_TOKEN);
        return 0;
    }
    if (!sid) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }

    /* The documented single-user model: the session user is
     * S-1-5-21-0-0-1000 and is a member of BUILTIN\Administrators
     * (S-1-5-32-544).  Both answer TRUE; everything else answers FALSE.
     * This is the one sanctioned "admin" -- see docs/win32.md. */
    static const W32_SID_IDENTIFIER_AUTHORITY nt = { {0, 0}, {0,0,0,0,0,5} };
    void *admin = NULL, *me = NULL;
    W32_BOOL ok = AllocateAndInitializeSid(&nt, 2, 32, 544, 0,0,0,0,0,0, &admin);
    if (ok) ok = AllocateAndInitializeSid(&nt, 4, 21, 0, 0, 1000, 0,0,0,0, &me);
    if (ok) {
        if (EqualSid(sid, admin) || EqualSid(sid, me)) *is_member = 1;
    }
    if (admin) FreeSid(admin);
    if (me) FreeSid(me);
    if (!ok) return 0;                 /* last error already set */
    return 1;
}

/* ---- security descriptors (the builder set) ---------------------------------- */

W32_BOOL W32ABI InitializeSecurityDescriptor(void *sd, W32_DWORD revision) {
    if (!sd || revision != W32_SECURITY_DESCRIPTOR_REVISION) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(sd, 0, sizeof(W32_SECURITY_DESCRIPTOR));
    ((W32_SECURITY_DESCRIPTOR *)sd)->revision = W32_SECURITY_DESCRIPTOR_REVISION;
    return 1;
}

W32_BOOL W32ABI SetSecurityDescriptorDacl(void *sd, W32_BOOL present,
                                          void *dacl, W32_BOOL defaulted) {
    if (!sd) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    W32_SECURITY_DESCRIPTOR *d = sd;
    if (present) {
        d->control |= W32_SE_DACL_PRESENT;
        d->control &= ~(W32_DWORD)W32_SE_DACL_DEFAULTED;
        if (defaulted) d->control |= W32_SE_DACL_DEFAULTED;
        d->dacl = (uint64_t)(uintptr_t)dacl;
    } else {
        d->control &= ~(W32_DWORD)W32_SE_DACL_PRESENT;
        d->dacl = 0;
    }
    return 1;
}

W32_BOOL W32ABI SetSecurityDescriptorOwner(void *sd, void *owner,
                                           W32_BOOL defaulted) {
    if (!sd) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    W32_SECURITY_DESCRIPTOR *d = sd;
    d->control &= ~(W32_DWORD)W32_SE_OWNER_DEFAULTED;
    if (defaulted) d->control |= W32_SE_OWNER_DEFAULTED;
    d->owner = (uint64_t)(uintptr_t)owner;
    return 1;
}

/* ---- CryptoAPI (hash-only, onto libatls) --------------------------------------
 * Uniform engine, documented: every algorithm buffers its input and the
 * digest is computed on demand at CryptGetHashParam(HP_HASHVAL).  SHA-256
 * and SHA-512 are libatls one-shots over the buffer; SHA-3 likewise.
 * CALG_SHA_384 / CALG_SHA1 / CALG_MD5 refuse with NTE_BAD_ALGID (the
 * libatls set is the contract; no receipt shows a core flow needing
 * SHA-1 -- the plan's recorded decision). */

typedef struct {
    int       used;
    int       prov_ok;              /* belongs to a live provider slot    */
    uint32_t  alg;
    uint8_t  *buf;
    size_t    len, cap;
} crypt_hash_t;

static struct { int used; } crypt_provs[REG_CRYPTO_PROVS];
static crypt_hash_t crypt_hashes[REG_CRYPTO_HASHES];

static int hash_size(uint32_t alg) {
    if (alg == W32_CALG_SHA_256 || alg == W32_CALG_SHA3_256) return 32;
    if (alg == W32_CALG_SHA_512 || alg == W32_CALG_SHA3_512) return 64;
    return 0;
}

W32_BOOL W32ABI CryptAcquireContextW(W32_HCRYPTPROV *out, const uint16_t *container,
                                     const uint16_t *provider, W32_DWORD type,
                                     W32_ULONG flags) {
    (void)container; (void)provider; (void)type; (void)flags;
    /* One in-memory CSP; container/provider names and type are recorded
     * and ignored (documented: the single-user machine has no key
     * database to name). */
    if (!out) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    *out = NULL;
    for (int i = 0; i < REG_CRYPTO_PROVS; i++) {
        if (!crypt_provs[i].used) {
            crypt_provs[i].used = 1;
            *out = (W32_HCRYPTPROV)(uintptr_t)&crypt_provs[i];
            return 1;
        }
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

W32_BOOL W32ABI CryptReleaseContext(W32_HCRYPTPROV prov, W32_ULONG flags) {
    (void)flags;
    for (int i = 0; i < REG_CRYPTO_PROVS; i++) {
        if ((W32_HCRYPTPROV)(uintptr_t)&crypt_provs[i] == prov && crypt_provs[i].used) {
            /* hashes stop being usable with their provider, like the real thing */
            crypt_provs[i].used = 0;
            for (int h = 0; h < REG_CRYPTO_HASHES; h++)
                if (crypt_hashes[h].used && crypt_hashes[h].prov_ok == i + 1) {
                    free(crypt_hashes[h].buf);
                    memset(&crypt_hashes[h], 0, sizeof crypt_hashes[h]);
                }
            return 1;
        }
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

W32_BOOL W32ABI CryptCreateHash(W32_HCRYPTPROV prov, W32_ULONG alg,
                                void *key, W32_ULONG flags, W32_HCRYPTHASH *out) {
    (void)key; (void)flags;          /* HMAC-style keyed hashes: not in the ledger */
    if (!out) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    *out = NULL;
    int prov_i = -1;
    for (int i = 0; i < REG_CRYPTO_PROVS; i++)
        if ((W32_HCRYPTPROV)(uintptr_t)&crypt_provs[i] == prov && crypt_provs[i].used)
            prov_i = i;
    if (prov_i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (!hash_size(alg)) {
        w32_set_last_error(W32_NTE_BAD_ALGID);
        return 0;
    }
    for (int i = 0; i < REG_CRYPTO_HASHES; i++) {
        if (!crypt_hashes[i].used) {
            crypt_hashes[i].used = 1;
            crypt_hashes[i].prov_ok = prov_i + 1;
            crypt_hashes[i].alg = (uint32_t)alg;
            crypt_hashes[i].buf = NULL;
            crypt_hashes[i].len = crypt_hashes[i].cap = 0;
            *out = (W32_HCRYPTHASH)(uintptr_t)&crypt_hashes[i];
            return 1;
        }
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

W32_BOOL W32ABI CryptHashData(W32_HCRYPTHASH hash, const uint8_t *data,
                              W32_DWORD len, W32_ULONG flags) {
    (void)flags;
    for (int i = 0; i < REG_CRYPTO_HASHES; i++) {
        if ((W32_HCRYPTHASH)(uintptr_t)&crypt_hashes[i] == hash && crypt_hashes[i].used) {
            if (crypt_hashes[i].len + len > REG_HASH_MAX_DATA) {
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            if (crypt_hashes[i].len + len > crypt_hashes[i].cap) {
                size_t want = crypt_hashes[i].cap ? crypt_hashes[i].cap : 256;
                while (want < crypt_hashes[i].len + len) want *= 2;
                uint8_t *g = realloc(crypt_hashes[i].buf, want);
                if (!g) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
                crypt_hashes[i].buf = g;
                crypt_hashes[i].cap = want;
            }
            if (len) memcpy(crypt_hashes[i].buf + crypt_hashes[i].len, data, len);
            crypt_hashes[i].len += len;
            return 1;
        }
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

W32_BOOL W32ABI CryptGetHashParam(W32_HCRYPTHASH hash, W32_DWORD param,
                                  uint8_t *data, W32_DWORD *len, W32_ULONG flags) {
    (void)flags;
    for (int i = 0; i < REG_CRYPTO_HASHES; i++) {
        if ((W32_HCRYPTHASH)(uintptr_t)&crypt_hashes[i] == hash && crypt_hashes[i].used) {
            crypt_hash_t *h = &crypt_hashes[i];
            if (!len) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
            if (param == W32_HP_ALGID) {
                if (!data) { *len = 4; return 1; }
                if (*len < 4) { *len = 4; w32_set_last_error(W32_ERROR_MORE_DATA); return 0; }
                uint32_t a = h->alg;
                memcpy(data, &a, 4);
                *len = 4;
                return 1;
            }
            if (param == W32_HP_HASHSIZE) {
                uint32_t sz = (uint32_t)hash_size(h->alg);
                if (!data) { *len = 4; return 1; }
                if (*len < 4) { *len = 4; w32_set_last_error(W32_ERROR_MORE_DATA); return 0; }
                memcpy(data, &sz, 4);
                *len = 4;
                return 1;
            }
            if (param == W32_HP_HASHVAL) {
                uint32_t sz = (uint32_t)hash_size(h->alg);
                if (!data) { *len = sz; return 1; }
                if (*len < sz) { *len = sz; w32_set_last_error(W32_ERROR_MORE_DATA); return 0; }
                uint8_t digest[64];
                if (h->alg == W32_CALG_SHA_256)      atls_sha256(h->buf, h->len, digest);
                else if (h->alg == W32_CALG_SHA_512) atls_sha512(h->buf, h->len, digest);
                else if (h->alg == W32_CALG_SHA3_256) atls_sha3_256(h->buf, h->len, digest);
                else if (h->alg == W32_CALG_SHA3_512) atls_sha3_512(h->buf, h->len, digest);
                else { w32_set_last_error(W32_NTE_BAD_ALGID); return 0; }
                memcpy(data, digest, sz);
                *len = sz;
                return 1;
            }
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

W32_BOOL W32ABI CryptDestroyHash(W32_HCRYPTHASH hash) {
    for (int i = 0; i < REG_CRYPTO_HASHES; i++) {
        if ((W32_HCRYPTHASH)(uintptr_t)&crypt_hashes[i] == hash && crypt_hashes[i].used) {
            free(crypt_hashes[i].buf);
            memset(&crypt_hashes[i], 0, sizeof crypt_hashes[i]);
            return 1;
        }
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

/* ---- RtlGenRandom (SystemFunction036) ------------------------------------------ */

W32_BOOL W32ABI SystemFunction036(void *buf, W32_ULONG len) {
    if (!buf) return 0;
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(p + done, len - done, 0);
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

/* ---- the FAIL-CLEAN finals -------------------------------------------------------
 * Final, documented refusals.  Single-user machine: no LSA database, no
 * account table, no privileges to hold, no file ACLs.  Each logs its
 * reason once and answers with the documented code. */

static int n_lsa_open, n_lsa_add, n_lsa_close, n_lookup_acct, n_lookup_priv,
            n_open_tok, n_adj_tok, n_gfs, n_sfs;

W32_LONG W32ABI LsaOpenPolicy(void *system_name, void *attr,
                              W32_ULONG access, void **handle) {
    (void)system_name; (void)attr; (void)access;
    if (handle) *handle = NULL;
    note_once(&n_lsa_open,
        "advapi32!LsaOpenPolicy: refused -- no LSA database on the "
        "single-user machine (STATUS_ACCESS_DENIED)");
    return W32_STATUS_ACCESS_DENIED;
}

W32_LONG W32ABI LsaAddAccountRights(void *handle, void *sid,
                                    void *user_rights, W32_ULONG count) {
    (void)handle; (void)sid; (void)user_rights; (void)count;
    note_once(&n_lsa_add,
        "advapi32!LsaAddAccountRights: refused -- no LSA database on the "
        "single-user machine (STATUS_ACCESS_DENIED)");
    return W32_STATUS_ACCESS_DENIED;
}

W32_LONG W32ABI LsaClose(void *handle) {
    (void)handle;
    note_once(&n_lsa_close,
        "advapi32!LsaClose: refused -- no LSA handle can exist on the "
        "single-user machine (STATUS_INVALID_HANDLE)");
    return W32_STATUS_INVALID_HANDLE;
}

W32_BOOL W32ABI LookupAccountNameW(const uint16_t *system_name,
                                   const uint16_t *account, void *sid,
                                   W32_DWORD *sid_len, void *domain,
                                   W32_DWORD *domain_len, W32_DWORD *name_use) {
    (void)system_name; (void)account; (void)sid; (void)sid_len;
    (void)domain; (void)domain_len; (void)name_use;
    note_once(&n_lookup_acct,
        "advapi32!LookupAccountNameW: refused -- no account database on "
        "the single-user machine (ERROR_NONE_MAPPED)");
    w32_set_last_error(W32_ERROR_NONE_MAPPED);
    return 0;
}

W32_BOOL W32ABI LookupPrivilegeValueW(const uint16_t *system_name,
                                      const uint16_t *name, void *luid) {
    (void)system_name; (void)name; (void)luid;
    note_once(&n_lookup_priv,
        "advapi32!LookupPrivilegeValueW: refused -- no privilege database "
        "on the single-user machine (ERROR_NO_SUCH_PRIVILEGE)");
    w32_set_last_error(W32_ERROR_NO_SUCH_PRIVILEGE);
    return 0;
}

W32_BOOL W32ABI OpenProcessToken(W32_HANDLE process, W32_ULONG access,
                                 W32_HANDLE *token) {
    (void)process; (void)access;
    if (token) *token = NULL;
    note_once(&n_open_tok,
        "advapi32!OpenProcessToken: refused -- no token object exists on "
        "the single-user machine (ERROR_NO_TOKEN)");
    w32_set_last_error(W32_ERROR_NO_TOKEN);
    return 0;
}

W32_BOOL W32ABI AdjustTokenPrivileges(W32_HANDLE token, W32_BOOL disable_all,
                                      void *new_state, W32_ULONG buf_len,
                                      void *prev_state, W32_ULONG *ret_len) {
    (void)token; (void)disable_all; (void)new_state; (void)buf_len;
    (void)prev_state; (void)ret_len;
    /* The documented degradation: the call "succeeds" having assigned
     * nothing -- GetLastError() is ERROR_NOT_ALL_ASSIGNED -- so a caller
     * with a backup-privilege plan (7-Zip's) falls back to normal file
     * access and reports its own error.  No privileges exist to hold. */
    note_once(&n_adj_tok,
        "advapi32!AdjustTokenPrivileges: no privileges exist on the "
        "single-user machine; nothing assigned (ERROR_NOT_ALL_ASSIGNED)");
    w32_set_last_error(W32_ERROR_NOT_ALL_ASSIGNED);
    return 1;
}

W32_BOOL W32ABI GetFileSecurityW(const uint16_t *path, W32_ULONG info,
                                 void *sd, W32_DWORD len, W32_DWORD *needed) {
    (void)path; (void)info; (void)sd; (void)len; (void)needed;
    note_once(&n_gfs,
        "advapi32!GetFileSecurityW: refused -- the VFS is owner-only, "
        "there are no file ACLs to read (ERROR_NOT_SUPPORTED)");
    w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
    return 0;
}

W32_BOOL W32ABI SetFileSecurityW(const uint16_t *path, W32_ULONG info, void *sd) {
    (void)path; (void)info; (void)sd;
    note_once(&n_sfs,
        "advapi32!SetFileSecurityW: refused -- the VFS is owner-only, "
        "there are no file ACLs to set (ERROR_NOT_SUPPORTED)");
    w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
    return 0;
}

/* Defined last on purpose: it touches every table above (host tests
 * only; the guest never calls it). */
void w32_advapi_reset_for_host_test(void) {
    reg_free_key(hkcu_root);
    reg_free_key(hklm_root);
    hkcu_root = hklm_root = NULL;
    hive_loaded = 0;
    hive_corrupt = 0;
    hive_missing_ok = 0;
    hive_seq = 0;
    memset(reg_handles, 0, sizeof reg_handles);
    for (int i = 0; i < REG_CRYPTO_HASHES; i++) free(crypt_hashes[i].buf);
    memset(crypt_hashes, 0, sizeof crypt_hashes);
    memset(crypt_provs, 0, sizeof crypt_provs);
}
