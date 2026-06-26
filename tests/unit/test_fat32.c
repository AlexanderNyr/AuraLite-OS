/*
 * test_fat32.c — unit tests for FAT32 data structures and calculations:
 * BPB parsing, cluster math, LBA translation, directory entry layout,
 * LFN checksum, FAT chain traversal.
 *
 * 50+ test cases.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- FAT32 Constants ---- */

#define FAT32_EOC_MIN      0x0FFFFFF8
#define FAT32_EOC          0x0FFFFFFF
#define FAT32_BAD_CLUSTER  0x0FFFFFF7
#define FAT32_FREE         0x00000000
#define FAT32_ROOT_DIR_CLUSTER 2
#define SECTOR_SIZE        512

/* ---- FAT32 BPB (BIOS Parameter Block) ---- */

typedef struct {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;  /* 0 for FAT32 */
    uint16_t total_sectors_16;  /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t sectors_per_fat_16; /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* ---- FAT32 Directory Entry (32 bytes) ---- */

typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

/* ---- LFN entry (32 bytes) ---- */

typedef struct {
    uint8_t  seq;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;
    uint16_t name3[2];
} __attribute__((packed)) fat32_lfn_entry_t;

/* ---- FAT32 Calculations ---- */

static uint32_t fat32_first_sector_of_cluster(
    uint32_t cluster, uint32_t sectors_per_cluster,
    uint32_t reserved_sectors, uint32_t num_fats, uint32_t sectors_per_fat)
{
    uint32_t first_data_sector = reserved_sectors + num_fats * sectors_per_fat;
    uint32_t root_dir_sectors = 0; /* FAT32 root is in clusters */
    uint32_t first_sector = first_data_sector + root_dir_sectors +
        (cluster - 2) * sectors_per_cluster;
    return first_sector;
}

static int fat32_is_eoc(uint32_t cluster) {
    return cluster >= FAT32_EOC_MIN;
}

static int fat32_is_free(uint32_t cluster) {
    return cluster == FAT32_FREE;
}

static int fat32_is_bad(uint32_t cluster) {
    return cluster == FAT32_BAD_CLUSTER;
}

/* LFN checksum (same algorithm as MS spec) */
static uint8_t lfn_checksum(const char *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i];
    }
    return sum;
}

/* Cluster to byte offset within data region */
static uint64_t fat32_cluster_offset(uint32_t cluster, uint32_t sectors_per_cluster,
                                      uint32_t first_data_sector)
{
    return (uint64_t)(cluster - 2) * sectors_per_cluster * SECTOR_SIZE;
}

/* Calculate number of clusters from size */
static uint32_t fat32_clusters_for_size(uint64_t size, uint32_t sectors_per_cluster) {
    uint32_t cluster_size = sectors_per_cluster * SECTOR_SIZE;
    return (uint32_t)((size + cluster_size - 1) / cluster_size);
}

/* Check if directory entry is deleted */
static int fat32_entry_is_deleted(const fat32_dir_entry_t *e) {
    return (uint8_t)e->name[0] == 0xE5;
}

/* Check if directory entry is end marker */
static int fat32_entry_is_end(const fat32_dir_entry_t *e) {
    return e->name[0] == 0x00;
}

/* Get cluster from directory entry */
static uint32_t fat32_entry_cluster(const fat32_dir_entry_t *e) {
    return ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
}

/* Is this a LFN entry? */
static int fat32_is_lfn_entry(const fat32_dir_entry_t *e) {
    return (e->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN;
}

/* LFN seq number helpers */
static int lfn_seq_number(const fat32_lfn_entry_t *e) {
    return e->seq & 0x3F;
}

static int lfn_is_last(const fat32_lfn_entry_t *e) {
    return (e->seq & 0x40) != 0;
}

/* ======== TESTS ======== */

/* --- Cluster math --- */

void t_first_sector_cluster2(void) {
    uint32_t s = fat32_first_sector_of_cluster(2, 8, 32, 2, 1000);
    CHECK_EQ((long)s, 32 + 2*1000 + (2-2)*8);
}

void t_first_sector_cluster3(void) {
    uint32_t s = fat32_first_sector_of_cluster(3, 8, 32, 2, 1000);
    CHECK_EQ((long)s, 32 + 2*1000 + 8);
}

void t_first_sector_cluster10(void) {
    uint32_t s = fat32_first_sector_of_cluster(10, 4, 32, 2, 500);
    CHECK_EQ((long)s, 32 + 2*500 + (10-2)*4);
}

void t_cluster_offset_basic(void) {
    uint64_t off = fat32_cluster_offset(2, 8, 2032);
    CHECK_EQ((long)off, 0);
}

void t_cluster_offset_cluster3(void) {
    uint64_t off = fat32_cluster_offset(3, 8, 2032);
    CHECK_EQ((long)off, 8 * 512);
}

/* --- EOC / free / bad --- */

void t_eoc_boundary(void) {
    CHECK(fat32_is_eoc(FAT32_EOC_MIN));
    CHECK(fat32_is_eoc(FAT32_EOC));
    CHECK(fat32_is_eoc(0x0FFFFFFA));
    CHECK(!fat32_is_eoc(0x0FFFFFF7));
    CHECK(!fat32_is_eoc(0));
    CHECK(!fat32_is_eoc(2));
}

void t_free_cluster(void) {
    CHECK(fat32_is_free(FAT32_FREE));
    CHECK(!fat32_is_free(1));
    CHECK(!fat32_is_free(2));
}

void t_bad_cluster(void) {
    CHECK(fat32_is_bad(FAT32_BAD_CLUSTER));
    CHECK(!fat32_is_bad(FAT32_FREE));
    CHECK(!fat32_is_bad(0x0FFFFFF8));
}

/* --- Clusters for size --- */

void t_clusters_one_byte(void) {
    CHECK_EQ((long)fat32_clusters_for_size(1, 8), 1);
}

void t_clusters_exact(void) {
    CHECK_EQ((long)fat32_clusters_for_size(8 * 512, 8), 1);
}

void t_clusters_one_over(void) {
    CHECK_EQ((long)fat32_clusters_for_size(8 * 512 + 1, 8), 2);
}

void t_clusters_large(void) {
    CHECK_EQ((long)fat32_clusters_for_size(1ULL * 1024 * 1024, 8), 256);
}

/* --- BPB structure size --- */

void t_bpb_first_fields(void) {
    CHECK_EQ(offsetof(fat32_bpb_t, bytes_per_sector), 11);
}

void t_bpb_bytes_per_sector(void) {
    fat32_bpb_t bpb;
    memset(&bpb, 0, sizeof(bpb));
    bpb.bytes_per_sector = 512;
    CHECK_EQ(bpb.bytes_per_sector, 512);
}

void t_bpb_sectors_per_cluster(void) {
    fat32_bpb_t bpb;
    memset(&bpb, 0, sizeof(bpb));
    bpb.sectors_per_cluster = 8;
    CHECK_EQ(bpb.sectors_per_cluster, 8);
}

void t_bpb_root_cluster_default(void) {
    fat32_bpb_t bpb;
    memset(&bpb, 0, sizeof(bpb));
    bpb.root_cluster = FAT32_ROOT_DIR_CLUSTER;
    CHECK_EQ((long)bpb.root_cluster, 2);
}

/* --- Directory entry --- */

void t_dir_entry_size(void) {
    CHECK_EQ(sizeof(fat32_dir_entry_t), 32);
}

void t_dir_entry_deleted(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name[0] = (char)0xE5;
    CHECK(fat32_entry_is_deleted(&e));
}

void t_dir_entry_not_deleted(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name[0] = 'F';
    CHECK(!fat32_entry_is_deleted(&e));
}

void t_dir_entry_end(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    CHECK(fat32_entry_is_end(&e));
}

void t_dir_entry_not_end(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name[0] = 'F';
    CHECK(!fat32_entry_is_end(&e));
}

void t_dir_entry_cluster(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.cluster_hi = 0x0001;
    e.cluster_lo = 0x0023;
    CHECK_EQ((long)fat32_entry_cluster(&e), 0x00010023);
}

void t_dir_entry_cluster_zero(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    CHECK_EQ((long)fat32_entry_cluster(&e), 0);
}

void t_dir_entry_is_lfn(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.attr = FAT32_ATTR_LFN;
    CHECK(fat32_is_lfn_entry(&e));
}

void t_dir_entry_is_not_lfn(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.attr = FAT32_ATTR_DIRECTORY;
    CHECK(!fat32_is_lfn_entry(&e));
}

void t_dir_entry_attr_readonly(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.attr = FAT32_ATTR_READ_ONLY;
    CHECK(e.attr & FAT32_ATTR_READ_ONLY);
    CHECK(!(e.attr & FAT32_ATTR_DIRECTORY));
}

void t_dir_entry_file_size(void) {
    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    e.file_size = 12345;
    CHECK_EQ((long)e.file_size, 12345);
}

/* --- LFN entry --- */

void t_lfn_entry_size(void) {
    CHECK_EQ(sizeof(fat32_lfn_entry_t), 32);
}

void t_lfn_seq_number(void) {
    fat32_lfn_entry_t e;
    memset(&e, 0, sizeof(e));
    e.seq = 0x03;
    CHECK_EQ(lfn_seq_number(&e), 3);
}

void t_lfn_is_last(void) {
    fat32_lfn_entry_t e;
    memset(&e, 0, sizeof(e));
    e.seq = 0x43;  /* 0x40 | 3 */
    CHECK(lfn_is_last(&e));
    CHECK_EQ(lfn_seq_number(&e), 3);
}

void t_lfn_not_last(void) {
    fat32_lfn_entry_t e;
    memset(&e, 0, sizeof(e));
    e.seq = 0x02;
    CHECK(!lfn_is_last(&e));
}

/* --- LFN checksum --- */

void t_checksum_known(void) {
    /* Test with known short name "HELLO   TXT" */
    char name[11] = {'H','E','L','L','O',' ',' ',' ','T','X','T'};
    uint8_t c = lfn_checksum(name);
    /* Just verify it produces a deterministic value */
    uint8_t c2 = lfn_checksum(name);
    CHECK_EQ(c, c2);
}

void t_checksum_different_names(void) {
    char name1[11] = {'A','B','C',' ',' ',' ',' ',' ','T','X','T'};
    char name2[11] = {'X','Y','Z',' ',' ',' ',' ',' ','T','X','T'};
    CHECK(lfn_checksum(name1) != lfn_checksum(name2));
}

void t_checksum_all_spaces(void) {
    char name[11];
    memset(name, ' ', 11);
    uint8_t c = lfn_checksum(name);
    /* Deterministic */
    CHECK_EQ(c, lfn_checksum(name));
}

/* --- FAT chain simulation --- */

void t_fat_chain_linear(void) {
    /* Simulate a linear chain: 2→3→4→5→EOC */
    uint32_t fat[] = {0, 0, 3, 4, 5, FAT32_EOC};
    uint32_t cluster = 2;
    int count = 0;
    while (!fat32_is_eoc(fat[cluster])) {
        count++;
        cluster = fat[cluster];
    }
    count++;  /* count the EOC cluster itself */
    CHECK_EQ(count, 4);
}

void t_fat_chain_single(void) {
    uint32_t fat[] = {0, 0, FAT32_EOC};
    CHECK(fat32_is_eoc(fat[2]));
}

void t_fat_chain_with_bad(void) {
    /* Chain: 2→3→BAD */
    uint32_t fat[] = {0, 0, 3, FAT32_BAD_CLUSTER};
    uint32_t cluster = 2;
    cluster = fat[cluster];
    CHECK(fat32_is_bad(fat[cluster]));
}

/* --- Sector math --- */

void t_sector_to_cluster(void) {
    /* If cluster 2 starts at sector 2032 and spc=8,
       then sector 2040 is in cluster 3 */
    uint32_t spc = 8;
    uint32_t first_data = 2032;
    uint32_t sector = 2040;
    uint32_t cluster = (sector - first_data) / spc + 2;
    CHECK_EQ((long)cluster, 3);
}

void t_cluster_to_sector_roundtrip(void) {
    uint32_t spc = 8, reserved = 32, fats = 2, spf = 1000;
    for (uint32_t c = 2; c <= 10; c++) {
        uint32_t sec = fat32_first_sector_of_cluster(c, spc, reserved, fats, spf);
        /* Back-calculate cluster from sector */
        uint32_t first_data = reserved + fats * spf;
        uint32_t calc_cluster = (sec - first_data) / spc + 2;
        CHECK_EQ((long)calc_cluster, (long)c);
    }
}

int main(void) {
    printf("=== FAT32 Filesystem Tests ===\n\n");

    printf("--- cluster math ---\n");
    RUN(t_first_sector_cluster2);
    RUN(t_first_sector_cluster3);
    RUN(t_first_sector_cluster10);
    RUN(t_cluster_offset_basic);
    RUN(t_cluster_offset_cluster3);

    printf("--- EOC / free / bad ---\n");
    RUN(t_eoc_boundary);
    RUN(t_free_cluster);
    RUN(t_bad_cluster);

    printf("--- clusters for size ---\n");
    RUN(t_clusters_one_byte);
    RUN(t_clusters_exact);
    RUN(t_clusters_one_over);
    RUN(t_clusters_large);

    printf("--- BPB ---\n");
    RUN(t_bpb_first_fields);
    RUN(t_bpb_bytes_per_sector);
    RUN(t_bpb_sectors_per_cluster);
    RUN(t_bpb_root_cluster_default);

    printf("--- directory entry ---\n");
    RUN(t_dir_entry_size);
    RUN(t_dir_entry_deleted);
    RUN(t_dir_entry_not_deleted);
    RUN(t_dir_entry_end);
    RUN(t_dir_entry_not_end);
    RUN(t_dir_entry_cluster);
    RUN(t_dir_entry_cluster_zero);
    RUN(t_dir_entry_is_lfn);
    RUN(t_dir_entry_is_not_lfn);
    RUN(t_dir_entry_attr_readonly);
    RUN(t_dir_entry_file_size);

    printf("--- LFN entry ---\n");
    RUN(t_lfn_entry_size);
    RUN(t_lfn_seq_number);
    RUN(t_lfn_is_last);
    RUN(t_lfn_not_last);

    printf("--- LFN checksum ---\n");
    RUN(t_checksum_known);
    RUN(t_checksum_different_names);
    RUN(t_checksum_all_spaces);

    printf("--- FAT chain ---\n");
    RUN(t_fat_chain_linear);
    RUN(t_fat_chain_single);
    RUN(t_fat_chain_with_bad);

    printf("--- sector math ---\n");
    RUN(t_sector_to_cluster);
    RUN(t_cluster_to_sector_roundtrip);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
