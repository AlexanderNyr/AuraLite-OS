/* tools/selfhost/mkiso.c -- SELFHOST_PLAN.md SH7d: MBR + GPT + FAT32 ESP writer.
 *
 * SH7 replaces the host-only image tooling (Fact 5: the mformat/mcopy pair
 * plus the inline python3 that patches the FAT BPB) with one C program the
 * guest tcc can compile and `sh build.sh iso` can run in-guest.  This is
 * that twin for the boot image: it lays down the same raw hybrid disk the
 * host tools/mkisoimage_dual.sh produces, byte-for-byte in the parts the
 * firmware and the loaders actually read.
 *
 * The on-disk layout it writes (LBA = 512-byte sectors), identical to the
 * host script it replaces:
 *
 *   LBA 0          hybrid MBR (mbr_dual.bin, with its partition table
 *                  patched: disk signature, bootable 0x0C FAT32 slot and the
 *                  0xEE GPT-protective slot)
 *   LBA 1          primary GPT header ("EFI PART")
 *   LBA 2..33      primary GPT partition array (128 x 128 B)
 *   LBA 34..159    Stage 2 flat binary (spliced verbatim)
 *   LBA 160..255   (zero)
 *   LBA 256..      the ESP: a FAT32 volume we build ourselves --
 *                    BPB + FSInfo (+ backup at relative sector 6/7),
 *                    two FATs, root cluster 2 with EFI/ and EFI/BOOT/,
 *                    BOOTX64.EFI, KERNEL.ELF (twice), INITRD.TAR, KERNEL32
 *   LBA N-33..N-2  backup GPT partition array
 *   LBA N-1        backup GPT header
 *
 * Nothing is guessed.  The BPB fields, the FSInfo lead/struct signatures
 * (0x41615252 / 0x61417272), the reserved FAT entries ([0]=0x0FFFFFF8,
 * [1]=EOC), the 0xF8 media byte, the 8.3 directory entry shape and the
 * FAT32 >=65525 data-cluster floor are all taken from the Microsoft FAT32
 * spec and cross-checked against the tree's own reader (kernel/fs/fat32.c,
 * whose parse_or_format() reads exactly these offsets) and mtools.  The GPT
 * fields (revision 0x10000, 128 entries of 128 B, CRC32 of the entry array
 * and of the header with its own CRC field zeroed, ESP type GUID
 * C12A7328-F81F-11D2-BA4B-00A0C93EC93B) follow UEFI 2.10 s.5.3, same as the
 * host script.
 *
 * The python BPB patch disappears entirely: unlike mformat we always write
 * BPB_TotSec16 == 0 with the count only in BPB_TotSec32, which is what FAT32
 * mandates and OVMF's strict FatPkg driver validates.  The geometry the host
 * passed to mformat becomes the --esp-mb flag (>= 40 MiB enforced).
 *
 * Like mkinitrd.c (SH7b) this file is the real shipped tool and is also
 * #included by the host unit test with MKISO_NO_MAIN defined, so the writer
 * is pinned at dev speed without a VM: the host test builds an image from
 * synthetic files and asserts the MBR/GPT/FAT structures and that the file
 * bytes round-trip at the right paths.
 *
 * Usage:
 *   mkiso [--esp-mb N] [--stage2 f] --mbr f --kernel f --efi f \
 *         [--initrd f] [--kernel32 f] <output.iso>
 *   mkiso --selftest
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- layout constants (match tools/mkisoimage_dual.sh) ------------------ */
#define MK_SECT            512u
#define MK_ESP_START_LBA   256u   /* ESP partition starts here on the disk   */
#define MK_STAGE2_LBA      34u    /* Stage 2 spliced at LBA 34                */
#define MK_GPT_HDR_LBA     1u
#define MK_GPT_ARR_LBA     2u
#define MK_GPT_ENTRIES     128u
#define MK_GPT_ENT_SIZE    128u

/* FAT32 volume geometry we always emit. */
#define MK_FAT_RESERVED    32u    /* reserved sectors (BPB+FSInfo+backup)    */
#define MK_FAT_NUMFATS     2u     /* two FATs (robust; OVMF ignores the 2nd)  */
#define MK_FAT_SPC         1u     /* sectors per cluster (1 => 512 B)        */
#define MK_FAT_ROOTCLUS    2u
#define MK_FAT_EOC         0x0FFFFFFFu
#define MK_FAT_MIN_CLUSTER 65525u /* below this OVMF calls it FAT16          */
#define MK_ESP_MIN_MB      40u

#define MK_MAX_FILES       32

/* ---- a whole-file input, loaded lazily by the host and as needed ------- */
struct mk_file {
    const char *path;
    unsigned char *data;
    long size;
    int present;
};

struct mk_opts {
    const char *out;
    const char *mbr_path;
    const char *stage2_path;
    const char *kernel_path;
    const char *efi_path;
    const char *initrd_path;   /* may be NULL */
    const char *kernel32_path; /* may be NULL */
    unsigned esp_mb;           /* ESP size in MiB */
};

/* ---- CRC32 (the zlib polynomial GPT uses; identical to apkg_crc32) ------ */
static uint32_t mk_crc32(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint16_t mk_rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t mk_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void mk_wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static void mk_wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static void mk_wr64(unsigned char *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
}

/* Read a whole file into freshly malloc'd storage.  Returns 0/-1. */
static int mk_load(const char *path, struct mk_file *f) {
    FILE *fp;
    long sz;
    if (!path) { f->present = 0; return 0; }
    fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "mkiso: cannot open %s\n", path); return -1; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    sz = ftell(fp);
    if (sz < 0) { fclose(fp); return -1; }
    rewind(fp);
    f->data = (unsigned char *)malloc((size_t)(sz > 0 ? sz : 1));
    if (!f->data) { fclose(fp); return -1; }
    f->size = 0;
    if (sz > 0) {
        if (fread(f->data, 1, (size_t)sz, fp) != (size_t)sz) {
            free(f->data); f->data = NULL; fclose(fp); return -1;
        }
    }
    f->size = sz;
    f->path = path;
    f->present = 1;
    fclose(fp);
    return 0;
}

/* ---- FAT32 directory entry --------------------------------------------- */
/* Fill an 11-byte 8.3 field (8 name + 3 ext, space-padded, upper case). */
static void mk_83(unsigned char *dst, const char *name83) {
    int i;
    for (i = 0; i < 11; i++) dst[i] = ' ';
    for (i = 0; i < 11 && name83[i]; i++) {
        char c = name83[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        dst[i] = (unsigned char)c;
    }
}

/* Build a 32-byte short directory entry.
 *   name83 : 11 chars "NAME    EXT" (no dot), e.g. "KERNEL  ELF"
 *   attr   : FAT_ATTR_DIR (0x10) / FAT_ATTR_ARCH (0x20) / FAT_ATTR_VOL (0x08)
 *   cluster: first cluster (0 for the volume label)
 *   size   : file size in bytes (0 for directories / label)
 */
static void mk_dirent(unsigned char *e, const char *name83, uint8_t attr,
                      uint32_t cluster, uint32_t size) {
    memset(e, 0, 32);
    if (attr == 0x08) {
        /* Volume label: name in bytes 0..10, attr 0x08, no cluster/size. */
        memcpy(e, "AURALITE   ", 11);
        e[11] = 0x08;
        return;
    }
    mk_83(e, name83);
    e[11] = attr;
    /* Fixed, deterministic create/access/write timestamp (2026-01-01 00:00).
     * FAT date: ((year-1980)<<9)|(month<<5)|day ; time: (h<<11)|(m<<5)|(s/2). */
    {
        uint16_t dt = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
        uint16_t tm = 0;
        mk_wr16(e + 14, tm);   /* create time (h<<11|m<<5|s/2) */
        mk_wr16(e + 16, dt);   /* create date (y<<9|m<<5|d) */
        mk_wr16(e + 18, dt);   /* last access date */
        mk_wr16(e + 22, tm);   /* write time */
        mk_wr16(e + 24, dt);   /* write date */
    }
    mk_wr16(e + 20, (uint16_t)(cluster >> 16)); /* high first-cluster */
    mk_wr16(e + 26, (uint16_t)(cluster & 0xFFFF)); /* low first-cluster */
    mk_wr32(e + 28, size);
}

/* Write one sector at an absolute LBA into the image. */
static int mk_wr_sect(FILE *img, uint64_t lba, const void *buf) {
    if (fseek(img, (long)(lba * MK_SECT), SEEK_SET) != 0) return -1;
    if (fwrite(buf, 1, MK_SECT, img) != MK_SECT) return -1;
    return 0;
}

/* Fill the BPB (FAT32 boot sector) for a volume of `tot32` sectors. */
static void mk_make_bpb(unsigned char *bpb, uint32_t tot32, uint32_t fat_sz32) {
    memset(bpb, 0, MK_SECT);
    bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;   /* jmp rel8; nop */
    memcpy(bpb + 3, "AURALITE", 8);                 /* OEM name */
    mk_wr16(bpb + 11, 512);                         /* bytes/sector */
    bpb[13] = MK_FAT_SPC;                           /* sectors/cluster */
    mk_wr16(bpb + 14, MK_FAT_RESERVED);             /* reserved sectors */
    bpb[16] = MK_FAT_NUMFATS;                       /* number of FATs */
    mk_wr16(bpb + 17, 0);                           /* root entries (0 for FAT32) */
    mk_wr16(bpb + 19, 0);                           /* TotSec16 = 0 (FAT32!) */
    bpb[21] = 0xF8;                                 /* media: fixed disk */
    mk_wr16(bpb + 22, 0);                           /* FATsz16 (0 for FAT32) */
    mk_wr16(bpb + 24, 32);                          /* sectors/track */
    mk_wr16(bpb + 26, 32);                          /* heads */
    mk_wr32(bpb + 28, MK_ESP_START_LBA);            /* hidden sectors = ESP LBA */
    mk_wr32(bpb + 32, tot32);                       /* TotSec32 */
    mk_wr32(bpb + 36, fat_sz32);                    /* FATsize32 */
    mk_wr16(bpb + 40, 0);                           /* ext flags: mirror all */
    mk_wr16(bpb + 42, 0);                           /* FS version 0.0 */
    mk_wr32(bpb + 44, MK_FAT_ROOTCLUS);             /* root cluster = 2 */
    mk_wr16(bpb + 48, 1);                           /* FSInfo sector */
    mk_wr16(bpb + 50, 6);                           /* backup boot sector */
    bpb[64] = 0x80;                                 /* drive number */
    bpb[66] = 0x29;                                 /* ext boot signature */
    mk_wr32(bpb + 67, 0x12345678u);                 /* volume serial */
    memcpy(bpb + 71, "AURALITE   ", 11);            /* volume label */
    memcpy(bpb + 82, "FAT32   ", 8);                /* FS type string */
    bpb[510] = 0x55; bpb[511] = 0xAA;               /* boot signature */
}

static void mk_make_fsinfo(unsigned char *fi, uint32_t free_count,
                           uint32_t next_free) {
    memset(fi, 0, MK_SECT);
    mk_wr32(fi + 0, 0x41615252u);    /* lead signature */
    mk_wr32(fi + 484, 0x61417272u);  /* struct signature */
    mk_wr32(fi + 488, free_count);   /* free cluster count (0xFFFFFFFF=unknown) */
    mk_wr32(fi + 492, next_free);    /* next free cluster hint */
    mk_wr16(fi + 508, 0x55AA);       /* trailing 0x0000AA55 little-endian */
    fi[510] = 0x55; fi[511] = 0xAA;
}

/* Encode a GUID/UUID into GPT mixed-endian byte order (fields 1-3 little,
 * the rest network order). */
static void mk_guid(unsigned char *out,
                    uint32_t a, uint16_t b, uint16_t c,
                    const unsigned char d[8]) {
    out[0] = (unsigned char)(a & 0xFF);
    out[1] = (unsigned char)((a >> 8) & 0xFF);
    out[2] = (unsigned char)((a >> 16) & 0xFF);
    out[3] = (unsigned char)((a >> 24) & 0xFF);
    out[4] = (unsigned char)(b & 0xFF);
    out[5] = (unsigned char)((b >> 8) & 0xFF);
    out[6] = (unsigned char)(c & 0xFF);
    out[7] = (unsigned char)((c >> 8) & 0xFF);
    memcpy(out + 8, d, 8);
}

struct mk_dir {                 /* a directory we have to allocate/write */
    uint32_t cluster;           /* its first (only) cluster */
    unsigned char sect[MK_SECT];/* the 512 B it holds */
    int n;                      /* entries already used */
};

/* Add one entry to a directory buffer. Returns 0, -1 if the sector is full
 * (one cluster = 1 sector here, so >16 entries would need a chain; our tree
 * never does). */
static int mk_dir_add(struct mk_dir *d, const char *name83, uint8_t attr,
                      uint32_t cluster, uint32_t size) {
    if (d->n >= 16) return -1;
    mk_dirent(d->sect + (size_t)d->n * 32, name83, attr, cluster, size);
    d->n++;
    return 0;
}

/* ---- the build --------------------------------------------------------- */
int mkiso_build(const struct mk_opts *o) {
    struct mk_file mbr = {0}, st2 = {0}, kern = {0}, efi = {0},
                   initrd = {0}, kern32 = {0};
    FILE *img = NULL;
    int rc = -1;
    unsigned esp_mb = o->esp_mb ? o->esp_mb : 48;
    uint32_t esp_sec, disk_sec, tot32, fat_sz, data_sec, cluster_count;
    uint32_t fat_lba_rel, data_lba_rel;
    uint32_t next_clus, free_count;
    unsigned char *bpb = NULL, *fsinfo = NULL, *fatbuf = NULL, *zero = NULL;
    struct mk_dir root, edir, bdir;
    /* Files laid into the FAT, in allocation order (root & dirs first). */
    struct { struct mk_file *f; const char *root83; const char *boot83;
             uint32_t first; uint32_t nclus; } files[MK_MAX_FILES];
    int nfiles = 0;
    uint32_t i;

    if (esp_mb < MK_ESP_MIN_MB) {
        fprintf(stderr, "mkiso: ERROR: esp-mb=%u is below the %u MiB FAT32 "
                "floor (<65525 clusters makes OVMF reject the volume)\n",
                esp_mb, MK_ESP_MIN_MB);
        return -1;
    }
    if (!o->out || !o->mbr_path || !o->kernel_path || !o->efi_path) {
        fprintf(stderr, "mkiso: --mbr/--kernel/--efi and an output path are "
                "required\n");
        return -1;
    }
    if (mk_load(o->mbr_path, &mbr) != 0) return -1;
    if (o->stage2_path && mk_load(o->stage2_path, &st2) != 0) goto done;
    if (mk_load(o->kernel_path, &kern) != 0) goto done;
    if (mk_load(o->efi_path, &efi) != 0) goto done;
    if (o->initrd_path && mk_load(o->initrd_path, &initrd) != 0) goto done;
    if (o->kernel32_path && mk_load(o->kernel32_path, &kern32) != 0) goto done;

    /* The MBR must be a 512-byte boot sector with the 0x55AA signature. */
    if (mbr.size != 512 || mbr.data[510] != 0x55 || mbr.data[511] != 0xAA) {
        fprintf(stderr, "mkiso: MBR must be 512 B with 0x55AA signature\n");
        goto done;
    }
    /* initrd slot bound (mirrors the BIOS loader's 16 MiB reservation). */
    if (initrd.present && initrd.size > (long)(16 * 1024 * 1024)) {
        fprintf(stderr, "mkiso: initrd is %ld bytes (BIOS loader max 16 MiB)\n",
                initrd.size);
        goto done;
    }

    /* ---- FAT geometry: choose FAT size so clusters >= the FAT32 floor. */
    esp_sec = esp_mb * 2048u;                       /* MiB -> 512 B sectors */
    disk_sec = esp_sec + 2048u;                     /* +1 MiB for MBR/GPT   */
    tot32 = esp_sec;                                /* the FAT volume alone */
    /* FAT size: the FAT holds one 32-bit entry per *data cluster*, and each
     * FAT sector holds 128 entries.  Start from the count that assumes a
     * zero-sized FAT and add the sectors the FAT itself costs, iterating a
     * couple of times to a fixed point (the fat_sz sectors are themselves
     * not data clusters). */
    fat_sz = 0;
    for (i = 0; i < 4; i++) {
        data_sec = tot32 - MK_FAT_RESERVED - MK_FAT_NUMFATS * fat_sz;
        cluster_count = data_sec / MK_FAT_SPC;
        if (cluster_count < MK_FAT_MIN_CLUSTER) {
            fprintf(stderr, "mkiso: ERROR: %u MiB yields %u data clusters "
                    "(FAT32 needs >= %u); raise esp-mb\n",
                    esp_mb, cluster_count, MK_FAT_MIN_CLUSTER);
            goto done;
        }
        {
            uint32_t need = (cluster_count + 127u) / 128u + 1u;
            if (need <= fat_sz) break;
            fat_sz = need;
        }
    }
    fat_lba_rel  = MK_FAT_RESERVED;
    data_lba_rel = MK_FAT_RESERVED + MK_FAT_NUMFATS * fat_sz;

    bpb    = (unsigned char *)calloc(1, MK_SECT);
    fsinfo = (unsigned char *)calloc(1, MK_SECT);
    fatbuf = (unsigned char *)calloc(fat_sz, MK_SECT);
    zero   = (unsigned char *)calloc(1, MK_SECT);
    if (!bpb || !fsinfo || !fatbuf || !zero) goto done;

    /* ---- allocate directory clusters and register files. */
    memset(&root, 0, sizeof(root));
    memset(&edir, 0, sizeof(edir));
    memset(&bdir, 0, sizeof(bdir));
    root.cluster = MK_FAT_ROOTCLUS;      /* 2 */
    edir.cluster = 3;                    /* EFI/  */
    bdir.cluster = 4;                    /* EFI/BOOT/ */

    /* Volume label first, then directories, then root files. */
    if (mk_dir_add(&root, "", 0x08, 0, 0) != 0) goto done;       /* label */
    /* EFI/ entry: "." and ".." live inside it; its own entry goes in root. */
    if (mk_dir_add(&root, "EFI       ", 0x10, edir.cluster, 0) != 0) goto done;

    /* Register files (data clusters start at 5). */
    nfiles = 0;
    /* /KERNEL.ELF (BIOS Stage 2 root lookup) and /EFI/BOOT/KERNEL.ELF share
     * bytes but FAT has no hard links in a tree this small; copy like mcopy. */
    files[nfiles].f = &kern;   files[nfiles].root83 = "KERNEL  ELF";
    files[nfiles].boot83 = "KERNEL  ELF"; nfiles++;
    files[nfiles].f = &efi;    files[nfiles].root83 = NULL;
    files[nfiles].boot83 = "BOOTX64 EFI"; nfiles++;
    if (kern32.present) {
        files[nfiles].f = &kern32; files[nfiles].root83 = "KERNEL32ELF";
        files[nfiles].boot83 = NULL; nfiles++;
    }
    if (initrd.present) {
        files[nfiles].f = &initrd; files[nfiles].root83 = "INITRD  TAR";
        files[nfiles].boot83 = "INITRD  TAR"; nfiles++;
    }
    if (nfiles >= MK_MAX_FILES) goto done;

    next_clus = 5;
    for (i = 0; i < (uint32_t)nfiles; i++) {
        long bytes = files[i].f->size;
        uint32_t nc = (uint32_t)((bytes + MK_SECT - 1) / MK_SECT);
        if (nc == 0) nc = 1;                /* a 0-byte file still owns none,
                                               but keep a sane first cluster */
        if (bytes == 0) { files[i].first = 0; files[i].nclus = 0; continue; }
        files[i].first = next_clus;
        files[i].nclus = nc;
        next_clus += nc;
    }
    free_count = cluster_count - (next_clus - MK_FAT_ROOTCLUS);

    /* ---- build directory sectors. */
    /* EFI/BOOT/ contents. */
    mk_dir_add(&bdir, ".          ", 0x10, bdir.cluster, 0);
    mk_dir_add(&bdir, "..         ", 0x10, edir.cluster, 0);
    /* EFI/ contents. */
    mk_dir_add(&edir, ".          ", 0x10, edir.cluster, 0);
    mk_dir_add(&edir, "..         ", 0x10, root.cluster, 0);
    mk_dir_add(&edir, "BOOT      ", 0x10, bdir.cluster, 0);
    for (i = 0; i < (uint32_t)nfiles; i++) {
        if (files[i].boot83)
            mk_dir_add(&bdir, files[i].boot83, 0x20, files[i].first,
                       (uint32_t)files[i].f->size);
        if (files[i].root83)
            mk_dir_add(&root, files[i].root83, 0x20, files[i].first,
                       (uint32_t)files[i].f->size);
    }

    /* ---- FAT chains (contiguous; EOC at the end of each). */
    #define MK_FAT_SET(c, v) mk_wr32(fatbuf + (size_t)(c) * 4, (v))
    MK_FAT_SET(0, 0x0FFFFFF8u);
    MK_FAT_SET(1, MK_FAT_EOC);
    MK_FAT_SET(MK_FAT_ROOTCLUS, MK_FAT_EOC);
    MK_FAT_SET(edir.cluster, MK_FAT_EOC);
    MK_FAT_SET(bdir.cluster, MK_FAT_EOC);
    for (i = 0; i < (uint32_t)nfiles; i++) {
        uint32_t c, n;
        if (files[i].nclus == 0) continue;
        for (n = 0; n < files[i].nclus; n++) {
            c = files[i].first + n;
            MK_FAT_SET(c, (n + 1 == files[i].nclus) ? MK_FAT_EOC : c + 1);
        }
    }
    #undef MK_FAT_SET

    /* ---- open the image and write it. */
    img = fopen(o->out, "wb+");
    if (!img) { fprintf(stderr, "mkiso: cannot create %s\n", o->out); goto done; }

    /* BPB + FSInfo (+ backups at relative sector 6 / 7). */
    mk_make_bpb(bpb, tot32, fat_sz);
    mk_make_fsinfo(fsinfo, free_count, next_clus);
    {
        uint64_t base = MK_ESP_START_LBA;
        if (mk_wr_sect(img, base + 0, bpb) != 0) goto done;          /* BPB */
        if (mk_wr_sect(img, base + 1, fsinfo) != 0) goto done;      /* FSInfo */
        if (mk_wr_sect(img, base + 6, bpb) != 0) goto done;         /* backup BPB */
        if (mk_wr_sect(img, base + 7, fsinfo) != 0) goto done;      /* backup FSInfo */
        /* two FATs */
        for (i = 0; i < MK_FAT_NUMFATS; i++) {
            uint64_t flba = base + fat_lba_rel + (uint64_t)i * fat_sz;
            uint32_t s;
            for (s = 0; s < fat_sz; s++)
                if (mk_wr_sect(img, flba + s, fatbuf + (size_t)s * MK_SECT) != 0)
                    goto done;
        }
        /* directory clusters (data starts at data_lba_rel; cluster N at
         * data_lba_rel + (N-2)*spc). */
        #define MK_CLUS_LBA(c) (base + data_lba_rel + ((c) - 2) * MK_FAT_SPC)
        if (mk_wr_sect(img, MK_CLUS_LBA(root.cluster), root.sect) != 0) goto done;
        if (mk_wr_sect(img, MK_CLUS_LBA(edir.cluster), edir.sect) != 0) goto done;
        if (mk_wr_sect(img, MK_CLUS_LBA(bdir.cluster), bdir.sect) != 0) goto done;
        /* file data */
        for (i = 0; i < (uint32_t)nfiles; i++) {
            uint32_t n;
            if (files[i].nclus == 0) continue;
            for (n = 0; n < files[i].nclus; n++) {
                unsigned char sec[MK_SECT];
                long off = (long)n * MK_SECT;
                long rem = files[i].f->size - off;
                size_t cnt = rem >= MK_SECT ? MK_SECT : (size_t)rem;
                memset(sec, 0, MK_SECT);
                if (cnt > 0)
                    memcpy(sec, files[i].f->data + off, cnt);
                if (mk_wr_sect(img, MK_CLUS_LBA(files[i].first + n), sec) != 0)
                    goto done;
            }
        }
        #undef MK_CLUS_LBA
    }

    /* ---- MBR: splice mbr_dual.bin and patch its partition table. ------ */
    {
        unsigned char *m = (unsigned char *)malloc(512);
        if (!m) goto done;
        memcpy(m, mbr.data, 512);
        mk_wr32(m + 0x1B8, 0xAA55DEADu);            /* disk signature */
        /* slot 1 (0x1BE): bootable FAT32-LBA (0x0C) for BIOS Stage 2. */
        memset(m + 0x1BE, 0, 16);
        m[0x1BE + 0] = 0x80;  m[0x1BE + 1] = 0xFE;
        m[0x1BE + 2] = 0xFF;  m[0x1BE + 3] = 0xFF;
        m[0x1BE + 4] = 0x0C;  m[0x1BE + 5] = 0xFE;
        m[0x1BE + 6] = 0xFF;  m[0x1BE + 7] = 0xFF;
        mk_wr32(m + 0x1BE + 8, MK_ESP_START_LBA);
        mk_wr32(m + 0x1BE + 12, esp_sec);
        /* slot 2 (0x1CE): GPT protective (0xEE). */
        memset(m + 0x1CE, 0, 16);
        m[0x1CE + 0] = 0x00;  m[0x1CE + 1] = 0xFE;
        m[0x1CE + 2] = 0xFF;  m[0x1CE + 3] = 0xFF;
        m[0x1CE + 4] = 0xEE;  m[0x1CE + 5] = 0xFE;
        m[0x1CE + 6] = 0xFF;  m[0x1CE + 7] = 0xFF;
        mk_wr32(m + 0x1CE + 8, 1);
        mk_wr32(m + 0x1CE + 12, disk_sec - 1);
        m[510] = 0x55; m[511] = 0xAA;
        if (mk_wr_sect(img, 0, m) != 0) { free(m); goto done; }
        free(m);
    }

    /* ---- Stage 2 at LBA 34 (126 sectors max). */
    if (st2.present) {
        long ns = (st2.size + MK_SECT - 1) / MK_SECT;
        long s;
        if (ns > 126) {
            fprintf(stderr, "mkiso: stage2 is %ld B (max 126 sectors)\n", st2.size);
            goto done;
        }
        for (s = 0; s < ns; s++) {
            unsigned char sec[MK_SECT];
            long off = s * MK_SECT;
            long rem = st2.size - off;
            size_t cnt = rem >= MK_SECT ? MK_SECT : (size_t)rem;
            memset(sec, 0, MK_SECT);
            if (cnt > 0) memcpy(sec, st2.data + off, cnt);
            if (mk_wr_sect(img, MK_STAGE2_LBA + (uint64_t)s, sec) != 0) goto done;
        }
    }

    /* ---- GPT (primary + backup). */
    {
        unsigned char arr[MK_GPT_ENTRIES * MK_GPT_ENT_SIZE];
        unsigned char hdr[MK_SECT];
        const unsigned char esp_tail[8] =
            { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
        const unsigned char uniq_tail[8] =
            { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
        const unsigned char disk_tail[8] =
            { 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };
        uint32_t arr_crc;
        uint64_t esp_end = MK_ESP_START_LBA + (uint64_t)esp_sec - 1;
        uint64_t backup_arr_lba = disk_sec - 33;
        const char *name = "AURALITE ESP";

        memset(arr, 0, sizeof(arr));
        /* entry 0: the ESP */
        mk_guid(arr + 0, 0xC12A7328u, 0xF81F, 0x11D2, esp_tail);
        mk_guid(arr + 16, 0x12345678u, 0x9ABC, 0xDEF0, uniq_tail);
        mk_wr64(arr + 32, MK_ESP_START_LBA);
        mk_wr64(arr + 40, esp_end);
        mk_wr64(arr + 48, 0);                 /* attributes */
        for (i = 0; name[i] && i < 36; i++)
            arr[56 + i] = (unsigned char)name[i];   /* UTF-16LE: ASCII, 0 high */
        arr_crc = mk_crc32(arr, sizeof(arr));

        /* Primary header. */
        memset(hdr, 0, MK_SECT);
        memcpy(hdr + 0, "EFI PART", 8);
        mk_wr32(hdr + 8, 0x00010000u);       /* revision 1.0 */
        mk_wr32(hdr + 12, 92);               /* header size */
        mk_wr32(hdr + 16, 0);                /* header CRC placeholder */
        mk_wr32(hdr + 20, 0);                /* reserved */
        mk_wr64(hdr + 24, MK_GPT_HDR_LBA);   /* MyLBA */
        mk_wr64(hdr + 32, disk_sec - 1);     /* AlternateLBA */
        mk_wr64(hdr + 40, 34);               /* FirstUsableLBA */
        mk_wr64(hdr + 48, disk_sec - 34);    /* LastUsableLBA */
        mk_guid(hdr + 56, 0xAABBCCDDu, 0xEEFF, 0x0011, disk_tail);
        mk_wr64(hdr + 72, MK_GPT_ARR_LBA);   /* PartitionEntryLBA */
        mk_wr32(hdr + 80, MK_GPT_ENTRIES);
        mk_wr32(hdr + 84, MK_GPT_ENT_SIZE);
        mk_wr32(hdr + 88, arr_crc);
        mk_wr32(hdr + 16, mk_crc32(hdr, 92));

        /* Write primary header + array. */
        if (mk_wr_sect(img, MK_GPT_HDR_LBA, hdr) != 0) goto done;
        for (i = 0; i < 32; i++)
            if (mk_wr_sect(img, MK_GPT_ARR_LBA + i,
                           arr + (size_t)i * MK_SECT) != 0) goto done;

        /* Backup: array at disk-33, header at disk-1; swap My/Alternate and
         * repoint the entry array, then recompute the header CRC. */
        for (i = 0; i < 32; i++)
            if (mk_wr_sect(img, backup_arr_lba + i,
                           arr + (size_t)i * MK_SECT) != 0) goto done;
        mk_wr64(hdr + 24, disk_sec - 1);     /* MyLBA = last */
        mk_wr64(hdr + 32, MK_GPT_HDR_LBA);   /* AlternateLBA = 1 */
        mk_wr64(hdr + 72, backup_arr_lba);   /* PartitionEntryLBA */
        mk_wr32(hdr + 16, 0);
        mk_wr32(hdr + 16, mk_crc32(hdr, 92));
        if (mk_wr_sect(img, disk_sec - 1, hdr) != 0) goto done;
    }

    if (fflush(img) != 0) goto done;
    rc = 0;
    printf("[selfhost] mkiso PASS: %s written in-guest\n", o->out);
done:
    if (img) fclose(img);
    free(bpb); free(fsinfo); free(fatbuf); free(zero);
    free(mbr.data); free(st2.data); free(kern.data); free(efi.data);
    free(initrd.data); free(kern32.data);
    return rc;
}

/* ---- self-test: exercise the geometry math / dirent / CRC without a VM. */
int mkiso_selftest(void) {
    unsigned char e[32], guid[16];
    unsigned char bpb[MK_SECT], fi[MK_SECT];
    const unsigned char tail[8] = { 0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
    int fails = 0, it;
    uint32_t esp = 48 * 2048u, fatsz = 0, clusters = 0, data;

    /* CRC32 of "123456789" == 0xCBF43926 (the standard check vector). */
    if (mk_crc32("123456789", 9) != 0xCBF43926u) { printf("crc32 vector\n"); fails++; }

    /* 8.3 packing. */
    memset(e, 0, sizeof(e));
    mk_83(e, "KERNEL  ELF");
    if (memcmp(e, "KERNEL  ELF", 11) != 0) { printf("8.3 pack\n"); fails++; }

    /* dirent: attr, cluster split, size. */
    mk_dirent(e, "BOOTX64 EFI", 0x20, 0x12345678u, 0x9ABCDE);
    if (e[11] != 0x20) { printf("dirent attr\n"); fails++; }
    if (mk_rd16(e + 20) != 0x1234 || mk_rd16(e + 26) != 0x5678) {
        printf("dirent cluster\n"); fails++;
    }
    if (mk_rd32(e + 28) != 0x9ABCDEu) { printf("dirent size\n"); fails++; }

    /* volume label. */
    mk_dirent(e, "AURALITE", 0x08, 0, 0);
    if (e[11] != 0x08 || memcmp(e, "AURALITE   ", 11) != 0) {
        printf("vol label\n"); fails++;
    }

    /* geometry at 48 MiB clears the FAT32 floor and the FAT is sized
     * (one 512 B sector per 128 clusters), not left at zero. */
    for (it = 0; it < 4; it++) {
        data = esp - MK_FAT_RESERVED - MK_FAT_NUMFATS * fatsz;
        clusters = data / MK_FAT_SPC;
        fatsz = (clusters + 127u) / 128u + 1u;
    }
    if (clusters < MK_FAT_MIN_CLUSTER) { printf("floor\n"); fails++; }
    if (fatsz == 0) { printf("fat_sz zero\n"); fails++; }

    /* BPB: TotSec16 zero, TotSec32 set, root cluster 2, sig. */
    mk_make_bpb(bpb, esp, fatsz);
    if (mk_rd16(bpb + 19) != 0) { printf("TotSec16\n"); fails++; }
    if (mk_rd32(bpb + 32) != esp) { printf("TotSec32\n"); fails++; }
    if (mk_rd32(bpb + 44) != MK_FAT_ROOTCLUS) { printf("root clus\n"); fails++; }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) { printf("bpb sig\n"); fails++; }
    if (memcmp(bpb + 82, "FAT32", 5) != 0) { printf("fat32 str\n"); fails++; }

    /* FSInfo signatures. */
    mk_make_fsinfo(fi, 1, 5);
    if (mk_rd32(fi + 0) != 0x41615252u || mk_rd32(fi + 484) != 0x61417272u) {
        printf("fsinfo sig\n"); fails++;
    }

    /* ESP type GUID bytes (mixed endian) for C12A7328-F81F-11D2-BA4B... */
    mk_guid(guid, 0xC12A7328u, 0xF81F, 0x11D2, tail);
    {
        const unsigned char want[8] = { 0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11 };
        if (memcmp(guid, want, 8) != 0) { printf("guid le\n"); fails++; }
        if (memcmp(guid + 8, tail, 8) != 0) { printf("guid tail\n"); fails++; }
    }

    if (fails) { printf("mkiso selftest: %d check(s) FAILED\n", fails); return 1; }
    printf("all 10 SH7d mkiso selftest checks passed (clusters=%u fat_sz=%u)\n",
           clusters, fatsz);
    return 0;
}

#ifndef MKISO_NO_MAIN
static void mk_usage(void) {
    fprintf(stderr,
        "usage: mkiso [--esp-mb N] [--stage2 f] --mbr f --kernel f --efi f\n"
        "             [--initrd f] [--kernel32 f] <output.iso>\n"
        "       mkiso --selftest\n");
}

int main(int argc, char **argv) {
    struct mk_opts o;
    const char *out = NULL;
    int i;
    memset(&o, 0, sizeof(o));
    o.esp_mb = 48;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--selftest")) return mkiso_selftest();
        else if (!strcmp(a, "--esp-mb") && i + 1 < argc) o.esp_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--mbr") && i + 1 < argc) o.mbr_path = argv[++i];
        else if (!strcmp(a, "--stage2") && i + 1 < argc) o.stage2_path = argv[++i];
        else if (!strcmp(a, "--kernel") && i + 1 < argc) o.kernel_path = argv[++i];
        else if (!strcmp(a, "--efi") && i + 1 < argc) o.efi_path = argv[++i];
        else if (!strcmp(a, "--initrd") && i + 1 < argc) o.initrd_path = argv[++i];
        else if (!strcmp(a, "--kernel32") && i + 1 < argc) o.kernel32_path = argv[++i];
        else if (a[0] != '-' && !out) out = a;
        else { mk_usage(); return 2; }
    }
    o.out = out;
    if (!o.mbr_path || !o.kernel_path || !o.efi_path || !o.out) { mk_usage(); return 2; }
    return mkiso_build(&o) == 0 ? 0 : 1;
}
#endif
