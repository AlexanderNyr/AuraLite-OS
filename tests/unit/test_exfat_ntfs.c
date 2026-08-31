/* test_exfat_ntfs.c — host unit test for the FSFULL F1 mount-refusal
 * paths of the exFAT and NTFS drivers.
 *
 * Compiles the REAL kernel/fs/exfat.c and kernel/fs/ntfs.c (not copies)
 * against stubs for kprintf/kmalloc and a RAM-backed bc_get.  Why a host
 * test at all: the NTFS slot is blkdev 6, which QEMU cannot reach with a
 * single 6-port AHCI controller, so the NTFS refusal path has no
 * integration lane — this is that lane.
 *
 * The safety property under test: a foreign or unreadable boot sector
 * makes *_init() return -1 (so the caller will NOT vfs_mount), while a
 * correct OEM signature is accepted.  Neither file contains a write path,
 * so "no writes happen on refusal" is structural here, not asserted.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/exfat.h"
#include "kernel/fs/ntfs.h"

/* ---- stubs for the sources under test ---- */

int kprintf(const char *fmt, ...) { (void)fmt; return 0; }

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

/* RAM-backed fake buffer cache.  The test fills fake_sector, then
 * bc_get returns a buffer carrying a copy of it (or NULL when
 * fake_read_fails is set). */
static uint8_t  fake_sector[BC_BLOCK_SIZE];
static struct buffer fake_buffer;
static int fake_read_fails = 0;

struct buffer *bc_get(uint32_t device_id, uint64_t block_num) {
    (void)device_id;
    (void)block_num;
    if (fake_read_fails) return NULL;
    memset(&fake_buffer, 0, sizeof(fake_buffer));
    memcpy(fake_buffer.data, fake_sector, BC_BLOCK_SIZE);
    return &fake_buffer;
}

void bc_release(struct buffer *buf) { (void)buf; }

/* ---- test harness ---- */

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS: %s\n", msg);                                     \
        } else {                                                             \
            printf("  FAIL: %s\n", msg);                                     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void clear_sector(void) {
    memset(fake_sector, 0x5A, sizeof(fake_sector));   /* foreign pattern */
    fake_read_fails = 0;
}

int main(void) {
    printf("[fs-sigs] foreign boot sector is refused by both drivers\n");
    clear_sector();
    CHECK(exfat_init(2) != 0, "exfat_init refuses a foreign sector");
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses a foreign sector");

    printf("[fs-sigs] unreadable device is refused by both drivers\n");
    clear_sector();
    fake_read_fails = 1;
    CHECK(exfat_init(2) != 0, "exfat_init refuses an unreadable sector");
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses an unreadable sector");

    printf("[fs-sigs] correct OEM signatures are accepted\n");
    clear_sector();
    memcpy(fake_sector + 3, "EXFAT   ", 8);
    CHECK(exfat_init(2) == 0, "exfat_init accepts the EXFAT OEM name");
    clear_sector();
    memcpy(fake_sector + 3, "NTFS    ", 8);
    CHECK(ntfs_init(6) == 0, "ntfs_init accepts the NTFS OEM name");

    printf("[fs-sigs] near-miss signatures are still refused\n");
    clear_sector();
    memcpy(fake_sector + 3, "EXFAT  ", 8);            /* 7 chars, not 8 */
    CHECK(exfat_init(2) != 0, "exfat_init refuses 'EXFAT  ' (short OEM)");
    clear_sector();
    memcpy(fake_sector + 3, "NTFS", 4);               /* short magic */
    CHECK(ntfs_init(6) != 0, "ntfs_init refuses a truncated OEM");

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", failures);
    return 1;
}
