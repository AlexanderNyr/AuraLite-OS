/* tests/unit/test_mkiso.c -- SELFHOST_PLAN.md SH7d host gate for mkiso.
 *
 * The twin pattern (same as test_mkinitrd / test_bootoffsets_twin): we
 * #include the REAL shipped writer (tools/selfhost/mkiso.c) with
 * MKISO_NO_MAIN so the exact bytes the guest tcc compiles and `sh build.sh
 * iso` runs are the bytes under test.  We build a synthetic hybrid image in
 * a temp file with a crafted MBR + Stage 2 + kernel/EFI/initrd, then parse
 * the result back and assert the on-disk contract both firmware paths rely
 * on:
 *
 *   - hybrid MBR: 0x55AA sig, disk id, bootable 0x0C FAT32 slot and the
 *     0xEE GPT-protective slot at the right LBAs;
 *   - GPT: "EFI PART" primary header at LBA1, entry array at LBA2, the ESP
 *     typed C12A7328-… and pointing at LBA 256, header/array CRC32 valid,
 *     backup header+array at the last sectors;
 *   - FAT32 ESP at LBA256: BPB TotSec16==0 / TotSec32 set, root cluster 2,
 *     >=65525 data clusters, FSInfo signatures, reserved FAT entries;
 *   - files land at the right 8.3 paths (/KERNEL.ELF, /EFI/BOOT/BOOTX64.EFI,
 *     /EFI/BOOT/KERNEL.ELF, /INITRD.TAR) and round-trip byte-identically
 *     through the cluster chains.
 *
 * Pure host C, no QEMU and no mtools dependency.
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
#include <unistd.h>

#define MKISO_NO_MAIN 1
#include "tools/selfhost/mkiso.c"

static int fails = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } \
} while (0)

/* Whole-image read-back helpers. */
static unsigned char *read_image(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = (unsigned char *)malloc((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return p;
}
static uint32_t rd32p(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64p(const unsigned char *p) {
    uint64_t v = 0; int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static uint16_t rd16p_local(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Write a temp file of `size` bytes with a recognisable fill pattern. */
static void write_pattern_file(const char *path, size_t size, unsigned seed) {
    FILE *f = fopen(path, "wb");
    unsigned char *b = (unsigned char *)malloc(size ? size : 1);
    for (size_t i = 0; i < size; i++)
        b[i] = (unsigned char)((i * 31u + seed) & 0xFF);
    fwrite(b, 1, size, f);
    free(b);
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/mkiso_test_XXXXXX";
    char mbr_p[128], st2_p[128], kern_p[128], efi_p[128],
         initrd_p[128], k32_p[128], out_p[128];
    struct mk_opts o;
    const size_t kern_sz = 200000, efi_sz = 10240,
                 initrd_sz = 300000, k32_sz = 50000, st2_sz = 5632;
    size_t img_len = 0;
    unsigned char *img;
    unsigned esp_mb = 48;
    uint32_t esp_sec = esp_mb * 2048u, disk_sec = esp_sec + 2048u;

    if (!mkdtemp(dir)) { printf("cannot make temp dir\n"); return 1; }
    snprintf(mbr_p,    sizeof mbr_p,    "%s/mbr.bin", dir);
    snprintf(st2_p,    sizeof st2_p,    "%s/stage2.bin", dir);
    snprintf(kern_p,   sizeof kern_p,   "%s/kernel.elf", dir);
    snprintf(efi_p,    sizeof efi_p,    "%s/bootx64.efi", dir);
    snprintf(initrd_p, sizeof initrd_p, "%s/initrd.tar", dir);
    snprintf(k32_p,    sizeof k32_p,    "%s/kernel32.elf", dir);
    snprintf(out_p,    sizeof out_p,    "%s/out.iso", dir);

    /* A 512-byte MBR with the boot signature (mkiso patches the table). */
    {
        unsigned char mbr[512];
        FILE *f;
        memset(mbr, 0xCC, sizeof mbr);   /* distinguishable boot code */
        mbr[510] = 0x55; mbr[511] = 0xAA;
        f = fopen(mbr_p, "wb"); fwrite(mbr, 1, 512, f); fclose(f);
    }
    write_pattern_file(st2_p, st2_sz, 7);
    write_pattern_file(kern_p, kern_sz, 1);
    write_pattern_file(efi_p, efi_sz, 2);
    write_pattern_file(initrd_p, initrd_sz, 3);
    write_pattern_file(k32_p, k32_sz, 4);

    memset(&o, 0, sizeof o);
    o.out = out_p; o.esp_mb = esp_mb;
    o.mbr_path = mbr_p; o.stage2_path = st2_p;
    o.kernel_path = kern_p; o.efi_path = efi_p;
    o.initrd_path = initrd_p; o.kernel32_path = k32_p;

    CHECK(mkiso_build(&o) == 0, "mkiso_build returns 0");

    img = read_image(out_p, &img_len);
    CHECK(img != NULL, "output image readable");
    if (!img) return 1;
    CHECK(img_len == (size_t)disk_sec * 512, "image size = (esp+1MiB) sectors");

    /* ---- MBR ---- */
    CHECK(img[510] == 0x55 && img[511] == 0xAA, "MBR boot sig");
    CHECK(rd32p(img + 0x1B8) == 0xAA55DEADu, "MBR disk signature");
    /* slot 1: bootable 0x0C at LBA 256, esp_sec sectors */
    CHECK(img[0x1BE + 0] == 0x80, "MBR slot1 bootable");
    CHECK(img[0x1BE + 4] == 0x0C, "MBR slot1 type 0x0C FAT32-LBA");
    CHECK(rd32p(img + 0x1BE + 8) == MK_ESP_START_LBA, "MBR slot1 starts LBA256");
    CHECK(rd32p(img + 0x1BE + 12) == esp_sec, "MBR slot1 size = esp sectors");
    /* slot 2: protective 0xEE */
    CHECK(img[0x1CE + 4] == 0xEE, "MBR slot2 GPT protective 0xEE");
    CHECK(rd32p(img + 0x1CE + 8) == 1, "MBR slot2 starts LBA1");
    /* MBR boot code preserved from input. */
    CHECK(img[0] == 0xCC, "MBR boot code preserved from input");

    /* ---- Stage 2 spliced at LBA 34 ---- */
    {
        const unsigned char *s2 = img + (size_t)MK_STAGE2_LBA * 512;
        size_t ok = 1, i;
        for (i = 0; i < st2_sz; i++)
            if (s2[i] != (unsigned char)((i * 31u + 7) & 0xFF)) { ok = 0; break; }
        CHECK(ok, "Stage2 bytes at LBA34 match input");
    }

    /* ---- GPT primary header at LBA 1 ---- */
    {
        const unsigned char *h = img + (size_t)MK_GPT_HDR_LBA * 512;
        uint32_t crc_stored, crc_calc;
        unsigned char tmp[512];
        CHECK(memcmp(h, "EFI PART", 8) == 0, "GPT signature");
        CHECK(rd32p(h + 12) == 92, "GPT header size 92");
        CHECK(rd64p(h + 24) == MK_GPT_HDR_LBA, "GPT MyLBA=1");
        CHECK(rd64p(h + 32) == disk_sec - 1, "GPT AlternateLBA=last");
        CHECK(rd64p(h + 72) == MK_GPT_ARR_LBA, "GPT entry array LBA=2");
        CHECK(rd32p(h + 80) == 128 && rd32p(h + 84) == 128,
              "GPT 128 entries x 128 B");
        /* verify header CRC (zero the CRC field, CRC 92 bytes). */
        crc_stored = rd32p(h + 16);
        memcpy(tmp, h, 512);
        memset(tmp + 16, 0, 4);
        crc_calc = mk_crc32(tmp, 92);
        CHECK(crc_stored == crc_calc, "GPT header CRC32 valid");
    }

    /* ---- GPT entry 0: ESP type + span ---- */
    {
        const unsigned char *e = img + (size_t)MK_GPT_ARR_LBA * 512;
        const unsigned char want_type[16] = {
            0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
            0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
        uint32_t arr_crc = mk_crc32(img + (size_t)MK_GPT_ARR_LBA * 512,
                                    128u * 128u);
        CHECK(memcmp(e + 0, want_type, 16) == 0, "GPT ESP type GUID");
        CHECK(rd64p(e + 32) == MK_ESP_START_LBA, "GPT ESP starts LBA256");
        CHECK(rd64p(e + 40) == MK_ESP_START_LBA + esp_sec - 1,
              "GPT ESP ends at LBA256+esp-1");
        /* partition array CRC matches header field. */
        {
            const unsigned char *h = img + 512;
            CHECK(rd32p(h + 88) == arr_crc, "GPT array CRC32 matches header");
        }
    }

    /* ---- backup GPT ---- */
    {
        const unsigned char *bh = img + (size_t)(disk_sec - 1) * 512;
        CHECK(memcmp(bh, "EFI PART", 8) == 0, "backup GPT signature");
        CHECK(rd64p(bh + 24) == disk_sec - 1, "backup MyLBA=last");
        CHECK(rd64p(bh + 32) == 1, "backup AlternateLBA=1");
        CHECK(rd64p(bh + 72) == (uint64_t)(disk_sec - 33),
              "backup entry array LBA");
        {
            unsigned char tmp[512]; uint32_t c;
            memcpy(tmp, bh, 512); memset(tmp + 16, 0, 4);
            c = mk_crc32(tmp, 92);
            CHECK(rd32p(bh + 16) == c, "backup GPT header CRC valid");
        }
    }

    /* ---- FAT32 BPB at LBA 256 ---- */
    {
        const unsigned char *b = img + (size_t)MK_ESP_START_LBA * 512;
        uint32_t fat_sz = rd32p(b + 36), tot32 = rd32p(b + 32);
        uint32_t data_lba_rel, clusters;
        uint32_t fat_lba_abs, data_lba_abs;
        CHECK(b[510] == 0x55 && b[511] == 0xAA, "BPB boot sig");
        CHECK(memcmp(b + 82, "FAT32", 5) == 0, "BPB fs type FAT32");
        CHECK(rd16p_local(b + 11) == 512, "BPB bytes/sector 512");
        CHECK(b[13] == 1, "BPB sectors/cluster 1");
        CHECK(rd16p_local(b + 14) == 32, "BPB reserved 32");
        CHECK(b[16] == 2, "BPB num FATs 2");
        CHECK(rd16p_local(b + 19) == 0,
              "BPB TotSec16 == 0 (FAT32 requirement)");
        CHECK(rd32p(b + 32) == esp_sec, "BPB TotSec32 = esp sectors");
        CHECK(b[21] == 0xF8, "BPB media 0xF8");
        CHECK(rd32p(b + 44) == 2, "BPB root cluster 2");
        CHECK(rd16p_local(b + 48) == 1, "BPB FSInfo at rel sector 1");
        CHECK(rd16p_local(b + 50) == 6, "BPB backup boot sector at 6");
        CHECK(rd32p(b + 28) == MK_ESP_START_LBA, "BPB hidden sectors = 256");
        /* cluster count clears the FAT32 floor. */
        data_lba_rel = 32 + 2 * fat_sz;
        clusters = tot32 - data_lba_rel;
        CHECK(clusters >= 65525, "FAT32 data clusters >= 65525");

        /* FSInfo at rel sector 1. */
        {
            const unsigned char *fi = b + 512;
            CHECK(rd32p(fi + 0) == 0x41615252u, "FSInfo lead sig");
            CHECK(rd32p(fi + 484) == 0x61417272u, "FSInfo struct sig");
        }
        /* reserved FAT entries: [0]=0x0FFFFFF8, [1]=EOC, [2]=EOC (root). */
        fat_lba_abs = MK_ESP_START_LBA + 32;
        (void)fat_lba_abs;
        {
            const unsigned char *fat = img + (size_t)(MK_ESP_START_LBA + 32) * 512;
            CHECK(rd32p(fat + 0) == 0x0FFFFFF8u, "FAT[0] media");
            CHECK(rd32p(fat + 4) == 0x0FFFFFFFu, "FAT[1] EOC");
            CHECK(rd32p(fat + 8) == 0x0FFFFFFFu, "FAT[2] root EOC");
        }
        data_lba_abs = MK_ESP_START_LBA + 32 + 2 * fat_sz;
        /* root dir cluster (2) -> sector data_lba_abs; find 8.3 entries. */
        {
            const unsigned char *root = img + (size_t)data_lba_abs * 512;
            int saw_label = 0, saw_efi = 0, saw_kernel = 0, saw_initrd = 0,
                saw_k32 = 0, e;
            for (e = 0; e < 16; e++) {
                const unsigned char *de = root + e * 32;
                if (de[0] == 0) break;
                if (de[11] == 0x08) saw_label = 1;
                if (de[11] == 0x10 && !memcmp(de, "EFI        ", 11)) saw_efi = 1;
                if (de[11] == 0x20 && !memcmp(de, "KERNEL  ELF", 11)) saw_kernel = 1;
                if (de[11] == 0x20 && !memcmp(de, "INITRD  TAR", 11)) saw_initrd = 1;
                if (de[11] == 0x20 && !memcmp(de, "KERNEL32ELF", 11)) saw_k32 = 1;
            }
            CHECK(saw_label, "root: volume label entry");
            CHECK(saw_efi, "root: EFI/ dir entry");
            CHECK(saw_kernel, "root: KERNEL.ELF entry");
            CHECK(saw_initrd, "root: INITRD.TAR entry");
            CHECK(saw_k32, "root: KERNEL32.ELF entry");
        }
        /* EFI/BOOT cluster (clusters 3 then 4): BOOTX64.EFI + KERNEL.ELF +
         * INITRD.TAR present. */
        {
            const unsigned char *bootdir =
                img + (size_t)(data_lba_abs + (4 - 2)) * 512;
            int saw_efi_app = 0, saw_kernel = 0, saw_initrd = 0, e;
            for (e = 0; e < 16; e++) {
                const unsigned char *de = bootdir + e * 32;
                if (de[0] == 0) break;
                if (de[11] == 0x20 && !memcmp(de, "BOOTX64 EFI", 11)) saw_efi_app = 1;
                if (de[11] == 0x20 && !memcmp(de, "KERNEL  ELF", 11)) saw_kernel = 1;
                if (de[11] == 0x20 && !memcmp(de, "INITRD  TAR", 11)) saw_initrd = 1;
            }
            CHECK(saw_efi_app, "EFI/BOOT: BOOTX64.EFI entry");
            CHECK(saw_kernel, "EFI/BOOT: KERNEL.ELF entry");
            CHECK(saw_initrd, "EFI/BOOT: INITRD.TAR entry");
        }
    }

    /* ---- file content round-trip: walk each 8.3 file's cluster chain. ---- */
    {
        const unsigned char *b = img + (size_t)MK_ESP_START_LBA * 512;
        uint32_t fat_sz = rd32p(b + 36);
        uint32_t data_lba_abs = MK_ESP_START_LBA + 32 + 2 * fat_sz;
        const unsigned char *fat = img + (size_t)(MK_ESP_START_LBA + 32) * 512;

        /* Find a file's first cluster by 8.3 name within a dir cluster. */
        #define FIND_FIRST(dirsec, name83) ({ \
            uint32_t _fc = 0; int _e; \
            for (_e = 0; _e < 16; _e++) { \
                const unsigned char *_de = (dirsec) + _e * 32; \
                if (_de[0] == 0) break; \
                if (_de[11] == 0x20 && !memcmp(_de, name83, 11)) { \
                    _fc = ((uint32_t)rd16p_local(_de + 20) << 16) | \
                          rd16p_local(_de + 26); break; } } _fc; })
        /* Read a file's bytes through its FAT chain; compare to pattern. */
        #define VERIFY_FILE(dirsec, name83, expect_sz, seed) do { \
            uint32_t _c = FIND_FIRST(dirsec, name83); \
            size_t _off = 0; int _ok = 1; \
            if (!_c) { printf("FAIL: no cluster for %s\n", name83); fails++; break; } \
            while (_c >= 2 && (_c & 0x0FFFFFFFu) < 0x0FFFFFF8u) { \
                const unsigned char *_p = img + \
                    (size_t)(data_lba_abs + (_c - 2)) * 512; \
                size_t _n = 512; \
                if (_off + _n > (expect_sz)) _n = (expect_sz) - _off; \
                for (size_t _i = 0; _i < _n; _i++) \
                    if (_p[_i] != (unsigned char)(((_off + _i) * 31u + (seed)) & 0xFF)) \
                        { _ok = 0; } \
                _off += _n; \
                if (_off >= (expect_sz)) break; \
                _c = rd32p(fat + (size_t)_c * 4) & 0x0FFFFFFFu; } \
            CHECK(_ok && _off == (expect_sz), "content round-trip: " name83); \
        } while (0)

        const unsigned char *rootdir = img + (size_t)data_lba_abs * 512;
        const unsigned char *bootdir = img + (size_t)(data_lba_abs + 2) * 512;
        VERIFY_FILE(rootdir, "KERNEL  ELF", kern_sz, 1);
        VERIFY_FILE(rootdir, "INITRD  TAR", initrd_sz, 3);
        VERIFY_FILE(bootdir, "BOOTX64 EFI", efi_sz, 2);
        VERIFY_FILE(bootdir, "KERNEL  ELF", kern_sz, 1);
        VERIFY_FILE(bootdir, "INITRD  TAR", initrd_sz, 3);
        #undef FIND_FIRST
        #undef VERIFY_FILE
    }

    /* ---- reject a too-small ESP (< 40 MiB / < 65525 clusters) ---- */
    {
        struct mk_opts bad = o;
        char bad_out[128];
        snprintf(bad_out, sizeof bad_out, "%s/bad.iso", dir);
        bad.out = bad_out; bad.esp_mb = 16;
        CHECK(mkiso_build(&bad) != 0, "esp-mb 16 (<floor) rejected");
    }

    /* ---- selftest ---- */
    CHECK(mkiso_selftest() == 0, "mkiso_selftest passes");

    free(img);
    if (fails) {
        printf("\n%d SH7d mkiso check(s) FAILED\n", fails);
        return 1;
    }
    printf("all SH7d mkiso checks passed\n");
    return 0;
}
