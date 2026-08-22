/* fsglue32.h -- PARITY P7: the i386 side of the shared-fs adoption. */
#ifndef AURALITE_ARCH_I386_FSGLUE32_H
#define AURALITE_ARCH_I386_FSGLUE32_H

/* Register the ATA drives with the blkdev seam; mount ext2 on the
 * slave when one is attached; prove a read. */
void fs32_bringup(void);

#endif /* AURALITE_ARCH_I386_FSGLUE32_H */
