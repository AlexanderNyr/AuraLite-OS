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
#include "kernel/fs/devfs.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/string.h"
#include "kernel/fs/vfsmount.h"
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

/* RESIDUE2 T3 (RES-07): tmpfs.c links krealloc(), which needs the old
 * allocation size.  The flat port allocators do not track it, so every
 * kmalloc here carries an 8-byte size header; kfree/krealloc read it
 * back.  All shared objects in this binary allocate and free through
 * these two names, so the header is consistent everywhere. */
void *kmalloc(uint64_t size)
{
    uint64_t *p = kmalloc_a64((size_t)size + 8);
    if (!p) return 0;
    *p = (uint64_t)size;
    return p + 1;
}

void kfree(void *p)
{
    if (p) kfree_a64((uint64_t *)p - 1);
}

void *krealloc(void *ptr, uint64_t size)
{
    if (!ptr) return kmalloc((uint64_t)size);
    if (size == 0) { kfree(ptr); return 0; }
    uint64_t old = *((uint64_t *)ptr - 1);
    void *np = kmalloc((uint64_t)size);
    if (!np) return 0;
    memcpy(np, ptr, (size_t)(old < size ? old : size));
    kfree(ptr);
    return np;
}

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
                kprintf("[a64fs] RES-07: tmpfs /tmp round-trip OK "
                        "(%lld bytes)\n", (long long)r);
            else
                kprintf("[a64fs] RES-07: tmpfs round-trip FAILED\n");
        } else {
            kprintf("[a64fs] RES-07: tmpfs create FAILED\n");
        }
    }
    int dev = blkdev_register("vblk0", &vblk_bd_ops, 0, BLKDEV_SECTOR_SIZE);
    if (dev < 0) {
        kprintf("[blkdev] REFUSED vblk0: rc=%d\n", dev);
        return;
    }
    kprintf("[blkdev] blk%d = vblk0 (%s, %llu sectors)\n",
            dev, vblk_a64_transport(),
            (unsigned long long)blkdev_sector_count(dev));
    int pk = blkdev_partition_kind(dev);
    if (pk > 0)
        kprintf("[blkdev] blk%d carries a %s partition table; raw "
                "mounts IGNORE it (RES-04)\n", dev,
                pk == BLKDEV_PART_GPT ? "GPT" : "MBR");

    if (ext2_init(dev) != 0) {
        kprintf("[a64fs] ext2 mount failed on blkdev %d\n", dev);
        return;
    }
    kprintf("[a64fs] mounted ext2 on blkdev %d (VFS-mounted since R2)\n",
            dev);
    mounted_ops = &ext2_ops;
    vfsm_mount("/", &ext2_ops, 0);      /* R2: the shared mount table */

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
