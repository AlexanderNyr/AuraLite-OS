#ifndef AURALITE_FS_NTFS_H
#define AURALITE_FS_NTFS_H

#include "kernel/fs/vfs.h"
#include <stdint.h>

/* ============================================================================
 * F5b — NTFS, read-only.
 *
 * This header and kernel/fs/ntfs.c implement a real NTFS reader, not a
 * fake-vnode skeleton.  What is implemented:
 *   - boot-sector parse (BytesPerSector, SectorsPerCluster, $MFT LCN,
 *     ClustersPerFileRecord, ClustersPerIndexBuffer);
 *   - MFT record parse (FILE magic + update-sequence-array fixup);
 *   - attribute walk ($STANDARD_INFORMATION, $FILE_NAME, $DATA,
 *     INDEX_ROOT / INDEX_ALLOCATION $I30);
 *   - runlist decode (the sparse (lcn,count) runs) and file read through
 *     runs, resident or non-resident;
 *   - $MFT itself located through its own $DATA runlist (falls back to the
 *     boot-sector $MFT LCN as a single run when record 0 is unreadable);
 *   - root and subdirectory readdir via the $I30 index;
 *   - stat (type, size, nlink, mode).
 *
 * The volume is mounted READ-ONLY.  Every mutation operation
 * (create/mkdir/write/unlink/rmdir/rename/truncate/settimes/link) prints a
 * `[ntfs] ... refused: read-only (-EROFS)` line and returns -EROFS.  A
 * write/open-for-create attempt surfaces -EROFS through the VFS.  This is
 * the "stop lying" property from FSFULL_PLAN.md F5b: no fake vnode is ever
 * handed out and no write path pretends to succeed.
 *
 * Honest scope notes (FSFULL_PLAN.md F5b / F6):
 *   - read-only by design; there is deliberately no write path.
 *   - only ASCII filename bytes are transcoded from the $FILE_NAME UTF-16LE
 *     name (non-ASCII names are still listed/compared but through their low
 *     bytes only — case-insensitive; exact Unicode upcasing is out of scope).
 *   - no $ATTRIBUTE_LIST (files needing more than one attribute record) and
 *     no compression / sparse $DATA streams beyond returning zero runs.
 *   - no NTFS security / ACL / EFS / encrypted-file handling.
 *
 * Caller contract (F1): mount only when ntfs_init() returns 0.  A foreign
 * or unreadable boot sector returns -1 and is never mounted.
 * ============================================================================ */

/* Largest MFT record size we will read into a stack buffer. */
#define NTFS_MAX_REC_SZ  4096

/* Well-known MFT indices. */
#define NTFS_MFT_INDEX_ROOT      5   /* the "\" root directory */
#define NTFS_MFT_INDEX_MFT       0

/* FILE_ATTRIBUTE flag bits (from $STANDARD_INFORMATION / $FILE_NAME). */
#define NTFS_ATTR_FLAG_READONLY  0x00000001u
#define NTFS_ATTR_FLAG_DIRECTORY 0x00000010u
#define NTFS_ATTR_FLAG_ARCHIVE   0x00000020u

/* Attribute types we consume. */
#define NTFS_AT_STANDARD_INFORMATION 0x10
#define NTFS_AT_FILENAME             0x30
#define NTFS_AT_DATA                 0x80
#define NTFS_AT_INDEX_ROOT           0x90
#define NTFS_AT_INDEX_ALLOCATION     0xA0

#define NTFS_ATTR_END      0xFFFFFFFFu
/* Attribute-header byte 8 is the non-resident flag: 0 = resident,
 * 1 = non-resident.  Only bit 0 is meaningful. */
#define NTFS_ATTR_NONRESID 0x01u

/* 0 on success (real NTFS signature + usable geometry), -1 on refusal — the
 * caller mounts only on success (FSFULL_PLAN.md F1). */
int ntfs_init(int device_id);

/* In-driver structural self-test against the mounted volume.  Returns 0 and
 * prints receipts on success, nonzero on failure. */
int ntfs_self_test(void);

extern const struct vfs_ops ntfs_ops;

#endif /* AURALITE_FS_NTFS_H */
