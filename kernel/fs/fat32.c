/* fat32.c — full FAT32 with subdirectories, LFN, FSInfo, mkdir/rmdir/unlink/rename.
 *
 * On-disk layout (we mount whatever is at LBA 64; format only if absent):
 *
 *   +-----------------------------+ LBA 64       (BPB / Boot sector)
 *   +-----------------------------+ LBA 65       (FSInfo sector)
 *   +-----------------------------+ LBA 70       (backup BPB at sector 6 of vol)
 *   +-----------------------------+ LBA 96       (FAT[0] start, fatsz sectors)
 *   ...
 *   +-----------------------------+ data_lba    (cluster 2 onward)
 *
 * Important constants are read from the BPB at mount time; nothing is hard-
 * coded except the default format geometry (used only when there's no FAT32
 * signature present at LBA 64).
 *
 * Directory layout:
 *   - 32-byte short directory entries (DIR entries).
 *   - LFN entries are 32-byte chunks with attr=0x0F, chained backwards
 *     ("Microsoft Extensible Firmware Initiative FAT32 File System
 *     Specification" §6).  We decode UCS-2 → 7-bit ASCII for our shell.
 *   - "." and ".." are present in every non-root directory.
 *
 * Path resolution:
 *   - Caller-supplied paths are slash-separated, relative to the mount root.
 *     Empty string ("") == root directory.
 *   - Components longer than 8.3 are matched against LFN; if missing, they
 *     fall back to the 8.3 short name (case-insensitive).
 */

#include <stdint.h>
#include "kernel/fs/fat32.h"
#include "drivers/ahci/ahci.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/klog.h"
#include "kernel/lib/string.h"
#include "kernel/mm/kheap.h"

/* ------- format defaults (only used when LBA 64 has no FAT32 signature) ------- */
#define FAT_DEFAULT_BASE_LBA      64u
#define FAT_DEFAULT_TOTAL_SECT    8192u   /* 4 MiB superfloppy */
#define FAT_DEFAULT_RESERVED      32u
#define FAT_DEFAULT_NUM_FATS      1u
#define FAT_DEFAULT_FAT_SECTORS   64u
#define FAT_DEFAULT_SPC           1u
#define FAT_ROOT_CLUSTER          2u
#define FAT_EOC                   0x0FFFFFFFu
#define FAT_BAD                   0x0FFFFFF7u
#define FAT_FREE                  0x00000000u
#define FAT_ATTR_RO     0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYS    0x04
#define FAT_ATTR_VOL    0x08
#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_ARCH   0x20
#define FAT_ATTR_LFN    0x0F

/* Caps / cache sizes (per-mount). */
#define FAT_MAX_OPEN_VNODES   128
#define FAT_MAX_NAME          128
#define FAT_DIR_ENT_SIZE      32

/* ---- Mount state (single mount instance for now) ---- */
struct fat32_mount {
    int       ahci_port;
    uint32_t  base_lba;         /* volume start LBA */
    uint16_t  bytes_per_sect;   /* always 512 in our usage */
    uint8_t   sect_per_clus;    /* 1, 2, 4, … */
    uint16_t  reserved;
    uint8_t   num_fats;
    uint32_t  fat_sectors;
    uint32_t  total_sectors;
    uint32_t  root_cluster;
    uint16_t  fsinfo_sect;      /* relative to base_lba */
    uint32_t  fat_lba;          /* absolute LBA of FAT[0] */
    uint32_t  data_lba;         /* absolute LBA of cluster 2 */
    uint32_t  cluster_count;    /* number of data clusters */
    uint32_t  bytes_per_clus;
    /* FSInfo cache (kept in sync after every alloc/free) */
    uint32_t  fsi_free_count;   /* 0xFFFFFFFF = unknown */
    uint32_t  fsi_next_free;
    int       mounted;
};

static struct fat32_mount fs;

/* Reusable sector-sized scratch buffers — guarded by single-threaded FS. */
static uint8_t  scratch[512];
/* Cluster-sized scratch for sequential R/W (allocated lazily). */
static uint8_t *cluster_buf = NULL;

/* Open-vnode cache so multiple lookups of the same path return the same
 * vnode pointer (so file-handle position state stays consistent). */
struct fat_vinfo {
    int       in_use;
    char      path[256];        /* full path inside the mount, slash-separated */
    uint32_t  first_cluster;    /* 0 = empty file */
    uint32_t  size;             /* in bytes (regular file); 0 for dirs */
    uint32_t  parent_cluster;   /* cluster of parent dir (root=root_cluster) */
    uint32_t  dirent_offset;    /* byte offset within parent dir cluster chain
                                   pointing at this entry's *short* dir entry */
    uint8_t   is_dir;
    struct vnode vnode;
};
static struct fat_vinfo vinfo_pool[FAT_MAX_OPEN_VNODES];

/* ---- Tiny endian helpers ---- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

/* ---- Block I/O ---- */
static int read_sect_abs(uint32_t lba, void *buf) {
    return ahci_read((uint32_t)fs.ahci_port, lba, 1, buf);
}
static int write_sect_abs(uint32_t lba, const void *buf) {
    return ahci_write((uint32_t)fs.ahci_port, lba, 1, buf);
}
static uint32_t cluster_to_lba(uint32_t cl) {
    return fs.data_lba + (cl - 2u) * fs.sect_per_clus;
}
static int read_cluster(uint32_t cl, void *buf) {
    return ahci_read((uint32_t)fs.ahci_port, cluster_to_lba(cl), fs.sect_per_clus, buf);
}
static int write_cluster(uint32_t cl, const void *buf) {
    return ahci_write((uint32_t)fs.ahci_port, cluster_to_lba(cl), fs.sect_per_clus, buf);
}

/* ---- FAT access ---- */
static uint32_t fat_get(uint32_t cl) {
    if (cl < 2 || cl >= fs.cluster_count + 2) return FAT_EOC;
    uint32_t off  = cl * 4u;
    uint32_t lba  = fs.fat_lba + off / 512u;
    uint32_t soff = off % 512u;
    if (read_sect_abs(lba, scratch) != 0) return FAT_EOC;
    return rd32(scratch + soff) & 0x0FFFFFFF;
}

static int fat_set_all(uint32_t cl, uint32_t val) {
    if (cl < 2 || cl >= fs.cluster_count + 2) return -1;
    uint32_t off  = cl * 4u;
    uint32_t soff = off % 512u;
    /* Write to every FAT mirror. */
    for (uint8_t f = 0; f < fs.num_fats; f++) {
        uint32_t lba = fs.fat_lba + (uint32_t)f * fs.fat_sectors + off / 512u;
        if (read_sect_abs(lba, scratch) != 0) return -1;
        uint32_t prev = rd32(scratch + soff);
        wr32(scratch + soff, (prev & 0xF0000000u) | (val & 0x0FFFFFFFu));
        if (write_sect_abs(lba, scratch) != 0) return -1;
    }
    return 0;
}

/* Mark a cluster zeroed at the data location. */
static int zero_cluster(uint32_t cl) {
    memset(cluster_buf, 0, fs.bytes_per_clus);
    return write_cluster(cl, cluster_buf);
}

/* ---- FSInfo ---- */
static int fsinfo_load(void) {
    if (fs.fsinfo_sect == 0) return 0;
    if (read_sect_abs(fs.base_lba + fs.fsinfo_sect, scratch) != 0) return -1;
    /* Lead, mid, trail signatures. */
    if (rd32(scratch + 0)   != 0x41615252u ||
        rd32(scratch + 484) != 0x61417272u ||
        rd32(scratch + 508) != 0xAA550000u) {
        fs.fsi_free_count = 0xFFFFFFFF;
        fs.fsi_next_free  = 3;
        return 0;
    }
    fs.fsi_free_count = rd32(scratch + 488);
    fs.fsi_next_free  = rd32(scratch + 492);
    if (fs.fsi_next_free < 2 || fs.fsi_next_free >= fs.cluster_count + 2)
        fs.fsi_next_free = 3;
    return 0;
}

static int fsinfo_save(void) {
    if (fs.fsinfo_sect == 0) return 0;
    if (read_sect_abs(fs.base_lba + fs.fsinfo_sect, scratch) != 0) return -1;
    wr32(scratch + 488, fs.fsi_free_count);
    wr32(scratch + 492, fs.fsi_next_free);
    return write_sect_abs(fs.base_lba + fs.fsinfo_sect, scratch);
}

/* ---- Cluster alloc/free ---- */
static uint32_t alloc_cluster(uint32_t link_after) {
    uint32_t start = (fs.fsi_next_free >= 2) ? fs.fsi_next_free : 3u;
    uint32_t end   = fs.cluster_count + 2u;
    /* Scan start..end then 2..start. */
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t lo = pass == 0 ? start : 2u;
        uint32_t hi = pass == 0 ? end   : start;
        for (uint32_t cl = lo; cl < hi; cl++) {
            if (fat_get(cl) == FAT_FREE) {
                if (fat_set_all(cl, FAT_EOC) != 0) return 0;
                if (link_after) {
                    if (fat_set_all(link_after, cl) != 0) return 0;
                }
                if (zero_cluster(cl) != 0) return 0;
                fs.fsi_next_free = cl + 1;
                if (fs.fsi_free_count != 0xFFFFFFFF && fs.fsi_free_count > 0)
                    fs.fsi_free_count--;
                fsinfo_save();
                return cl;
            }
        }
    }
    return 0;
}

static void free_chain(uint32_t first) {
    uint32_t cl = first;
    while (cl >= 2 && cl < FAT_BAD) {
        uint32_t next = fat_get(cl);
        fat_set_all(cl, FAT_FREE);
        if (fs.fsi_free_count != 0xFFFFFFFF) fs.fsi_free_count++;
        cl = next;
    }
    fsinfo_save();
}

/* Resolve nth cluster (0-based) in a chain.  Returns 0 if past end. */
static uint32_t chain_at(uint32_t first, uint32_t idx) {
    uint32_t cl = first;
    while (idx-- && cl >= 2 && cl < FAT_BAD) cl = fat_get(cl);
    if (cl < 2 || cl >= FAT_BAD) return 0;
    return cl;
}

/* Extend a chain so it has at least `clusters_needed` clusters.  If first is 0,
 * allocates a new chain and returns its head; on error returns 0. */
static uint32_t ensure_chain(uint32_t first, uint32_t clusters_needed) {
    if (clusters_needed == 0) return first;
    if (first == 0) {
        first = alloc_cluster(0);
        if (!first) return 0;
    }
    uint32_t cl = first;
    uint32_t have = 1;
    while (have < clusters_needed) {
        uint32_t next = fat_get(cl);
        if (next >= FAT_BAD || next == FAT_FREE) {
            uint32_t n = alloc_cluster(cl);
            if (!n) return 0;
            cl = n;
        } else {
            cl = next;
        }
        have++;
    }
    return first;
}

/* Truncate a chain to `keep` clusters; frees everything past it. */
static int truncate_chain(uint32_t first, uint32_t keep) {
    if (first == 0) return 0;
    if (keep == 0) { free_chain(first); return 0; }
    uint32_t cl = first;
    uint32_t i = 1;
    while (i < keep && cl >= 2 && cl < FAT_BAD) {
        uint32_t nx = fat_get(cl);
        if (nx >= FAT_BAD) return 0; /* already short enough */
        cl = nx; i++;
    }
    uint32_t tail = fat_get(cl);
    fat_set_all(cl, FAT_EOC);
    if (tail >= 2 && tail < FAT_BAD) free_chain(tail);
    return 0;
}

/* ---- Directory I/O via cluster chains ---- */

/* Read a single 32-byte directory entry at byte-offset `off` within the
 * cluster-chain rooted at `dir_cluster`.  Returns 0 on success. */
static int dir_read_entry(uint32_t dir_cluster, uint32_t off, uint8_t out[32]) {
    uint32_t cl_idx  = off / fs.bytes_per_clus;
    uint32_t in_off  = off % fs.bytes_per_clus;
    uint32_t cl = chain_at(dir_cluster, cl_idx);
    if (!cl) return -1;
    uint32_t sect_in_clus = in_off / 512u;
    uint32_t soff = in_off % 512u;
    if (read_sect_abs(cluster_to_lba(cl) + sect_in_clus, scratch) != 0) return -1;
    memcpy(out, scratch + soff, 32);
    return 0;
}

/* Write a 32-byte dir entry at `off`. */
static int read_entry_times(uint32_t dir_cluster, uint32_t off, uint64_t *ctime, uint64_t *mtime, uint64_t *atime);
static int dir_write_entry(uint32_t dir_cluster, uint32_t off, const uint8_t in[32]) {
    uint32_t cl_idx = off / fs.bytes_per_clus;
    uint32_t in_off = off % fs.bytes_per_clus;
    uint32_t cl = chain_at(dir_cluster, cl_idx);
    if (!cl) {
        /* Extend the chain. */
        if (ensure_chain(dir_cluster, cl_idx + 1) == 0) return -1;
        cl = chain_at(dir_cluster, cl_idx);
        if (!cl) return -1;
    }
    uint32_t sect_in_clus = in_off / 512u;
    uint32_t soff = in_off % 512u;
    uint32_t lba  = cluster_to_lba(cl) + sect_in_clus;
    if (read_sect_abs(lba, scratch) != 0) return -1;
    memcpy(scratch + soff, in, 32);
    return write_sect_abs(lba, scratch);
}

/* Walk a directory's cluster chain length in bytes (full clusters allocated). */
static uint32_t dir_chain_bytes(uint32_t first) {
    uint32_t cl = first; uint32_t n = 0;
    while (cl >= 2 && cl < FAT_BAD) { n++; cl = fat_get(cl); }
    return n * fs.bytes_per_clus;
}

/* ---- 8.3 name handling ---- */
static char upcase(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Compute the LFN checksum (per Microsoft FAT spec). */
static uint8_t lfn_checksum(const uint8_t name83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name83[i]);
    return sum;
}

/* Convert a 0-terminated name to space-padded 8.3.  Returns 0 on success.
 * Falls back to a "~N" alias if the name is too long or has illegal chars. */
static int to_name83(const char *name, uint8_t out[11], int alias_idx) {
    memset(out, ' ', 11);
    if (!name || !*name) return -1;

    /* Split at last dot. */
    int dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    int base_end = (dot >= 0) ? dot : (int)strlen(name);
    int ext_start = (dot >= 0) ? dot + 1 : (int)strlen(name);

    int needs_alias = (base_end > 8) || ((int)strlen(name + ext_start) > 3);
    /* Check char-set: only allow A-Z 0-9 _ - in short name. */
    for (int i = 0; i < base_end; i++) {
        char c = upcase(name[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')) {
            needs_alias = 1;
            break;
        }
    }

    if (!needs_alias && alias_idx == 0) {
        int o = 0;
        for (int i = 0; i < base_end && o < 8; i++) {
            char c = upcase(name[i]);
            if (c == ' ') continue;
            out[o++] = (uint8_t)c;
        }
        o = 8;
        for (int i = ext_start; name[i] && o < 11; i++) {
            out[o++] = (uint8_t)upcase(name[i]);
        }
        return 0;
    }

    /* Generate "BASE~N.EXT" alias.  Take up to 6 sane chars from base, append
     * "~<digit>".  alias_idx 1..9 produce ~1..~9. */
    if (alias_idx < 1) alias_idx = 1;
    if (alias_idx > 9) alias_idx = 9;
    int o = 0;
    for (int i = 0; i < base_end && o < 6; i++) {
        char c = upcase(name[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            c = '_';
        out[o++] = (uint8_t)c;
    }
    out[o++] = '~';
    out[o++] = (uint8_t)('0' + alias_idx);
    o = 8;
    for (int i = ext_start; name[i] && o < 11; i++) {
        char c = upcase(name[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            c = '_';
        out[o++] = (uint8_t)c;
    }
    return 0;
}

/* Convert 11-byte name83 to a "NAME.EXT" displayable string. */
static void name83_to_display(const uint8_t n[11], char out[13]) {
    int o = 0;
    for (int i = 0; i < 8 && n[i] != ' '; i++) out[o++] = (char)n[i];
    int has_ext = (n[8] != ' ' || n[9] != ' ' || n[10] != ' ');
    if (has_ext) {
        out[o++] = '.';
        for (int i = 8; i < 11 && n[i] != ' '; i++) out[o++] = (char)n[i];
    }
    out[o] = 0;
}

/* ---- LFN encode/decode ---- */

/* Decode an LFN entry chain (entries collected in `lfn_buf` bottom-up by
 * sequence number) into a 7-bit ASCII string. */
struct lfn_collector {
    uint8_t  buf[20][26];  /* up to 20 LFN entries × 26 name bytes (13 UCS-2) */
    uint8_t  seq_max;
    uint8_t  checksum;
    int      have_lfn;
};

static void lfn_clear(struct lfn_collector *c) {
    c->seq_max = 0; c->checksum = 0; c->have_lfn = 0;
}

static void lfn_decode(struct lfn_collector *c, char *out, int outsz) {
    int o = 0;
    if (!c->have_lfn || c->seq_max == 0) { out[0] = 0; return; }
    /* pending_lfn[0] holds the entry that appeared FIRST on disk = highest seq
     * = leftmost 13 chars of the name.  We packed each entry's 26 name bytes
     * contiguously in c->buf[i] (no header bytes), as 13 little-endian UCS-2
     * units sitting at offsets 0,2,4,...,24. */
    for (int i = 0; i < c->seq_max; i++) {
        /* Highest-seq entry on disk holds the LEFTMOST 13 chars.  In
         * pending_lfn order they're already disk order = decreasing seq, but
         * that's NOT what we want here — pending_lfn[0] is the entry with
         * "seq | 0x40" marker (== highest seq), which contains the LAST
         * group of <=13 characters (not the first).  So we walk in REVERSE. */
        const uint8_t *e = c->buf[c->seq_max - 1 - i];
        for (int k = 0; k < 13; k++) {
            uint16_t u = (uint16_t)e[k*2] | ((uint16_t)e[k*2+1] << 8);
            if (u == 0x0000 || u == 0xFFFF) { goto done; }
            if (o < outsz - 1) out[o++] = (char)(u < 0x80 ? u : '?');
        }
    }
done:
    out[o] = 0;
}

/* Compute number of LFN entries needed for a name of length N (UTF-16). */
static int lfn_entries_for(const char *name) {
    int n = (int)strlen(name);
    return (n + 12) / 13;
}

/* Build LFN entries (write order is reversed: top entry first) for the given
 * name.  Caller provides `entries[count][32]` and the matching checksum.
 * Sets seq numbers with high bit on the last (numerically highest) entry. */
static void lfn_build(const char *name, uint8_t checksum, uint8_t *out, int count) {
    int nlen = (int)strlen(name);
    for (int i = 0; i < count; i++) {
        uint8_t *e = out + i * 32;
        memset(e, 0xFF, 32);
        uint8_t seq = (uint8_t)(count - i);   /* top-down: highest seq first */
        if (i == 0) seq |= 0x40;              /* "last" marker */
        e[0]  = seq;
        e[11] = FAT_ATTR_LFN;
        e[12] = 0;
        e[13] = checksum;
        e[26] = 0; e[27] = 0;
        int base = (count - i - 1) * 13;      /* characters this entry holds */
        /* layout slots */
        int slot_offsets[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
        for (int k = 0; k < 13; k++) {
            int idx = base + k;
            uint16_t u;
            if (idx < nlen)             u = (uint16_t)(uint8_t)name[idx];
            else if (idx == nlen)       u = 0x0000;
            else                        u = 0xFFFF;
            e[slot_offsets[k]]   = (uint8_t)(u & 0xFF);
            e[slot_offsets[k]+1] = (uint8_t)(u >> 8);
        }
    }
}

/* ---- Directory enumeration ---- */

struct fat_dir_iter {
    uint32_t  dir_cluster;
    uint32_t  off;              /* byte offset within chain */
    /* Output of next(): */
    char      lfn[FAT_MAX_NAME];
    char      sfn[13];
    uint8_t   name83[11];
    uint8_t   attr;
    uint32_t  first_cluster;
    uint32_t  size;
    uint32_t  entry_off;        /* offset of the SHORT entry (for updates) */
    int       lfn_span;         /* number of LFN slots preceding short entry */
};

static int dir_iter_init(struct fat_dir_iter *it, uint32_t dir_cluster) {
    memset(it, 0, sizeof(*it));
    it->dir_cluster = dir_cluster;
    it->off = 0;
    return 0;
}

/* Step to next visible entry.  Returns 1 if got one, 0 = end, -1 = io err. */
static int dir_iter_next(struct fat_dir_iter *it) {
    struct lfn_collector lfn;
    lfn_clear(&lfn);
    uint8_t  pending_lfn[20][26];
    int      pending_count = 0;
    uint32_t lfn_start_off = 0;

    while (1) {
        uint8_t e[32];
        if (dir_read_entry(it->dir_cluster, it->off, e) != 0) return 0;

        if (e[0] == 0x00) return 0;              /* end of dir */
        if (e[0] == 0xE5) {                       /* deleted */
            it->off += 32;
            lfn_clear(&lfn); pending_count = 0;
            continue;
        }
        if ((e[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            /* Collect LFN entry; the "last" entry is encountered first (highest
             * seq with bit 0x40).  We push entries into pending_lfn in order
             * of decreasing seq. */
            if (pending_count == 0) lfn_start_off = it->off;
            if (pending_count < 20) {
                /* extract 26 name bytes into pending_lfn[pending_count]. */
                uint8_t *dst = pending_lfn[pending_count];
                dst[0]=e[1]; dst[1]=e[2]; dst[2]=e[3]; dst[3]=e[4];
                dst[4]=e[5]; dst[5]=e[6]; dst[6]=e[7]; dst[7]=e[8];
                dst[8]=e[9]; dst[9]=e[10];
                dst[10]=e[14]; dst[11]=e[15]; dst[12]=e[16]; dst[13]=e[17];
                dst[14]=e[18]; dst[15]=e[19]; dst[16]=e[20]; dst[17]=e[21];
                dst[18]=e[22]; dst[19]=e[23]; dst[20]=e[24]; dst[21]=e[25];
                dst[22]=e[28]; dst[23]=e[29]; dst[24]=e[30]; dst[25]=e[31];
                lfn.checksum = e[13];
                pending_count++;
            }
            it->off += 32;
            continue;
        }
        /* It's a short entry.  Skip volume label entries. */
        if (e[11] & FAT_ATTR_VOL) {
            it->off += 32;
            lfn_clear(&lfn); pending_count = 0;
            continue;
        }
        /* Got a visible directory entry. */
        memcpy(it->name83, e, 11);
        name83_to_display(it->name83, it->sfn);
        it->attr          = e[11];
        it->first_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        it->size          = rd32(e + 28);
        it->entry_off     = it->off;
        it->lfn_span      = pending_count;

        /* Decode LFN if checksum matches. */
        if (pending_count > 0 && lfn.checksum == lfn_checksum(it->name83)) {
            /* Entries in pending_lfn are in disk order: top of LFN run first.
             * That happens to also be order of decreasing seq, which is what
             * lfn_decode expects (highest seq → leftmost characters). */
            lfn.seq_max = (uint8_t)pending_count;
            lfn.have_lfn = 1;
            for (int i = 0; i < pending_count; i++) {
                memcpy(lfn.buf[i], pending_lfn[i], 26);
            }
            lfn_decode(&lfn, it->lfn, sizeof(it->lfn));
            it->entry_off = it->off;
            /* lfn_start_off points to the first LFN entry; entry_off points
             * at the short one. */
            (void)lfn_start_off;
        } else {
            it->lfn[0] = 0;
        }

        it->off += 32;
        return 1;
    }
}

/* Best display name: LFN if present, else SFN. */
static const char *iter_name(struct fat_dir_iter *it) {
    return it->lfn[0] ? it->lfn : it->sfn;
}

/* Find a directory entry by name (case-insensitive against both LFN and SFN).
 * On success, fills *out and returns 1.  Returns 0 if not found, -1 on io. */
static int dir_find(uint32_t dir_cluster, const char *name, struct fat_dir_iter *out) {
    struct fat_dir_iter it;
    dir_iter_init(&it, dir_cluster);
    int rc;
    while ((rc = dir_iter_next(&it)) == 1) {
        const char *cmp = iter_name(&it);
        int eq = 1;
        for (int i = 0; ; i++) {
            char a = upcase(cmp[i]);
            char b = upcase(name[i]);
            if (a != b) { eq = 0; break; }
            if (a == 0) break;
        }
        if (eq) { *out = it; return 1; }
    }
    return rc < 0 ? -1 : 0;
}

/* Path resolution from mount root: returns cluster of containing dir, and the
 * dir_iter info of the final component.  If the final component doesn't exist,
 * `*out_iter` is left zero-initialized and we return 0 with `*found = 0`. */
static int path_resolve(const char *path, uint32_t *out_parent,
                        struct fat_dir_iter *out_iter, int *found,
                        char *out_basename, int basename_sz) {
    *found = 0;
    memset(out_iter, 0, sizeof(*out_iter));
    if (!path) return -1;

    uint32_t dir = fs.root_cluster;
    char comp[FAT_MAX_NAME];

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) {
        /* root */
        *out_parent = fs.root_cluster;
        memset(out_iter, 0, sizeof(*out_iter));
        out_iter->dir_cluster   = fs.root_cluster;
        out_iter->first_cluster = fs.root_cluster;
        out_iter->attr          = FAT_ATTR_DIR;
        *found = 1;
        if (out_basename && basename_sz) out_basename[0] = 0;
        return 0;
    }

    while (*p) {
        /* Extract next component. */
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;

        if (*p == 0) {
            /* This is the final component. */
            *out_parent = dir;
            if (out_basename && basename_sz)
                strncpy(out_basename, comp, basename_sz - 1), out_basename[basename_sz-1]=0;
            struct fat_dir_iter it;
            int rc = dir_find(dir, comp, &it);
            if (rc < 0) return -1;
            if (rc == 0) { *found = 0; return 0; }
            *out_iter = it;
            *found = 1;
            return 0;
        }
        /* Intermediate component must be a directory. */
        struct fat_dir_iter it;
        int rc = dir_find(dir, comp, &it);
        if (rc <= 0) return -1;
        if (!(it.attr & FAT_ATTR_DIR)) return -1;
        dir = it.first_cluster ? it.first_cluster : fs.root_cluster;
    }
    return -1;
}

/* ---- Vnode pool ---- */

static struct fat_vinfo *vinfo_intern(const char *path,
                                      uint32_t parent_cluster,
                                      uint32_t dirent_off,
                                      uint32_t first_cluster,
                                      uint32_t size, int is_dir) {
    /* Existing? */
    for (int i = 0; i < FAT_MAX_OPEN_VNODES; i++) {
        if (vinfo_pool[i].in_use && strcmp(vinfo_pool[i].path, path) == 0) {
            vinfo_pool[i].first_cluster  = first_cluster;
            vinfo_pool[i].size           = size;
            vinfo_pool[i].parent_cluster = parent_cluster;
            vinfo_pool[i].dirent_offset  = dirent_off;
            vinfo_pool[i].is_dir         = (uint8_t)is_dir;
            vinfo_pool[i].vnode.size     = size;
            read_entry_times(parent_cluster, dirent_off, &vinfo_pool[i].vnode.ctime,
                             &vinfo_pool[i].vnode.mtime, &vinfo_pool[i].vnode.atime);
            return &vinfo_pool[i];
        }
    }
    for (int i = 0; i < FAT_MAX_OPEN_VNODES; i++) {
        if (!vinfo_pool[i].in_use) {
            struct fat_vinfo *v = &vinfo_pool[i];
            memset(v, 0, sizeof(*v));
            v->in_use         = 1;
            strncpy(v->path, path, sizeof(v->path) - 1);
            v->parent_cluster = parent_cluster;
            v->dirent_offset  = dirent_off;
            v->first_cluster  = first_cluster;
            v->size           = size;
            v->is_dir         = (uint8_t)is_dir;
            strncpy(v->vnode.name, path, VFS_PATH_MAX - 1);
            v->vnode.type     = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            v->vnode.mode     = is_dir ? 0755 : 0644;
            v->vnode.size     = size;
            v->vnode.ops      = &fat32_ops;
            v->vnode.fs_data  = v;
            v->vnode.inode_id = first_cluster;
            read_entry_times(parent_cluster, dirent_off, &v->vnode.ctime,
                             &v->vnode.mtime, &v->vnode.atime);
            return v;
        }
    }
    return NULL;
}

static void vinfo_evict_by_path(const char *path) {
    for (int i = 0; i < FAT_MAX_OPEN_VNODES; i++) {
        if (vinfo_pool[i].in_use && strcmp(vinfo_pool[i].path, path) == 0) {
            vinfo_pool[i].in_use = 0;
            return;
        }
    }
}

/* ---- Directory entry write helpers ---- */

/* Find a contiguous run of `count` free 32-byte slots in `dir_cluster` chain;
 * extends the chain if needed.  Returns starting byte offset. */
static int find_free_run(uint32_t dir_cluster, int count, uint32_t *out_off) {
    uint32_t off = 0;
    uint32_t run_start = 0;
    int run = 0;
    uint32_t scanned = 0;
    uint32_t bytes = dir_chain_bytes(dir_cluster);

    while (scanned < bytes) {
        uint8_t e[32];
        if (dir_read_entry(dir_cluster, off, e) != 0) return -1;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (run == 0) run_start = off;
            run++;
            if (run >= count) { *out_off = run_start; return 0; }
        } else {
            run = 0;
        }
        off += 32;
        scanned += 32;
    }
    /* Need to grow the dir chain by one cluster. */
    uint32_t old_clusters = bytes / fs.bytes_per_clus;
    if (ensure_chain(dir_cluster, old_clusters + 1) == 0) return -1;
    /* The new cluster is all-zero (zero_cluster was called) → all 32-byte
     * slots are free.  Use the start of the new cluster. */
    *out_off = old_clusters * fs.bytes_per_clus;
    return 0;
}

/* Stamp current time (best-effort: we don't have an RTC — use ticks as a
 * pseudo "incrementing time" so files differ).  Returns FAT date/time. */
static void fat_now(uint16_t *out_date, uint16_t *out_time) {
    /* Approximate "epoch" date = 2026-06-24, time bumps with each call. */
    static uint16_t bump = 0;
    bump++;
    /* date = ((year-1980)<<9) | (month<<5) | day */
    *out_date = ((2026u - 1980u) << 9) | (6u << 5) | 24u;
    /* time = (hour<<11) | (min<<5) | (sec/2) */
    uint16_t s = (bump * 13) % 30;
    uint16_t m = (bump / 60) % 60;
    uint16_t h = (bump / 3600) % 24;
    *out_time = (h << 11) | (m << 5) | s;
}


static int is_leap_year(int y) {
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

static uint64_t ymdhms_to_epoch(int y, int mo, int d, int h, int mi, int se) {
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1980 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) return 0;
    uint64_t days = 0;
    for (int yr = 1970; yr < y; yr++) days += 365 + is_leap_year(yr);
    for (int m = 1; m < mo; m++) days += mdays[m - 1] + ((m == 2 && is_leap_year(y)) ? 1 : 0);
    days += (uint64_t)(d - 1);
    return days * 86400ULL + (uint64_t)h * 3600ULL + (uint64_t)mi * 60ULL + (uint64_t)se;
}

static uint64_t fat_datetime_to_epoch(uint16_t date, uint16_t time) {
    if (date == 0) return 0;
    int y = 1980 + ((date >> 9) & 0x7F);
    int mo = (date >> 5) & 0x0F;
    int d = date & 0x1F;
    int h = (time >> 11) & 0x1F;
    int mi = (time >> 5) & 0x3F;
    int se = (time & 0x1F) * 2;
    return ymdhms_to_epoch(y, mo, d, h, mi, se);
}

static uint64_t fat_date_to_epoch(uint16_t date) {
    return fat_datetime_to_epoch(date, 0);
}

static int read_entry_times(uint32_t dir_cluster, uint32_t off, uint64_t *ctime, uint64_t *mtime, uint64_t *atime) {
    uint8_t e[32];
    if (dir_read_entry(dir_cluster, off, e) != 0) return -1;
    if (ctime) *ctime = fat_datetime_to_epoch(rd16(e + 16), rd16(e + 14));
    if (mtime) *mtime = fat_datetime_to_epoch(rd16(e + 24), rd16(e + 22));
    if (atime) *atime = fat_date_to_epoch(rd16(e + 18));
    return 0;
}

/* Write a short directory entry. */
static void build_sfn_entry(uint8_t e[32], const uint8_t name83[11], uint8_t attr,
                            uint32_t first_cluster, uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = attr;
    uint16_t d, t;
    fat_now(&d, &t);
    /* create time tenths */
    e[13] = 0;
    /* create time + date */
    wr16(e + 14, t);
    wr16(e + 16, d);
    /* access date */
    wr16(e + 18, d);
    /* first cluster high */
    wr16(e + 20, (uint16_t)(first_cluster >> 16));
    /* write time + date */
    wr16(e + 22, t);
    wr16(e + 24, d);
    /* first cluster low */
    wr16(e + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr32(e + 28, size);
}

/* Update size + first-cluster fields of an existing short entry. */
static int update_sfn_entry(uint32_t dir_cluster, uint32_t off,
                            uint32_t first_cluster, uint32_t size) {
    uint8_t e[32];
    if (dir_read_entry(dir_cluster, off, e) != 0) return -1;
    wr16(e + 20, (uint16_t)(first_cluster >> 16));
    wr16(e + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr32(e + 28, size);
    uint16_t d, t; fat_now(&d, &t);
    wr16(e + 22, t);  /* write time */
    wr16(e + 24, d);  /* write date */
    return dir_write_entry(dir_cluster, off, e);
}

/* Check if a candidate 8.3 name is already in use in the directory. */
static int sfn_taken(uint32_t dir_cluster, const uint8_t name83[11]) {
    struct fat_dir_iter it;
    dir_iter_init(&it, dir_cluster);
    while (dir_iter_next(&it) == 1) {
        if (memcmp(it.name83, name83, 11) == 0) return 1;
    }
    return 0;
}

/* Create a directory entry (LFN + SFN if needed) and return parent_dir
 * cluster + the offset of the SHORT entry.  Used by both create-file and
 * mkdir.  `attr` is FAT_ATTR_ARCH or FAT_ATTR_DIR. */
static int dir_create_entry(uint32_t dir_cluster, const char *name, uint8_t attr,
                            uint32_t first_cluster, uint32_t size,
                            uint32_t *out_dirent_off) {
    /* Pick a unique 8.3 alias. */
    uint8_t n83[11];
    int alias = 0;
    while (1) {
        if (to_name83(name, n83, alias) != 0) return -1;
        if (!sfn_taken(dir_cluster, n83)) break;
        alias++;
        if (alias > 9) return -1;
    }
    int need_lfn = (alias > 0);
    /* Always emit LFN if the printable name differs from the synthesised
     * short name (case, length, dot placement, …). */
    if (!need_lfn) {
        char disp[13]; name83_to_display(n83, disp);
        if (strcmp(disp, name) != 0) need_lfn = 1;
    }

    int lfn_count = need_lfn ? lfn_entries_for(name) : 0;
    int total = lfn_count + 1;

    uint32_t off;
    if (find_free_run(dir_cluster, total, &off) != 0) return -1;

    if (lfn_count > 0) {
        uint8_t entries[20 * 32];
        memset(entries, 0xFF, sizeof(entries));
        lfn_build(name, lfn_checksum(n83), entries, lfn_count);
        for (int i = 0; i < lfn_count; i++) {
            if (dir_write_entry(dir_cluster, off + (uint32_t)i * 32, entries + i * 32) != 0)
                return -1;
        }
    }
    uint8_t sfn_e[32];
    build_sfn_entry(sfn_e, n83, attr, first_cluster, size);
    if (dir_write_entry(dir_cluster, off + (uint32_t)lfn_count * 32, sfn_e) != 0) return -1;

    if (out_dirent_off) *out_dirent_off = off + (uint32_t)lfn_count * 32;
    return 0;
}

/* Mark a sequence of dir entries as deleted (0xE5 in byte 0). */
static int dir_mark_deleted(uint32_t dir_cluster, uint32_t sfn_off, int lfn_span) {
    for (int i = 0; i < lfn_span; i++) {
        uint32_t off = sfn_off - (uint32_t)(lfn_span - i) * 32;
        uint8_t e[32];
        if (dir_read_entry(dir_cluster, off, e) != 0) return -1;
        e[0] = 0xE5;
        if (dir_write_entry(dir_cluster, off, e) != 0) return -1;
    }
    uint8_t e[32];
    if (dir_read_entry(dir_cluster, sfn_off, e) != 0) return -1;
    e[0] = 0xE5;
    return dir_write_entry(dir_cluster, sfn_off, e);
}

/* ---- Format ---- */

static int format_default(void) {
    kprintf("[fat32] formatting default FAT32 volume at LBA %u (%u sectors)...\n",
            FAT_DEFAULT_BASE_LBA, FAT_DEFAULT_TOTAL_SECT);
    fs.base_lba       = FAT_DEFAULT_BASE_LBA;
    fs.bytes_per_sect = 512;
    fs.sect_per_clus  = FAT_DEFAULT_SPC;
    fs.reserved       = FAT_DEFAULT_RESERVED;
    fs.num_fats       = FAT_DEFAULT_NUM_FATS;
    fs.fat_sectors    = FAT_DEFAULT_FAT_SECTORS;
    fs.total_sectors  = FAT_DEFAULT_TOTAL_SECT;
    fs.root_cluster   = FAT_ROOT_CLUSTER;
    fs.fsinfo_sect    = 1;
    fs.fat_lba        = fs.base_lba + fs.reserved;
    fs.data_lba       = fs.fat_lba + (uint32_t)fs.num_fats * fs.fat_sectors;
    fs.cluster_count  = fs.total_sectors - (fs.data_lba - fs.base_lba);
    fs.bytes_per_clus = fs.sect_per_clus * 512u;
    if (!cluster_buf) cluster_buf = (uint8_t *)kmalloc(fs.bytes_per_clus);

    /* BPB */
    memset(scratch, 0, 512);
    scratch[0]=0xEB; scratch[1]=0x58; scratch[2]=0x90;
    memcpy(scratch + 3, "AURALITE", 8);
    wr16(scratch + 11, 512);
    scratch[13] = fs.sect_per_clus;
    wr16(scratch + 14, fs.reserved);
    scratch[16] = fs.num_fats;
    wr32(scratch + 32, fs.total_sectors);
    scratch[21] = 0xF8;
    wr16(scratch + 24, 63);
    wr16(scratch + 26, 255);
    wr32(scratch + 28, fs.base_lba);
    wr32(scratch + 36, fs.fat_sectors);
    wr32(scratch + 44, fs.root_cluster);
    wr16(scratch + 48, 1);
    wr16(scratch + 50, 6);
    scratch[64] = 0x80;
    scratch[66] = 0x29;
    wr32(scratch + 67, 0xA2026022u);
    memcpy(scratch + 71, "AURALITE   ", 11);
    memcpy(scratch + 82, "FAT32   ", 8);
    scratch[510] = 0x55; scratch[511] = 0xAA;
    if (write_sect_abs(fs.base_lba, scratch) != 0) return -1;
    /* Backup BPB. */
    write_sect_abs(fs.base_lba + 6, scratch);

    /* FSInfo */
    memset(scratch, 0, 512);
    wr32(scratch + 0,   0x41615252);
    wr32(scratch + 484, 0x61417272);
    wr32(scratch + 488, fs.cluster_count - 1);  /* root takes 1 */
    wr32(scratch + 492, 3);
    wr32(scratch + 508, 0xAA550000);
    write_sect_abs(fs.base_lba + 1, scratch);

    /* Zero FATs. */
    memset(scratch, 0, 512);
    for (uint8_t f = 0; f < fs.num_fats; f++) {
        for (uint32_t i = 0; i < fs.fat_sectors; i++) {
            write_sect_abs(fs.fat_lba + (uint32_t)f * fs.fat_sectors + i, scratch);
        }
    }
    /* Reserved FAT entries [0]=0x0FFFFFF8 [1]=EOC, [2]=EOC (root). */
    for (uint8_t f = 0; f < fs.num_fats; f++) {
        memset(scratch, 0, 512);
        wr32(scratch + 0, 0x0FFFFFF8);
        wr32(scratch + 4, FAT_EOC);
        wr32(scratch + 8, FAT_EOC);
        write_sect_abs(fs.fat_lba + (uint32_t)f * fs.fat_sectors, scratch);
    }
    /* Zero root cluster. */
    memset(cluster_buf, 0, fs.bytes_per_clus);
    write_cluster(fs.root_cluster, cluster_buf);
    return 0;
}

/* Parse the BPB at LBA 64 (or format on absence/mismatch). */
static int parse_or_format(void) {
    fs.base_lba = FAT_DEFAULT_BASE_LBA;
    if (read_sect_abs(fs.base_lba, scratch) != 0) return -1;
    int looks_fat = (scratch[510]==0x55 && scratch[511]==0xAA &&
                     memcmp(scratch + 82, "FAT32", 5) == 0);
    if (!looks_fat) {
        if (format_default() != 0) return -1;
        if (read_sect_abs(fs.base_lba, scratch) != 0) return -1;
    }
    fs.bytes_per_sect = rd16(scratch + 11);
    fs.sect_per_clus  = scratch[13];
    fs.reserved       = rd16(scratch + 14);
    fs.num_fats       = scratch[16];
    fs.total_sectors  = rd32(scratch + 32);
    fs.fat_sectors    = rd32(scratch + 36);
    fs.root_cluster   = rd32(scratch + 44);
    fs.fsinfo_sect    = rd16(scratch + 48);
    if (fs.bytes_per_sect != 512) return -1;
    if (fs.sect_per_clus < 1 || fs.num_fats < 1) return -1;
    fs.fat_lba  = fs.base_lba + fs.reserved;
    fs.data_lba = fs.fat_lba + (uint32_t)fs.num_fats * fs.fat_sectors;
    fs.cluster_count = fs.total_sectors - (fs.data_lba - fs.base_lba);
    fs.bytes_per_clus = fs.sect_per_clus * 512u;
    if (!cluster_buf) cluster_buf = (uint8_t *)kmalloc(fs.bytes_per_clus);
    fsinfo_load();
    return 0;
}

/* ---- File I/O ---- */

static int64_t fat32_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (pos >= v->size) return 0;
    if (pos + count > v->size) count = v->size - pos;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t cl_idx = (uint32_t)((pos + done) / fs.bytes_per_clus);
        uint32_t off    = (uint32_t)((pos + done) % fs.bytes_per_clus);
        uint32_t cl = chain_at(v->first_cluster, cl_idx);
        if (!cl) break;
        if (read_cluster(cl, cluster_buf) != 0) return -1;
        uint64_t chunk = fs.bytes_per_clus - off;
        if (chunk > count - done) chunk = count - done;
        memcpy(out + done, cluster_buf + off, chunk);
        done += chunk;
    }
    if (done > 0) {
        uint8_t e[32];
        if (dir_read_entry(v->parent_cluster, v->dirent_offset, e) == 0) {
            uint16_t d, t;
            fat_now(&d, &t);
            wr16(e + 18, d);
            dir_write_entry(v->parent_cluster, v->dirent_offset, e);
            vn->atime = fat_date_to_epoch(d);
        }
    }
    return (int64_t)done;
}

static int64_t fat32_write(struct vnode *vn, uint64_t pos, const void *buf, uint64_t count) {
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (count == 0) return 0;
    uint64_t end = pos + count;
    if (end < pos) return -1;
    uint32_t clusters = (uint32_t)((end + fs.bytes_per_clus - 1) / fs.bytes_per_clus);
    uint32_t head = ensure_chain(v->first_cluster, clusters);
    if (!head) return -1;
    v->first_cluster = head;
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < count) {
        uint32_t cl_idx = (uint32_t)((pos + done) / fs.bytes_per_clus);
        uint32_t off    = (uint32_t)((pos + done) % fs.bytes_per_clus);
        uint32_t cl = chain_at(v->first_cluster, cl_idx);
        if (read_cluster(cl, cluster_buf) != 0) return -1;
        uint64_t chunk = fs.bytes_per_clus - off;
        if (chunk > count - done) chunk = count - done;
        memcpy(cluster_buf + off, in + done, chunk);
        if (write_cluster(cl, cluster_buf) != 0) return -1;
        done += chunk;
    }
    if (end > v->size) v->size = (uint32_t)end;
    v->vnode.size = v->size;
    update_sfn_entry(v->parent_cluster, v->dirent_offset, v->first_cluster, v->size);
    read_entry_times(v->parent_cluster, v->dirent_offset, &vn->ctime, &vn->mtime, &vn->atime);
    return (int64_t)done;
}

/* ---- VFS surface ---- */

static struct vnode *fat32_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (!fs.mounted) return NULL;
    uint32_t parent;
    struct fat_dir_iter it;
    int found;
    char base[FAT_MAX_NAME] = {0};
    if (path_resolve(path, &parent, &it, &found, base, sizeof(base)) != 0) return NULL;
    if (!found) return NULL;
    int is_dir = (it.attr & FAT_ATTR_DIR) != 0;
    return &vinfo_intern(path,
                         parent,
                         it.entry_off,
                         it.first_cluster,
                         it.size,
                         is_dir)->vnode;
}

static struct vnode *fat32_create(void *fs_data, const char *path) {
    (void)fs_data;
    if (!fs.mounted) return NULL;
    uint32_t parent;
    struct fat_dir_iter it;
    int found;
    char base[FAT_MAX_NAME] = {0};
    if (path_resolve(path, &parent, &it, &found, base, sizeof(base)) != 0) return NULL;
    if (found) {
        if (it.attr & FAT_ATTR_DIR) return NULL;
        return &vinfo_intern(path, parent, it.entry_off, it.first_cluster,
                             it.size, 0)->vnode;
    }
    if (!base[0]) return NULL;
    uint32_t dirent_off;
    if (dir_create_entry(parent, base, FAT_ATTR_ARCH, 0, 0, &dirent_off) != 0)
        return NULL;
    return &vinfo_intern(path, parent, dirent_off, 0, 0, 0)->vnode;
}

static int fat32_mkdir_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!fs.mounted) return -1;
    uint32_t parent;
    struct fat_dir_iter it;
    int found;
    char base[FAT_MAX_NAME] = {0};
    if (path_resolve(path, &parent, &it, &found, base, sizeof(base)) != 0) return -1;
    if (found) return -1;
    if (!base[0]) return -1;
    /* Allocate the new directory's first cluster. */
    uint32_t cl = alloc_cluster(0);
    if (!cl) return -1;
    /* Populate "." and ".." entries. */
    memset(cluster_buf, 0, fs.bytes_per_clus);
    uint8_t dot[32], dotdot[32];
    uint8_t n_dot[11]    = ".          ";
    uint8_t n_dotdot[11] = "..         ";
    build_sfn_entry(dot,    n_dot,    FAT_ATTR_DIR, cl, 0);
    /* ".." points at parent.  Per spec, if parent is the root, store 0. */
    uint32_t parent_link = (parent == fs.root_cluster) ? 0 : parent;
    build_sfn_entry(dotdot, n_dotdot, FAT_ATTR_DIR, parent_link, 0);
    memcpy(cluster_buf + 0,  dot,    32);
    memcpy(cluster_buf + 32, dotdot, 32);
    if (write_cluster(cl, cluster_buf) != 0) { free_chain(cl); return -1; }
    uint32_t off;
    if (dir_create_entry(parent, base, FAT_ATTR_DIR, cl, 0, &off) != 0) {
        free_chain(cl);
        return -1;
    }
    return 0;
}

static int fat32_unlink_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!fs.mounted) return -1;
    uint32_t parent;
    struct fat_dir_iter it;
    int found;
    if (path_resolve(path, &parent, &it, &found, NULL, 0) != 0) return -1;
    if (!found) return -1;
    if (it.attr & FAT_ATTR_DIR) return -1;
    if (it.first_cluster) free_chain(it.first_cluster);
    if (dir_mark_deleted(parent, it.entry_off, it.lfn_span) != 0) return -1;
    vinfo_evict_by_path(path);
    return 0;
}

/* True if dir at cluster `cl` is empty (i.e., only "." and ".."). */
static int dir_is_empty(uint32_t cl) {
    struct fat_dir_iter it;
    dir_iter_init(&it, cl);
    int rc;
    while ((rc = dir_iter_next(&it)) == 1) {
        const char *n = iter_name(&it);
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        return 0;
    }
    return rc >= 0;
}

static int fat32_rmdir_op(void *fs_data, const char *path) {
    (void)fs_data;
    if (!fs.mounted) return -1;
    uint32_t parent;
    struct fat_dir_iter it;
    int found;
    if (path_resolve(path, &parent, &it, &found, NULL, 0) != 0) return -1;
    if (!found) return -1;
    if (!(it.attr & FAT_ATTR_DIR)) return -1;
    if (!it.first_cluster) return -1;
    if (!dir_is_empty(it.first_cluster)) return -1;
    free_chain(it.first_cluster);
    if (dir_mark_deleted(parent, it.entry_off, it.lfn_span) != 0) return -1;
    vinfo_evict_by_path(path);
    return 0;
}

static int fat32_rename_op(void *fs_data, const char *from, const char *to) {
    (void)fs_data;
    if (!fs.mounted) return -1;
    uint32_t pf, pt;
    struct fat_dir_iter itf, itt;
    int ff, ft;
    char bt[FAT_MAX_NAME] = {0};
    if (path_resolve(from, &pf, &itf, &ff, NULL, 0) != 0 || !ff) return -1;
    if (path_resolve(to,   &pt, &itt, &ft, bt, sizeof(bt)) != 0) return -1;
    if (ft) return -1; /* destination exists */
    if (!bt[0]) return -1;
    uint8_t attr = itf.attr;
    uint32_t fc = itf.first_cluster;
    uint32_t sz = itf.size;
    if (dir_mark_deleted(pf, itf.entry_off, itf.lfn_span) != 0) return -1;
    uint32_t off;
    if (dir_create_entry(pt, bt, attr, fc, sz, &off) != 0) return -1;
    vinfo_evict_by_path(from);
    return 0;
}

static int fat32_truncate_op(struct vnode *vn, uint64_t new_size) {
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    if (!v || v->is_dir) return -1;
    if (new_size > 0xFFFFFFFFu) return -1;
    uint32_t need = (uint32_t)((new_size + fs.bytes_per_clus - 1) / fs.bytes_per_clus);
    if (need == 0) {
        if (v->first_cluster) free_chain(v->first_cluster);
        v->first_cluster = 0;
    } else {
        if (v->first_cluster == 0) {
            v->first_cluster = ensure_chain(0, need);
            if (!v->first_cluster) return -1;
        } else {
            uint32_t head = ensure_chain(v->first_cluster, need);
            if (!head) return -1;
            v->first_cluster = head;
            truncate_chain(v->first_cluster, need);
        }
    }
    v->size = (uint32_t)new_size;
    v->vnode.size = new_size;
    update_sfn_entry(v->parent_cluster, v->dirent_offset, v->first_cluster, v->size);
    read_entry_times(v->parent_cluster, v->dirent_offset, &vn->ctime, &vn->mtime, &vn->atime);
    return 0;
}

static int fat32_stat_op(struct vnode *vn, struct vfs_stat *st) {
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    if (!v) return -1;
    st->type   = v->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode   = v->is_dir ? 0755 : 0644;
    st->uid    = vn->uid;
    st->gid    = vn->gid;
    st->size   = v->size;
    st->inode  = v->first_cluster;
    st->nlink  = 1;
    st->blocks = (v->size + fs.bytes_per_clus - 1) / fs.bytes_per_clus;
    read_entry_times(v->parent_cluster, v->dirent_offset, &st->ctime, &st->mtime, &st->atime);
    return 0;
}

static int fat32_readdir_op(struct vnode *vn, struct vfs_dirent *out, int max) {
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    uint32_t dir_cluster;
    if (!v || (!v->is_dir && vn->type != VFS_TYPE_DIR)) {
        /* Maybe this is a synthetic root vnode (vn for "/" with no fs_data). */
        if (vn->type == VFS_TYPE_DIR && !v) {
            dir_cluster = fs.root_cluster;
        } else {
            return -1;
        }
    } else {
        dir_cluster = v->first_cluster ? v->first_cluster : fs.root_cluster;
    }
    struct fat_dir_iter it;
    dir_iter_init(&it, dir_cluster);
    int n = 0;
    while (n < max && dir_iter_next(&it) == 1) {
        const char *nm = iter_name(&it);
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, nm, VFS_PATH_MAX - 1);
        out[n].type  = (it.attr & FAT_ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        out[n].size  = (it.attr & FAT_ATTR_DIR) ? 0 : it.size;
        out[n].inode = it.first_cluster;
        n++;
    }
    return n;
}

const struct vfs_ops fat32_ops = {
    .lookup   = fat32_lookup,
    .create   = fat32_create,
    .read     = fat32_read,
    .write    = fat32_write,
    .readdir  = fat32_readdir_op,
    .mkdir    = fat32_mkdir_op,
    .rmdir    = fat32_rmdir_op,
    .unlink   = fat32_unlink_op,
    .rename   = fat32_rename_op,
    .stat     = fat32_stat_op,
    .truncate = fat32_truncate_op,
};

/* ---- Public API ---- */

int fat32_append_log(const char *data, uint64_t len) {
    if (!fs.mounted || !data || len == 0) return -1;
    struct vnode *vn = fat32_lookup(NULL, "AURALOG.TXT");
    if (!vn) vn = fat32_create(NULL, "AURALOG.TXT");
    if (!vn) return -1;
    struct fat_vinfo *v = (struct fat_vinfo *)vn->fs_data;
    return fat32_write(vn, v->size, data, len) == (int64_t)len ? 0 : -1;
}

/* Legacy flat listing. */
void fat32_list(void) {
    if (!fs.mounted) return;
    struct fat_dir_iter it;
    dir_iter_init(&it, fs.root_cluster);
    while (dir_iter_next(&it) == 1) {
        const char *nm = iter_name(&it);
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (it.attr & FAT_ATTR_DIR)
            kprintf("  /fat/%s/\n", nm);
        else
            kprintf("  /fat/%s  (%u bytes)\n", nm, it.size);
    }
}

int fat32_init(void) {
    memset(&fs, 0, sizeof(fs));
    memset(vinfo_pool, 0, sizeof(vinfo_pool));
    fs.ahci_port = ahci_get_first_port();
    if (fs.ahci_port < 0) {
        kprintf("[fat32] no AHCI disk; FAT32 disabled\n");
        return -1;
    }
    if (parse_or_format() != 0) {
        kprintf("[fat32] failed to mount/format FAT32 volume\n");
        return -1;
    }
    fs.mounted = 1;
    kprintf("[fat32] mounted FAT32 at /fat (base LBA %u, spc=%u, clusters=%u, free=%u)\n",
            fs.base_lba, fs.sect_per_clus, fs.cluster_count,
            fs.fsi_free_count == 0xFFFFFFFF ? 0 : fs.fsi_free_count);
    klog_set_sink(fat32_append_log);
    klog_flush();
    kprintf("[fat32] persistent kernel log: /fat/AURALOG.TXT\n");
    return 0;
}

/* ---- Self-tests ---- */
void fat32_self_test(void) {
    if (!fs.mounted) return;
    kprintf("[fat32] self-test: file/dir/LFN/rename...\n");

    /* Cleanup leftovers from a previous boot so the test is idempotent. */
    fat32_unlink_op(NULL, "TEST.TXT");
    fat32_unlink_op(NULL, "RENAMED.TXT");
    fat32_unlink_op(NULL, "ALongFileName.txt");
    fat32_unlink_op(NULL, "SUBDIR/INNER.TXT");
    fat32_rmdir_op (NULL, "SUBDIR");

    /* 1. Create + write + read a regular file. */
    struct vnode *vn = fat32_create(NULL, "TEST.TXT");
    if (!vn) { kprintf("[fat32] FAIL: create TEST.TXT\n"); return; }
    const char *msg = "hello fat32 v2";
    if (fat32_write(vn, 0, msg, strlen(msg)) != (int64_t)strlen(msg)) {
        kprintf("[fat32] FAIL: write TEST.TXT\n"); return;
    }
    char buf[64] = {0};
    if (fat32_read(vn, 0, buf, sizeof(buf)-1) != (int64_t)strlen(msg) ||
        strcmp(buf, msg) != 0) {
        kprintf("[fat32] FAIL: readback TEST.TXT: '%s'\n", buf); return;
    }

    /* 2. Create a long-named file (LFN). */
    vn = fat32_create(NULL, "ALongFileName.txt");
    if (!vn) { kprintf("[fat32] FAIL: create LFN file\n"); return; }
    const char *m2 = "long names work";
    if (fat32_write(vn, 0, m2, strlen(m2)) != (int64_t)strlen(m2)) {
        kprintf("[fat32] FAIL: write LFN file\n"); return;
    }
    /* Lookup by LFN. */
    struct vnode *vn2 = fat32_lookup(NULL, "ALongFileName.txt");
    if (!vn2) { kprintf("[fat32] FAIL: lookup LFN file\n"); return; }
    memset(buf, 0, sizeof(buf));
    if (fat32_read(vn2, 0, buf, sizeof(buf)-1) != (int64_t)strlen(m2) ||
        strcmp(buf, m2) != 0) {
        kprintf("[fat32] FAIL: LFN readback '%s'\n", buf); return;
    }

    /* 3. mkdir + nested file. */
    if (fat32_mkdir_op(NULL, "SUBDIR") != 0) {
        kprintf("[fat32] FAIL: mkdir SUBDIR\n"); return;
    }
    vn = fat32_create(NULL, "SUBDIR/INNER.TXT");
    if (!vn) { kprintf("[fat32] FAIL: create SUBDIR/INNER.TXT\n"); return; }
    const char *m3 = "nested file content";
    if (fat32_write(vn, 0, m3, strlen(m3)) != (int64_t)strlen(m3)) {
        kprintf("[fat32] FAIL: write SUBDIR/INNER.TXT\n"); return;
    }
    memset(buf, 0, sizeof(buf));
    vn2 = fat32_lookup(NULL, "SUBDIR/INNER.TXT");
    if (!vn2 || fat32_read(vn2, 0, buf, sizeof(buf)-1) != (int64_t)strlen(m3) ||
        strcmp(buf, m3) != 0) {
        kprintf("[fat32] FAIL: nested readback\n"); return;
    }

    /* 4. Rename + unlink. */
    if (fat32_rename_op(NULL, "TEST.TXT", "RENAMED.TXT") != 0) {
        kprintf("[fat32] FAIL: rename\n"); return;
    }
    if (fat32_lookup(NULL, "TEST.TXT")) { kprintf("[fat32] FAIL: old name still resolvable\n"); return; }
    if (!fat32_lookup(NULL, "RENAMED.TXT")) { kprintf("[fat32] FAIL: new name unresolvable\n"); return; }
    if (fat32_unlink_op(NULL, "RENAMED.TXT") != 0) {
        kprintf("[fat32] FAIL: unlink\n"); return;
    }

    kprintf("[fat32] PASS: FAT32 read/write and log sink ready\n");
}
