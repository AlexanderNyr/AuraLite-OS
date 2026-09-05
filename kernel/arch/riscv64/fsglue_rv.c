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
#include "kernel/fs/devfs.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/string.h"
#include "kernel/fs/vfsmount.h"
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

/* RESIDUE2 T3 (RES-07): tmpfs.c links krealloc(), which needs the old
 * allocation size.  The flat port allocators do not track it, so every
 * kmalloc here carries an 8-byte size header; kfree/krealloc read it
 * back.  All shared objects in this binary allocate and free through
 * these two names, so the header is consistent everywhere. */
void *kmalloc(uint64_t size)
{
    uint64_t *p = kmalloc_rv((size_t)size + 8);
    if (!p) return 0;
    *p = (uint64_t)size;
    return p + 1;
}

void kfree(void *p)
{
    if (p) kfree_rv((uint64_t *)p - 1);
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
                kprintf("[rvfs] RES-07: tmpfs /tmp round-trip OK "
                        "(%lld bytes)\n", (long long)r);
            else
                kprintf("[rvfs] RES-07: tmpfs round-trip FAILED\n");
        } else {
            kprintf("[rvfs] RES-07: tmpfs create FAILED\n");
        }
    }
    int dev = blkdev_register("vblk0", &vblk_bd_ops, 0, BLKDEV_SECTOR_SIZE);
    if (dev < 0) {
        kprintf("[blkdev] REFUSED vblk0: rc=%d\n", dev);
        return;
    }
    kprintf("[blkdev] blk%d = vblk0 (%s, %llu sectors)\n",
            dev, vblk_rv_transport(),
            (unsigned long long)blkdev_sector_count(dev));
    int pk = blkdev_partition_kind(dev);
    if (pk > 0)
        kprintf("[blkdev] blk%d carries a %s partition table; raw "
                "mounts IGNORE it (RES-04)\n", dev,
                pk == BLKDEV_PART_GPT ? "GPT" : "MBR");

    if (ext2_init(dev) != 0) {
        kprintf("[rvfs] ext2 mount failed on blkdev %d\n", dev);
        return;
    }
    kprintf("[rvfs] mounted ext2 on blkdev %d (VFS-mounted since R2)\n",
            dev);
    mounted_ops = &ext2_ops;
    /* R2 (RES-09): the REAL mount table -- vfsmount.c is the same
     * object x86_64's vfs.c delegates to; the '[vfs] mounted' line
     * below is printed by shared code, not an arch imitation. */
    vfsm_mount("/", &ext2_ops, 0);

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
