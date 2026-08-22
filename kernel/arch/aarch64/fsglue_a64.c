/* fsglue_a64.c — what the shared fs objects need from this arch
 * (PARITY_PLAN.md P3; fsglue_rv.c's mirror, fourth consumer of the
 * blkdev seam).
 *
 * The adoption set is identical to rv64's — kernel/lib/kprintf.c +
 * kernel/lib/spinlock.c + kernel/fs/blkdev.c + kernel/fs/ext2.c,
 * compiled UNCHANGED — and so is the measured symbol surface this
 * file provides: kprintf's three sinks, the heap names, vfs_now,
 * and the vblk-to-blkdev registration (single-sector backend, the
 * seam's count looped arch-side per the plan's §6).
 *
 * vfs.c stays home for the same measured reason recorded in
 * fsglue_rv.c (a raw x86 `sti` at vfs.c:71 + scheduler coupling);
 * the path-level API waits for P4.
 */

#include <stdint.h>

#include "kernel/fs/blkdev.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ext2.h"
#include "kernel/mm/kheap.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/vblk_a64.h"
#include "kernel/lib/kprintf.h"

/* ---- kprintf's three sinks ------------------------------------------ */

void uart_putchar(char c) { pl011_putc(c); }
void fb_putchar(char c)   { (void)c; }
void klog_putchar(char c) { (void)c; }

/* ---- the heap names ext2.c links against ---------------------------- */

void *kmalloc(size_t size)          { return kmalloc_a64(size); }
void  kfree(void *p)                { kfree_a64(p); }

/* ---- timestamps ------------------------------------------------------ */
/* cntvct_el0 / cntfrq_el0: the architected counter and its frequency
 * (QEMU virt: 62.5 MHz) -- seconds since boot, same contract as the
 * rv64 glue's rdtime/10MHz. */

uint64_t vfs_now(void)
{
    uint64_t t, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? t / f : 0;
}

/* ---- vblk behind the seam ------------------------------------------- */

static int vblk_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (vblk_a64_read(lba + i, p + (uint64_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static int vblk_bd_write(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf)
{
    (void)ctx;
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (vblk_a64_write(lba + i, p + (uint64_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static uint64_t vblk_bd_sectors(void *ctx)
{
    (void)ctx;
    return vblk_a64_sector_count();
}

static const struct blkdev_ops vblk_bd_ops = {
    .read         = vblk_bd_read,
    .write        = vblk_bd_write,
    .sector_count = vblk_bd_sectors,
};

/* ---- mount + proof --------------------------------------------------- */

/* P4: the dispatcher asks for the mounted ops table (NULL until
 * a64fs_bringup succeeds) -- fsglue_rv.c's mirror. */
static const struct vfs_ops *mounted_ops;

const struct vfs_ops *a64fs_ops(void)
{
    return mounted_ops;
}

void a64fs_bringup(void)
{
    int dev = blkdev_register("vblk0", &vblk_bd_ops, 0, BLKDEV_SECTOR_SIZE);
    if (dev < 0) {
        kprintf("[blkdev] REFUSED vblk0: rc=%d\n", dev);
        return;
    }
    kprintf("[blkdev] blk%d = vblk0 (virtio-mmio, %llu sectors)\n",
            dev, (unsigned long long)blkdev_sector_count(dev));

    if (ext2_init(dev) != 0) {
        kprintf("[a64fs] ext2 mount failed on blkdev %d\n", dev);
        return;
    }
    kprintf("[a64fs] mounted ext2 on blkdev %d (ops-level; fd layer: P4)\n",
            dev);
    mounted_ops = &ext2_ops;

    ext2_self_test();
    ext2_list();

    struct vnode *vn = ext2_ops.lookup(0, "LINUX.TXT");
    if (!vn) {
        kprintf("[a64fs] cat: LINUX.TXT not found\n");
        return;
    }
    char buf[128];
    int64_t n = ext2_ops.read(vn, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        kprintf("[a64fs] cat: read failed\n");
        return;
    }
    buf[n] = '\0';
    for (int64_t i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = ' ';
    kprintf("[a64fs] cat LINUX.TXT (%lld bytes): %s\n",
            (long long)n, buf);
}
