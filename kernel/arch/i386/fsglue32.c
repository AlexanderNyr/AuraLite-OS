/* fsglue32.c — what the shared fs objects need from the i386 port
 * (PARITY_PLAN.md P7; third glue, same measured surface as
 * fsglue_rv.c / fsglue_a64.c).
 *
 * The adoption set is identical to the DTB tenants' — blkdev.c,
 * ext2.c, kprintf.c, spinlock.c, compiled UNCHANGED with CFLAGS32
 * (which is exactly what P7's width pay-down bought: 32
 * -Wshorten-64-to-32 errors → 0 across kernel/fs).  This file
 * provides the sinks and names; the seam device is the primary
 * SLAVE, because the master is the boot disk whose MBR the ata
 * selftest pins — the same second-disk rule the x86_64 boot has
 * used for /ext2 since forever.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/fs/blkdev.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/devfs.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/string.h"
#include "kernel/fs/vfsmount.h"
#include "kernel/mm/kheap.h"
#include "kernel/arch/i386/kheap32.h"
#include "kernel/arch/i386/vga32.h"
#include "kernel/arch/i386/irq32.h"
#include "kernel/arch/i386/ata32.h"
#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/fsglue32.h"
#include "kernel/lib/kprintf.h"

/* ---- kprintf's three sinks ------------------------------------------ */
/* Same dual console kprintf32 drives: COM1 (the smokes read serial)
 * and the VGA text cell.  klog stays a no-op (no ring on i386). */

#define COM1 0x3F8

void uart_putchar(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;                            /* LSR.THRE */
    outb(COM1, (uint8_t)c);
}

void fb_putchar(char c)   { vga32_putc(c); }
void klog_putchar(char c) { (void)c; }

/* ---- the heap names ext2.c links against ---------------------------- */

/* kheap.h's contract is 64-bit sizes; this port's heap is 32-bit.
 * A >4G request on i386 is a bug upstream -- refuse it honestly
 * instead of truncating it into a small "success". */
void *kmalloc(uint64_t size)
{
    if (size > 0xFFFFFFFFu - 8u)
        return 0;
    uint64_t *p = kmalloc32((size_t)size + 8);
    if (!p) return 0;
    *p = size;                    /* RESIDUE2 T3: krealloc needs old size */
    return p + 1;
}

void kfree(void *p)
{
    if (p) kfree32((uint64_t *)p - 1);
}

/* RESIDUE2 T3 (RES-07): tmpfs.c links krealloc().  The size header
 * above tells us how much of the old buffer survives. */
void *krealloc(void *ptr, uint64_t size)
{
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return 0; }
    if (size > 0xFFFFFFFFu - 8u) return 0;
    uint64_t old = *((uint64_t *)ptr - 1);
    void *np = kmalloc(size);
    if (!np) return 0;
    memcpy(np, ptr, (size_t)(old < size ? old : size));
    kfree(ptr);
    return np;
}

/* ---- timestamps ------------------------------------------------------ */
/* PIT ticks at 100 Hz (the very rate TIMEFIX pinned); seconds. */

uint64_t vfs_now(void)
{
    return pit32_ticks() / 100u;
}

/* ---- ATA behind the seam --------------------------------------------- */

static int ata_bd_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    int drive = (int)(uintptr_t)ctx;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata32_read_drv(drive, (uint32_t)(lba + i),
                           p + (size_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static int ata_bd_write(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf)
{
    int drive = (int)(uintptr_t)ctx;
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata32_write_drv(drive, (uint32_t)(lba + i),
                            p + (size_t)i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    return 0;
}

static uint64_t ata_bd_sectors(void *ctx)
{
    return ata32_drive_sectors((int)(uintptr_t)ctx);
}

static const struct blkdev_ops ata_bd_ops = {
    .read         = ata_bd_read,
    .write        = ata_bd_write,
    .sector_count = ata_bd_sectors,
};

/* ---- mount + proof --------------------------------------------------- */

void fs32_bringup(void)
{
    /* RESIDUE2 T3 (RES-07): the buffer cache backs ext2's I/O on this
     * tenant too (ext2.c routes through fs_read_block/fs_write_block);
     * it must exist before the first mount. */
    bc_init();

    /* RESIDUE2 T3 (RES-07): the portable in-memory filesystems join
     * this tenant — /dev (the shared devfs core: null + zero) and
     * /tmp (the shared writable tmpfs volume).  Same objects x86_64
     * links, compiled UNCHANGED into this port. */
    devfs_init();
    vfsm_mount("/dev", &devfs_ops, 0);
    tmpfs_init();
    vfsm_mount("/tmp", &tmpfs_ops, tmpfs_volume_tmp());
    /* Adoption proof: one direct ops round-trip on the shared tmpfs
     * object, exactly the way x86_64's tmpfs_self_test starts. */
    {
        struct vnode *tv = tmpfs_ops.create(tmpfs_volume_tmp(), "res07.txt");
        static const char msg[] = "res07-port-tmpfs";
        if (tv) {
            int64_t w = tmpfs_ops.write(tv, 0, msg, sizeof(msg) - 1);
            char rb[32];
            int64_t r = tmpfs_ops.read(tv, 0, rb, sizeof(rb));
            if (w == (int64_t)(sizeof(msg) - 1) && r == w &&
                memcmp(rb, msg, (size_t)r) == 0)
                kprintf("[fs32] RES-07: tmpfs /tmp round-trip OK "
                        "(%lld bytes)\n", (long long)r);
            else
                kprintf("[fs32] RES-07: tmpfs round-trip FAILED\n");
        } else {
            kprintf("[fs32] RES-07: tmpfs create FAILED\n");
        }
    }

    int d0 = blkdev_register("ata0", &ata_bd_ops, (void *)0,
                             BLKDEV_SECTOR_SIZE);
    kprintf("[blkdev] blk%d = ata0 (primary master, %u sectors)\n",
            d0, ata32_drive_sectors(0));

    if (ata32_drive_sectors(1) == 0) {
        kprintf("[fs32] no second disk; /ext2 not mounted "
                "(attach a primary-slave drive to enable)\n");
        return;
    }
    int d1 = blkdev_register("ata1", &ata_bd_ops, (void *)1,
                             BLKDEV_SECTOR_SIZE);
    kprintf("[blkdev] blk%d = ata1 (primary slave, %u sectors)\n",
            d1, ata32_drive_sectors(1));
    int pk = blkdev_partition_kind(d1);
    if (pk > 0)
        kprintf("[blkdev] blk%d carries a %s partition table; raw "
                "mounts IGNORE it (RES-04)\n", d1,
                pk == BLKDEV_PART_GPT ? "GPT" : "MBR");

    /* ext2_init(-1): the shared picker prefers the SECOND device
     * when more than one is registered -- the x86_64 rule, verbatim,
     * now running in 32-bit code nobody edited. */
    if (ext2_init(-1) != 0) {
        kprintf("[fs32] ext2 mount failed on blkdev %d\n", d1);
        return;
    }
    kprintf("[fs32] mounted ext2 on blkdev %d (VFS-mounted since R2)\n", d1);
    /* R2 (RES-08): the shared mount table -- same object, third
     * width; the shell's open() resolves through it. */
    vfsm_mount("/ext2", &ext2_ops, 0);

    ext2_self_test();
    ext2_list();

    struct vnode *vn = ext2_ops.lookup(0, "LINUX.TXT");
    if (!vn) {
        kprintf("[fs32] cat: LINUX.TXT not found\n");
        return;
    }
    char buf[128];
    int64_t n = ext2_ops.read(vn, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        kprintf("[fs32] cat: read failed\n");
        return;
    }
    buf[n] = '\0';
    for (int64_t i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = ' ';
    kprintf("[fs32] cat LINUX.TXT (%lld bytes): %s\n", (long long)n, buf);
}
