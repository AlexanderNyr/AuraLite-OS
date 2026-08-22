/* fsglue_rv.h -- PARITY P2: the rv64 side of the shared-fs adoption. */
#ifndef AURALITE_ARCH_RISCV64_FSGLUE_RV_H
#define AURALITE_ARCH_RISCV64_FSGLUE_RV_H

/* Register vblk with the blkdev seam, mount ext2, prove a read. */
void rvfs_bringup(void);

/* P4: the mounted filesystem's ops table, or NULL before/without a
 * successful bringup.  The syscall fd layer keys off this. */
struct vfs_ops;
const struct vfs_ops *rvfs_ops(void);

#endif /* AURALITE_ARCH_RISCV64_FSGLUE_RV_H */
