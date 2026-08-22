/* fsglue_rv.c — what the shared fs objects need from this arch
 * (PARITY_PLAN.md P2).
 *
 * The adoption set is kernel/lib/kprintf.c + kernel/lib/spinlock.c +
 * kernel/fs/blkdev.c + kernel/fs/ext2.c, all compiled UNCHANGED (the
 * KERNELRV_SHARED contract).  Their undefined symbols, measured with
 * llvm-nm before a line of this file existed:
 *
 *   kprintf.c  -> uart_putchar, fb_putchar, klog_putchar, spinlock_*
 *   ext2.c     -> blkdev_*, kmalloc, kfree, kprintf, vfs_now, string.h
 *   spinlock.c -> (nothing; the V6 atomics work already paid this)
 *
 * This file provides exactly that surface and the vblk-to-blkdev
 * registration.  NOT here: vfs.c -- it does not even compile on this
 * target (a raw x86 `sti` at vfs.c:71, plus the scheduler/thread
 * coupling); the measured blocker is named in the plan, and the
 * path-level API waits for P4's syscall widening.
 */

#include <stdint.h>

#include "kernel/fs/blkdev.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ext2.h"
#include "kernel/mm/kheap.h"
#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/vblk_rv.h"
#include "kernel/lib/kprintf.h"

/* ---- kprintf's three sinks ------------------------------------------ */
/* The SBI console is the one output this kernel has; the fb and klog
 * sinks are honest no-ops (no framebuffer, no klog ring on rv64 yet). */

void uart_putchar(char c) { sbi_putc(c); }
void fb_putchar(char c)   { (void)c; }
void klog_putchar(char c) { (void)c; }

/* ---- the heap names ext2.c links against ---------------------------- */

void *kmalloc(size_t size)          { return kmalloc_rv(size); }
void  kfree(void *p)                { kfree_rv(p); }

/* ---- timestamps ------------------------------------------------------ */
/* rdtime counts at 10 MHz on QEMU virt (the DTB's timebase-frequency);
 * ext2 wants seconds.  Good enough for mtime until a real clock. */

uint64_t vfs_now(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t / 10000000u;
}

/* ---- vblk behind the seam ------------------------------------------- */
/* vblk_rv is single-sector by contract (buf512); the seam's count is
 * looped here, on the arch side, exactly as the plan's §6 says. */

static int vblk_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (vblk_rv_read(lba + i, p + (uint64_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static int vblk_bd_write(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf)
{
    (void)ctx;
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (vblk_rv_write(lba + i, p + (uint64_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static uint64_t vblk_bd_sectors(void *ctx)
{
    (void)ctx;
    return vblk_rv_sector_count();
}

static const struct blkdev_ops vblk_bd_ops = {
    .read         = vblk_bd_read,
    .write        = vblk_bd_write,
    .sector_count = vblk_bd_sectors,
};

/* ---- mount + proof --------------------------------------------------- */
/* Called from kmain_rv once vblk's own selftest has passed.  Registers
 * the device, mounts ext2 on it, and reads one named file through the
 * ops table -- the smoke pins every line this prints plus ext2.c's own
 * "[ext2] mounted existing volume" receipt. */

/* P4: the dispatcher asks for the mounted ops table (NULL until
 * rvfs_bringup succeeds) -- the fd layer lives in user_rv.c, the
 * MOUNT knowledge lives here, neither reaches into the other. */
static const struct vfs_ops *mounted_ops;

const struct vfs_ops *rvfs_ops(void)
{
    return mounted_ops;
}

void rvfs_bringup(void)
{
    int dev = blkdev_register("vblk0", &vblk_bd_ops, 0, BLKDEV_SECTOR_SIZE);
    if (dev < 0) {
        kprintf("[blkdev] REFUSED vblk0: rc=%d\n", dev);
        return;
    }
    kprintf("[blkdev] blk%d = vblk0 (virtio-mmio, %llu sectors)\n",
            dev, (unsigned long long)blkdev_sector_count(dev));
    int pk = blkdev_partition_kind(dev);
    if (pk > 0)
        kprintf("[blkdev] blk%d carries a %s partition table; raw "
                "mounts IGNORE it (RES-04)\n", dev,
                pk == BLKDEV_PART_GPT ? "GPT" : "MBR");

    if (ext2_init(dev) != 0) {
        kprintf("[rvfs] ext2 mount failed on blkdev %d\n", dev);
        return;
    }
    kprintf("[rvfs] mounted ext2 on blkdev %d (ops-level; fd layer: P4)\n",
            dev);
    mounted_ops = &ext2_ops;

    ext2_self_test();
    ext2_list();

    /* cat one known file, exactly the way the x86 VFS would drive the
     * ops table: lookup, then read at pos 0. */
    struct vnode *vn = ext2_ops.lookup(0, "LINUX.TXT");
    if (!vn) {
        kprintf("[rvfs] cat: LINUX.TXT not found\n");
        return;
    }
    char buf[128];
    int64_t n = ext2_ops.read(vn, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        kprintf("[rvfs] cat: read failed\n");
        return;
    }
    buf[n] = '\0';
    for (int64_t i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = ' ';
    kprintf("[rvfs] cat LINUX.TXT (%lld bytes): %s\n",
            (long long)n, buf);
}
