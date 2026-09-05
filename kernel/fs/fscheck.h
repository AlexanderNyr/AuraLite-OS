#ifndef AURALITE_KERNEL_FS_FSCHECK_H
#define AURALITE_KERNEL_FS_FSCHECK_H

/* kernel/fs/fscheck.h — read-only consistency walkers for FAT32 and
 * ext2 (RESIDUE2 T3, "fsck tooling: CHECKS first, repair second").
 *
 * These are DEFENSIVE CONSISTENCY CHECKS, not repairers: they walk an
 * already-mounted volume's on-disk structures through the blkdev seam,
 * name every inconsistency they find with a greppable
 * `[fscheck] <fs>: FINDING: ...` line, and summarise with either
 * `[fscheck] <fs>: CLEAN (...)` or `[fscheck] <fs>: N finding(s)`.
 * They never write a byte of the volume.
 *
 * Two consumers:
 *   - the kernel boot path, opt-in via `-fw_cfg name=opt/auralite.fscheck,string=1`
 *     (mirrors the fsformat/selftest knobs); the gate state lives here;
 *   - tests/unit/test_fscheck.c, which feeds the SAME walker code
 *     crafted + deliberately-corrupted images through a RAM blkdev and
 *     pins each named finding class.
 *
 * Design rule honoured: the walkers stand alone — they parse the BPB /
 * superblock themselves and do not touch fat32.c / ext2.c internal
 * state, so a checker can never agree with the bug it is checking for.
 */

#include <stdint.h>

/* Run the walkers.  `base_lba` is where the volume starts on the
 * device (the FAT32 superfloppy sits at LBA 64, the ext2 volumes at 0).
 * Return the number of findings (0 = clean volume) or -1 when the
 * volume cannot even be read/parsed.  Every finding is printed as it
 * is detected. */
int fscheck_fat32(int dev, uint32_t base_lba);
int fscheck_ext2(int dev, uint32_t base_lba);

/* Boot gate (fsformat.c shape): build default OFF, fw_cfg overrides. */
int         fscheck_enabled(void);
const char *fscheck_source(void);
void        fscheck_set(int enabled, const char *source);

#endif /* AURALITE_KERNEL_FS_FSCHECK_H */
