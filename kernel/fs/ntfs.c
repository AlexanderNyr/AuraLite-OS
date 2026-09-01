/* kernel/fs/ntfs.c — F5b, NTFS read-only driver.
 *
 * Replaces the former skeleton (which returned a fake vnode on every lookup
 * and the MFT buffer as file data).  This is a real NTFS reader: boot
 * sector, MFT records with update-sequence-array fixup, attribute walking,
 * runlist decoding, file reads through runs, and $I30 directory reads.
 *
 * The volume is mounted read-only.  Every mutation operation refuses with
 * -EROFS and prints a `[ntfs] ... refused: read-only (-EROFS)` line, so
 * nothing on this filesystem ever pretends to write.
 *
 * I/O goes through the F2 buffer-cache seam (fs_read_block) like every other
 * filesystem in FSFULL_PLAN.md.
 */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"

/* ASCII-only case-insensitive name equality (NTFS filenames are compared
 * case-insensitively; full Unicode upcasing is out of scope — see ntfs.h). */
static int ntfs_name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return (*a == 0 && *b == 0);
}

/* ---- module state (single mount) ---- */
static int      ntfs_dev          = -1;
static int      ntfs_bps          = 512;   /* bytes per sector */
static int      ntfs_spc          = 8;     /* sectors per cluster */
static uint64_t ntfs_cluster_size = 0;     /* bytes */
static uint64_t ntfs_mft_lcn      = 0;     /* boot-sector $MFT hint */
static int64_t  ntfs_file_rec_sz  = 1024;  /* bytes per MFT record */
static int64_t  ntfs_index_buf_sz = 4096;  /* bytes per index buffer */
static int      ntfs_mounted      = 0;

/* $MFT extent list, decoded from record 0's $DATA runlist (fallback: a
 * single run starting at the boot-sector $MFT LCN). */
#define NTFS_MAX_RUNS 32
static int64_t mft_runs_lcn[NTFS_MAX_RUNS];
static int64_t mft_runs_len[NTFS_MAX_RUNS];
static int     mft_runs_n;

/* ---- block I/O through the F2 cache seam ---- */
/* Module-static scratch, so the read path does not need 12+ KiB of kernel
 * stack (the x86_64 kernel stack is 16 KiB).  Single mount, read-only, so
 * a single set of scratch buffers is safe; each buffer is only live while
 * the operation that owns it runs, and the directory-size fill-in loop
 * deliberately uses a separate record buffer so it does not clobber the
 * directory record it is iterating. */
static uint8_t scratch_rec[NTFS_MAX_REC_SZ];    /* a MFT record */
static uint8_t scratch_child[NTFS_MAX_REC_SZ];  /* child MFT record */
static uint8_t scratch_sec[512];                /* one sector */
static uint8_t scratch_idxbuf[NTFS_MAX_REC_SZ]; /* one index buffer */

static int ntfs_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    return fs_read_block(ntfs_dev, lba, count, buf);
}
/* ---- update-sequence-array fixup on a raw record ---- */
static void ntfs_fixup(uint8_t *rec, int rec_sz) {
    if (rec_sz < 512) return;
    uint16_t usa_off = rec[4] | (rec[5] << 8);
    uint16_t usa_cnt = rec[6] | (rec[7] << 8);
    if (usa_off < 4 || usa_cnt < 2) return;
    /* The USA array occupies [usa_off, usa_off+2*usa_cnt); guard the whole
     * array against rec_sz before reading any element, so a record whose
     * USA fields are garbage (e.g. an unmounted/fake volume) cannot over-read. */
    if (usa_off + 2u * (uint32_t)(usa_cnt - 1) + 1u >= (uint32_t)rec_sz) return;
    for (int i = 1; i < usa_cnt; i++) {
        uint16_t val = rec[usa_off + 2 * i] | (rec[usa_off + 2 * i + 1] << 8);
        int sec = (i - 1) * 512;
        if (sec + 510 + 2 > rec_sz) break;
        rec[sec + 510] = (uint8_t)(val & 0xFF);
        rec[sec + 511] = (uint8_t)(val >> 8);
    }
}

/* ---- runlist decode (sparse (lcn,count) runs) ---- */
static int ntfs_parse_runs(const uint8_t *rp, int avail,
                           int64_t *out_lcn, int64_t *out_len, int max) {
    int n = 0;
    int64_t lcn = 0;
    while (n < max) {
        if (avail < 1) return -1;
        uint8_t h = *rp++;
        avail--;
        if (h == 0) break;
        int lsize = h & 0xF, osize = h >> 4;
        int64_t len = 0;
        for (int i = 0; i < lsize; i++) {
            if (avail < 1) return -1;
            len |= ((int64_t)*rp) << (8 * i);
            rp++; avail--;
        }
        int64_t off = 0;
        for (int i = 0; i < osize; i++) {
            if (avail < 1) return -1;
            off |= ((int64_t)*rp) << (8 * i);
            rp++; avail--;
        }
        if (osize && len) {
            int topbit = 1 << (osize * 8 - 1);
            if (off & topbit) off |= ~((1LL << (osize * 8)) - 1);
        }
        lcn += off;
        out_lcn[n] = lcn;
        out_len[n] = len;
        n++;
    }
    return n;
}

/* ---- locate the LCN that holds byte-offset @byte_off of $MFT ---- */
static int64_t ntfs_mft_lcn_for(uint64_t byte_off) {
    uint64_t want = byte_off / ntfs_cluster_size;
    for (int i = 0; i < mft_runs_n; i++) {
        if (want < (uint64_t)mft_runs_len[i])
            return mft_runs_lcn[i] + (int64_t)want;
        want -= (uint64_t)mft_runs_len[i];
    }
    return -1;
}

/* ---- read a whole MFT record, fixup applied ---- */
static int ntfs_mft_read(uint64_t idx, uint8_t *out, int out_sz) {
    uint64_t byte_off = idx * (uint64_t)ntfs_file_rec_sz;
    int64_t lcn = ntfs_mft_lcn_for(byte_off);
    if (lcn < 0) return -1;
    uint64_t within = byte_off % ntfs_cluster_size;
    uint64_t sec = (uint64_t)lcn * (uint64_t)ntfs_spc + within / 512;
    uint32_t boff = (uint32_t)(within % 512);
    int copied = 0;
    if (ntfs_read_sectors(sec, 1, scratch_sec)) return -1;
    int take = 512 - (int)boff;
    if (out_sz - copied < take) take = out_sz - copied;
    uint32_t c = (uint32_t)take;
    memcpy(out, scratch_sec + boff, c);
    copied = (int)c;
    while (copied < out_sz) {
        sec++;
        if (ntfs_read_sectors(sec, 1, scratch_sec)) return -1;
        uint32_t k = (uint32_t)(out_sz - copied < 512 ? out_sz - copied : 512);
        memcpy(out + copied, scratch_sec, k);
        copied += (int)k;
    }
    ntfs_fixup(out, out_sz);
    return 0;
}

/* ---- attribute walk ---- */
static const uint8_t *ntfs_attr(const uint8_t *rec, int rec_sz,
                                uint32_t type, const char *want_name) {
    uint16_t aoff = rec[0x14] | (rec[0x15] << 8);
    int p = aoff;
    while (p + 8 <= rec_sz) {
        uint32_t t = *(const uint32_t *)(rec + p);
        if (t == NTFS_ATTR_END) return NULL;
        uint32_t alen = *(const uint32_t *)(rec + p + 4);
        if (alen < 16u || (uint32_t)p + alen > (uint32_t)rec_sz) return NULL;
        if (t == type) {
            uint8_t nlen = rec[p + 9];
            if (want_name == NULL || want_name[0] == 0) {
                if (nlen == 0) return rec + p;
            } else {
                uint16_t noff = rec[p + 10] | (rec[p + 11] << 8);
                int wl = (int)strlen(want_name);
                if (nlen == (uint8_t)wl) {
                    int ok = 1;
                    for (int i = 0; i < wl; i++) {
                        uint16_t c = rec[p + noff + 2 * i] | (rec[p + noff + 2 * i + 1] << 8);
                        if (c != (uint8_t)want_name[i]) { ok = 0; break; }
                    }
                    if (ok) return rec + p;
                }
            }
        }
        p += (int)alen;
    }
    return NULL;
}

/* ---- attributes of a record ---- */
static uint64_t ntfs_attr_nonres_real_size(const uint8_t *a) {
    return *(const uint64_t *)(a + 0x30);   /* real size for non-resident */
}
static int ntfs_record_data_size(const uint8_t *rec, int rec_sz, uint64_t *out) {
    const uint8_t *d = ntfs_attr(rec, rec_sz, NTFS_AT_DATA, NULL);
    if (!d) { *out = 0; return -1; }
    if (!(d[8] & NTFS_ATTR_NONRESID)) {
        *out = *(const uint32_t *)(d + 16);            /* resident value length */
    } else {
        *out = ntfs_attr_nonres_real_size(d);
    }
    return 0;
}
static int ntfs_record_is_dir(const uint8_t *rec, int rec_sz) {
    /* $STANDARD_INFORMATION flags at value+0x20 */
    const uint8_t *si = ntfs_attr(rec, rec_sz, NTFS_AT_STANDARD_INFORMATION, NULL);
    if (si && !(si[8] & NTFS_ATTR_NONRESID)) {
        uint32_t vlen = *(const uint32_t *)(si + 16);
        uint16_t voff = *(const uint16_t *)(si + 20);
        if (vlen >= 0x24)
            return (*(const uint32_t *)(si + voff + 0x20) & NTFS_ATTR_FLAG_DIRECTORY) != 0;
    }
    return 0;
}

/* ---- read through a non-resident attribute's OWN runlist ---- */
static int64_t ntfs_run_read(const uint8_t *attr, int attr_avail,
                             uint64_t pos, void *buf, uint64_t count) {
    uint64_t realsize = ntfs_attr_nonres_real_size(attr);
    if (pos >= realsize) return 0;
    uint64_t n = count; if (pos + n > realsize) n = realsize - pos;
    uint16_t rp_off = attr[0x20] | (attr[0x21] << 8);
    int64_t rlcn[NTFS_MAX_RUNS], rlen[NTFS_MAX_RUNS];
    int nr = ntfs_parse_runs(attr + rp_off, attr_avail - rp_off,
                             rlcn, rlen, NTFS_MAX_RUNS);
    if (nr < 0) return -1;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < n) {
        uint64_t abs = pos + done;
        uint64_t clu = abs / ntfs_cluster_size;
        uint64_t within = abs % ntfs_cluster_size;
        int64_t lcn = -1;
        uint64_t cw = clu;
        for (int i = 0; i < nr; i++) {
            if (cw < (uint64_t)rlen[i]) { lcn = rlcn[i] + (int64_t)cw; break; }
            cw -= (uint64_t)rlen[i];
        }
        if (lcn < 0) return -1;
        if (lcn == 0) {                    /* sparse run: zeros */
            uint64_t z = n - done; uint64_t rem = ntfs_cluster_size - within;
            if (z > rem) z = rem;
            memset(out + done, 0, (size_t)z);
            done += z;
            continue;
        }
        uint64_t sec = (uint64_t)lcn * (uint64_t)ntfs_spc + within / 512;
        uint32_t boff = (uint32_t)(within % 512);
        if (ntfs_read_sectors(sec, 1, scratch_sec)) return -1;
        uint32_t c = (uint32_t)(512 - boff);
        if (c > n - done) c = (uint32_t)(n - done);
        memcpy(out + done, scratch_sec + boff, c);
        done += c;
    }
    return (int64_t)n;
}

/* ---- read through a record's $DATA (resident or runlist) ---- */
static int64_t ntfs_data_read(const uint8_t *rec, int rec_sz,
                              uint64_t pos, void *buf, uint64_t count) {
    const uint8_t *d = ntfs_attr(rec, rec_sz, NTFS_AT_DATA, NULL);
    if (!d) return -1;
    if (!(d[8] & NTFS_ATTR_NONRESID)) {
        uint32_t vlen = *(const uint32_t *)(d + 16);
        uint16_t voff = *(const uint16_t *)(d + 20);
        if (pos >= vlen) return 0;
        uint64_t n = count; if (pos + n > vlen) n = vlen - pos;
        memcpy(buf, d + voff + pos, (size_t)n);
        return (int64_t)n;
    }
    return ntfs_run_read(d, rec_sz - (int)(d - rec), pos, buf, count);
}

/* ---- directory entry enumeration via $I30 ---- */
struct ntfs_dirent {
    uint64_t mft;
    char     name[256];
    int      is_dir;
    uint64_t size;
};
static struct ntfs_dirent scratch_ents[256];    /* directory listing */

/* enumerate the entries held in one index block/root header.  src is the
 * start of the index entries (already pointer-corrected), end their end. */
static void ntfs_index_collect(const uint8_t *src, const uint8_t *end,
                               struct ntfs_dirent *ents, int *n, int max) {
    const uint8_t *p = src;
    while (p + 16 <= end) {
        uint64_t fr = *(const uint64_t *)p;
        uint16_t elen = p[8] | (p[9] << 8);
        uint16_t flags = p[12] | (p[13] << 8);
        if (elen == 0) break;
        if (elen < 16 || p + elen > end) break;
        if (!(flags & 0x01) && (p[10] | (p[11] << 8)) > 0 && *n < max) {   /* has a FILE_NAME value */
            const uint8_t *fn = p + 0x10;
            uint8_t nlen = fn[0x40];
            uint32_t fflags = fn[0x38] | (fn[0x39] << 8) | (fn[0x3A] << 16) | (fn[0x3B] << 24);
            struct ntfs_dirent *e = &ents[*n];
            e->mft = fr & 0xFFFFFFFFFFFFull;
            e->is_dir = (fflags & NTFS_ATTR_FLAG_DIRECTORY) ? 1 : 0;
            int l = nlen < 255 ? nlen : 255;
            for (int i = 0; i < l; i++)
                e->name[i] = (char)(fn[0x42 + 2 * i]);   /* UTF-16LE low byte (ASCII) */
            e->name[l] = 0;
            e->size = 0;
            (*n)++;
        }
        p += elen;
        if (flags & 0x02) break;             /* last entry in this node */
    }
}

/* enumerate a directory's entries from INDEX_ROOT (resident) and, if the
 * root node is "large", from the INDEX_ALLOCATION extents. */
static int ntfs_dir_enum(uint64_t dir_mft, struct ntfs_dirent *ents, int max) {
    uint8_t *rec = scratch_rec;
    int rec_sz = (int)ntfs_file_rec_sz;
    if (rec_sz > (int)sizeof(scratch_rec)) rec_sz = (int)sizeof(scratch_rec);
    if (ntfs_mft_read(dir_mft, rec, rec_sz)) return -1;
    if (memcmp(rec, "FILE", 4) != 0) return -1;

    int n = 0;

    const uint8_t *ir = ntfs_attr(rec, rec_sz, NTFS_AT_INDEX_ROOT, "$I30");
    if (ir && !(ir[8] & NTFS_ATTR_NONRESID)) {
        uint32_t vlen = *(const uint32_t *)(ir + 16);
        uint16_t voff = *(const uint16_t *)(ir + 20);
        const uint8_t *val = ir + voff;
        const uint8_t *h = val + 0x10;                    /* index header */
        uint32_t eoff = *(const uint32_t *)h;
        uint32_t ilen = *(const uint32_t *)(h + 4);
        const uint8_t *start = val + 0x10 + eoff;
        const uint8_t *end = val + 0x10 + ilen;
        if (end > ir + vlen) end = ir + vlen;
        ntfs_index_collect(start, end, ents, &n, max);
    }

    /* If the root node flags a large index, its children are in the
     * INDEX_ALLOCATION $DATA stream as INDX buffers. */
    const uint8_t *ia = ntfs_attr(rec, rec_sz, NTFS_AT_INDEX_ALLOCATION, "$I30");
    if (ia && (ia[8] & NTFS_ATTR_NONRESID) && n < max) {
        uint64_t isz = ntfs_attr_nonres_real_size(ia);
        uint64_t idx_buf_sz = (uint64_t)ntfs_index_buf_sz;
        uint8_t *buf = scratch_idxbuf;
        int ia_avail = rec_sz - (int)(ia - rec);
        if (idx_buf_sz <= (uint64_t)sizeof(scratch_idxbuf)) {
            uint64_t off = 0;
            while (off + idx_buf_sz <= isz && off < 512 * 1024 && n < max) {
                int64_t got = ntfs_run_read(ia, ia_avail, off, buf, idx_buf_sz);
                if (got <= 0) break;
                if (memcmp(buf, "INDX", 4) != 0) { off += idx_buf_sz; continue; }
                ntfs_fixup(buf, (int)idx_buf_sz);
                const uint8_t *h = buf + 0x18;            /* index header */
                uint32_t eoff = *(const uint32_t *)h;
                uint32_t ilen = *(const uint32_t *)(h + 4);
                const uint8_t *start = buf + 0x18 + eoff;
                const uint8_t *end = buf + 0x18 + ilen;
                if (end > buf + idx_buf_sz) end = buf + idx_buf_sz;
                ntfs_index_collect(start, end, ents, &n, max);
                off += idx_buf_sz;
            }
        }
    }

    /* Fill in file sizes (and confirm dirness) from each child's own $DATA.
     * A directory has no unnamed $DATA, so its size stays 0.  Uses the
     * separate scratch_child buffer so iterating does not clobber `rec`. */
    uint8_t *crec = scratch_child;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) continue;
        int cr = ntfs_mft_read(ents[i].mft, crec, rec_sz);
        if (cr == 0 && memcmp(crec, "FILE", 4) == 0) {
            uint64_t sz = 0;
            if (ntfs_record_data_size(crec, rec_sz, &sz) == 0)
                ents[i].size = sz;
            if (ntfs_record_is_dir(crec, rec_sz))
                ents[i].is_dir = 1;
        }
    }
    return n;
}

/* ---- vnode pool ---- */
struct ntfs_vnode {
    int      in_use;
    uint64_t mft;
    char     name[256];
    int      is_dir;
    uint64_t size;
    struct vnode vnode;
};
#define NTFS_MAX_VNODES 64
static struct ntfs_vnode nvpool[NTFS_MAX_VNODES];

static struct ntfs_vnode *ntfs_intern(uint64_t mft, const char *name,
                                      int is_dir, uint64_t size) {
    for (int i = 0; i < NTFS_MAX_VNODES; i++)
        if (nvpool[i].in_use && nvpool[i].mft == mft) {
            nvpool[i].size = size;
            nvpool[i].is_dir = is_dir;
            nvpool[i].vnode.size = size;
            return &nvpool[i];
        }
    for (int i = 0; i < NTFS_MAX_VNODES; i++)
        if (!nvpool[i].in_use) {
            struct ntfs_vnode *v = &nvpool[i];
            memset(v, 0, sizeof(*v));
            v->in_use = 1;
            v->mft = mft;
            strncpy(v->name, name, sizeof(v->name) - 1);
            v->is_dir = is_dir;
            v->size = size;
            v->vnode.type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode = is_dir ? 0755 : 0644;
            v->vnode.size = size;
            v->vnode.ops = &ntfs_ops;
            v->vnode.fs_data = v;
            v->vnode.inode_id = mft;
            return v;
        }
    return NULL;
}

/* ---- path resolution ---- */
static struct vnode *ntfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!ntfs_mounted) return NULL;
    if (!path || !*path || strcmp(path, "/") == 0)
        return &ntfs_intern(NTFS_MFT_INDEX_ROOT, "", 1, 0)->vnode;

    uint64_t cur = NTFS_MFT_INDEX_ROOT;
    const char *p = path;
    char comp[256];
    const char *last = "";
    while (*p) {
        int l = 0;
        while (p[l] && p[l] != '/') { if (l < 255) l++; else break; }
        if (l == 0) { p++; continue; }
        memcpy(comp, p, (size_t)l);
        comp[l] = 0;
        struct ntfs_dirent *ents = scratch_ents;
        int n = ntfs_dir_enum(cur, ents, 256);
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (ntfs_name_eq(ents[i].name, comp)) {
                cur = ents[i].mft;
                last = ents[i].name;
                found = 1;
                break;
            }
        }
        if (!found) return NULL;
        p += l;
        if (*p == '/') p++;
    }

    uint8_t *rec = scratch_child;
    int rec_sz = (int)ntfs_file_rec_sz;
    if (rec_sz > (int)sizeof(scratch_child)) rec_sz = (int)sizeof(scratch_child);
    if (ntfs_mft_read(cur, rec, rec_sz)) return NULL;
    if (memcmp(rec, "FILE", 4) != 0) return NULL;
    int is_dir = ntfs_record_is_dir(rec, rec_sz);
    uint64_t size = 0;
    if (!is_dir) ntfs_record_data_size(rec, rec_sz, &size);
    return &ntfs_intern(cur, last, is_dir, size)->vnode;
}

/* ---- file read ---- */
static int64_t ntfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct ntfs_vnode *v = (struct ntfs_vnode *)vn->fs_data;
    if (!v || !ntfs_mounted) return -1;
    uint8_t *rec = scratch_rec;
    int rec_sz = (int)ntfs_file_rec_sz;
    if (rec_sz > (int)sizeof(scratch_rec)) rec_sz = (int)sizeof(scratch_rec);
    if (ntfs_mft_read(v->mft, rec, rec_sz)) return -1;
    if (memcmp(rec, "FILE", 4) != 0) return -1;
    if (pos >= v->size && !v->is_dir) return 0;
    return ntfs_data_read(rec, rec_sz, pos, buf, count);
}

/* ---- readdir ---- */
static int ntfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct ntfs_vnode *v = (struct ntfs_vnode *)vn->fs_data;
    if (!v || !ntfs_mounted) return -1;
    uint64_t dir_mft = v->mft;
    if (v->is_dir == 0 && (v->mft != NTFS_MFT_INDEX_ROOT)) {
        /* a lookup of a dir sets is_dir; readdir on the root works too */
        uint8_t *rec = scratch_child;
        int rec_sz = (int)ntfs_file_rec_sz;
        if (rec_sz > (int)sizeof(scratch_child)) rec_sz = (int)sizeof(scratch_child);
        if (ntfs_mft_read(dir_mft, rec, rec_sz)) return -1;
        if (!ntfs_record_is_dir(rec, rec_sz)) return -1;
    }
    struct ntfs_dirent *ents = scratch_ents;
    int cap = max > 256 ? 256 : max;
    int n = ntfs_dir_enum(dir_mft, ents, cap);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].name, ents[i].name, VFS_PATH_MAX - 1);
        out[i].inode = ents[i].mft;
        out[i].type = ents[i].is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        out[i].size = ents[i].is_dir ? 0 : ents[i].size;
    }
    return n;
}

/* ---- stat ---- */
static int ntfs_stat(struct vnode *vn, struct vfs_stat *st) {
    struct ntfs_vnode *v = (struct ntfs_vnode *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;
    st->type = v->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->size = v->size;
    st->nlink = 1;
    st->mode = v->is_dir ? 0755 : 0644;
    st->inode = v->mft;
    st->ctime = 0; st->atime = 0; st->mtime = 0;   /* honest: times not decoded */
    return 0;
}

/* ---- read-only refusals ---- */
static int64_t ntfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    (void)vn; (void)pos; (void)buf; (void)count;
    kprintf("[ntfs] write refused: read-only (-EROFS)\n");
    return -EROFS;
}
static struct vnode *ntfs_create(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    kprintf("[ntfs] create refused: read-only (-EROFS)\n");
    return NULL;
}
static int ntfs_mkdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    kprintf("[ntfs] mkdir refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_unlink(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    kprintf("[ntfs] unlink refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_rmdir(void *fs_data, const char *path) {
    (void)fs_data; (void)path;
    kprintf("[ntfs] rmdir refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_rename(void *fs_data, const char *from, const char *to) {
    (void)fs_data; (void)from; (void)to;
    kprintf("[ntfs] rename refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_truncate(struct vnode *vn, uint64_t new_size) {
    (void)vn; (void)new_size;
    kprintf("[ntfs] truncate refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_settimes(struct vnode *vn, uint64_t atime, uint64_t mtime) {
    (void)vn; (void)atime; (void)mtime;
    kprintf("[ntfs] settimes refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_link(void *fs_data, const char *old, const char *new) {
    (void)fs_data; (void)old; (void)new;
    kprintf("[ntfs] link refused: read-only (-EROFS)\n");
    return -EROFS;
}
static int ntfs_sync(void *fs_data) {
    (void)fs_data;
    return 0;
}

const struct vfs_ops ntfs_ops = {
    .lookup   = ntfs_lookup,
    .create   = ntfs_create,
    .read     = ntfs_read,
    .write    = ntfs_write,
    .readdir  = ntfs_readdir,
    .mkdir    = ntfs_mkdir,
    .rmdir    = ntfs_rmdir,
    .unlink   = ntfs_unlink,
    .rename   = ntfs_rename,
    .truncate = ntfs_truncate,
    .settimes = ntfs_settimes,
    .link     = ntfs_link,
    .stat     = ntfs_stat,
    .sync     = ntfs_sync,
};

/* ---- mount (F1: accept only a real NTFS volume) ---- */
int ntfs_init(int device_id) {
    uint8_t s0[512];
    if (fs_read_block(device_id, 0, 1, s0) != 0) {
        kprintf("[ntfs] sector 0 of device %d unreadable; /ntfs not mounted\n",
                device_id);
        return -1;
    }
    if (memcmp(s0 + 3, "NTFS    ", 8) != 0) {
        kprintf("[ntfs] not NTFS signature (OEM '%.8s'); /ntfs not mounted\n",
                s0 + 3);
        return -1;
    }
    int bps = s0[11] | (s0[12] << 8);
    int spc = s0[13];
    if (bps != 512 || spc < 1 || (spc & (spc - 1)) != 0) {
        kprintf("[ntfs] unsupported geometry (bps=%d spc=%d); /ntfs not mounted\n",
                bps, spc);
        return -1;
    }

    ntfs_dev = device_id;
    ntfs_bps = bps;
    ntfs_spc = spc;
    ntfs_cluster_size = (uint64_t)bps * (uint64_t)spc;

    int8_t cpf = (int8_t)s0[0x40];
    if (cpf < 0)      ntfs_file_rec_sz = 1LL << (-cpf);
    else              ntfs_file_rec_sz = (int64_t)ntfs_cluster_size * cpf;
    int8_t cib = (int8_t)s0[0x44];
    if (cib < 0)      ntfs_index_buf_sz = 1LL << (-cib);
    else              ntfs_index_buf_sz = (int64_t)ntfs_cluster_size * cib;
    if (ntfs_file_rec_sz < 256 || ntfs_file_rec_sz > 8192) {
        kprintf("[ntfs] implausible MFT record size %d; /ntfs not mounted\n",
                (int)ntfs_file_rec_sz);
        return -1;
    }

    ntfs_mft_lcn = *(const uint64_t *)(s0 + 0x30);

    /* Locate the $MFT through record 0's own $DATA runlist; fall back to a
     * single run at the boot-sector $MFT LCN if record 0 cannot be read. */
    mft_runs_n = 0;
    mft_runs_lcn[0] = (int64_t)ntfs_mft_lcn;
    mft_runs_len[0] = 1024;             /* optimistic contiguous extent */
    mft_runs_n = 1;

    uint8_t *r0 = scratch_child;
    int r0_sz = (int)ntfs_file_rec_sz;
    if (r0_sz > (int)sizeof(scratch_child)) r0_sz = (int)sizeof(scratch_child);
    if (ntfs_mft_read(NTFS_MFT_INDEX_MFT, r0, r0_sz) == 0 &&
        memcmp(r0, "FILE", 4) == 0) {
        const uint8_t *d = ntfs_attr(r0, r0_sz, NTFS_AT_DATA, NULL);
        if (d && (d[8] & NTFS_ATTR_NONRESID)) {
            uint16_t rp_off = d[0x20] | (d[0x21] << 8);
            int nr = ntfs_parse_runs(d + rp_off, (int)(r0_sz - rp_off),
                                     mft_runs_lcn, mft_runs_len, NTFS_MAX_RUNS);
            if (nr > 0) mft_runs_n = nr;
        }
    }

    ntfs_mounted = 1;
    kprintf("[ntfs] mounted read-only volume on device %d "
            "(cluster %llu B, MFT record %d B, %d extent(s))\n",
            device_id, (unsigned long long)ntfs_cluster_size,
            (int)ntfs_file_rec_sz, mft_runs_n);
    return 0;
}

/* ---- structural self-test against the mounted volume ---- */
int ntfs_self_test(void) {
    if (!ntfs_mounted) {
        kprintf("[ntfs] self-test: not mounted\n");
        return -1;
    }
    struct vnode *root = ntfs_lookup(NULL, "/");
    if (!root) { kprintf("[ntfs] FAIL: root lookup\n"); return -1; }
    struct vfs_dirent out[8];
    int n = ntfs_readdir(root, out, 8);
    if (n < 0) { kprintf("[ntfs] FAIL: root readdir\n"); return -2; }
    kprintf("[ntfs] self-test: root has %d entries (read-only)\n", n);
    return 0;
}
