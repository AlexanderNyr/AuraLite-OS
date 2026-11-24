/* kernel/fs/datavol.c — FSMODERN M1: /data, the persistent volume.
 *
 * See datavol.h for why.  This file is the life cycle: pick a disk, mount a
 * CoW filesystem on it, prove the mount answers, and say out loud whether
 * the result will survive a power cut.
 *
 * THE DISK CHOICE IS DELIBERATE AND NARROW
 * ----------------------------------------
 * The mount ladder in kernel.c claims disk ids in order: /disk takes 0,
 * /fat 0, /ext2 1, /exfat 2, /ext4 3, /btrfs 4, /f2fs 5, /ntfs 6.  Those
 * experimental mounts only happen when the user attached that many disks,
 * which a default boot does not.  /data therefore takes the LOWEST disk id
 * that no other filesystem claimed -- computed from the live mount table,
 * not from a hardcoded number -- so it works on a 1-disk machine and on a
 * 6-disk machine without a second recipe.
 *
 * WHY CoW FIRST
 * -------------
 * The tree has four writable disk filesystems that could hold /data.  The
 * CoW one is chosen because it is the only one whose durability argument is
 * structural rather than procedural: every write allocates a fresh block,
 * every block carries a SHA-256 trailer verified on read, and the root is
 * re-pointed last.  That is the substrate M6's AuraFS evolves from.  If it
 * is not present, the next candidate is tried and the receipt names which
 * one actually answered -- /data never lies about what is behind it.
 */

#include "kernel/fs/datavol.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/blkdev.h"
#include "kernel/fs/btrfs.h"
#include "kernel/fs/fsformat.h"
#include "kernel/fs/ext4.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

#define DATAVOL_PROBE      DATAVOL_PATH "/.auralite-probe"
#define DATAVOL_PROBE_MSG  "auralite-data-volume"

static const char *g_backing = NULL;
static const char *g_fs_name = NULL;

const char *datavol_backing(void) { return g_backing; }
const char *datavol_fs_name(void) { return g_fs_name; }
int datavol_is_durable(void) { return g_backing != NULL; }

/* Is a path already a mount point of some filesystem?  vfs_stat on an
 * unmounted path simply does not resolve, so this doubles as "is that
 * filesystem mounted". */
static int mount_present(const char *mount) {
    struct vfs_stat st;
    return vfs_stat(mount, &st) == 0 && st.type == VFS_TYPE_DIR;
}

/* Write, read back and remove a probe file.  A mount that cannot do this is
 * not storage, and the difference between "mounted" and "usable" is exactly
 * what a first save discovers the hard way. */
static int probe_writable(void) {
    int fd = vfs_open(DATAVOL_PROBE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return fd;
    int64_t w = vfs_write(fd, DATAVOL_PROBE_MSG, sizeof(DATAVOL_PROBE_MSG) - 1);
    vfs_close(fd);
    if (w != (int64_t)(sizeof(DATAVOL_PROBE_MSG) - 1)) return -EIO;

    char buf[48];
    fd = vfs_open(DATAVOL_PROBE, O_RDONLY, 0);
    if (fd < 0) return fd;
    int64_t r = vfs_read(fd, buf, sizeof buf - 1);
    vfs_close(fd);
    if (r != w) return -EIO;
    buf[r] = '\0';
    if (strcmp(buf, DATAVOL_PROBE_MSG) != 0) return -EIO;

    vfs_unlink(DATAVOL_PROBE);
    return 0;
}

/* Which disk ids are already spoken for, by asking the mount table rather
 * than by counting disks.  A machine with 2 disks but no /ext2 (an
 * unformatted second disk) leaves id 1 free, and /data is allowed to use it. */
static int disk_is_claimed(int dev) {
    /* The mount ladder's assignment, in the order kernel.c performs it.
     * Kept here as data + a live check: a mount point that is present means
     * its device is claimed, and a mount point that is absent means the id
     * was never taken (mounters probe-then-mount, never mount-then-probe). */
    static const struct { int dev; const char *mount; } ladder[] = {
        { 0, "/disk" },
        { 0, "/fat"  },
        { 1, "/ext2" },
        { 3, "/ext4" },
        { 4, "/btrfs"},
        { 5, "/exfat"},   /* FSMODERN M1: moved off the 3rd disk */
        { 6, "/f2fs" },
        { 7, "/ntfs" },
    };
    for (unsigned i = 0; i < sizeof ladder / sizeof ladder[0]; i++) {
        if (ladder[i].dev == dev && mount_present(ladder[i].mount)) return 1;
    }
    return 0;
}

int datavol_init(void) {
    g_backing = NULL;
    g_fs_name = NULL;

    int ndisks = blkdev_count();
    if (ndisks <= 0) {
        kprintf("[data] no block devices at all; /data not mounted "
                "(nothing is durable on this machine)\n");
        return -ENODEV;
    }

    /* Candidates in preference order: the CoW filesystem first, then the two
     * other real filesystems that can hold a directory tree. */
    static const struct {
        const char *name;
        const char *mount;      /* temp mount point used for the probe */
        int (*init)(int);
        const struct vfs_ops *ops;
    } cand[] = {
        { "btrfs(CoW+SHA-256)", "/btrfs", btrfs_init, &btrfs_ops },
        { "ext4",               "/ext4",  ext4_init,  &ext4_ops  },
    };

    for (unsigned c = 0; c < sizeof cand / sizeof cand[0]; c++) {
        /* The candidate may already be mounted by kernel.c's ladder (a user
         * who attached enough disks gets /btrfs for free).  In that case it
         * is already claimed and /data must not pretend to own it. */
        if (mount_present(cand[c].mount)) {
            kprintf("[data] %s is already mounted at %s; leaving it there\n",
                    cand[c].name, cand[c].mount);
            continue;
        }

        for (int dev = 0; dev < ndisks && dev < BLKDEV_MAX; dev++) {
            if (disk_is_claimed(dev)) continue;

            uint64_t sectors = blkdev_sector_count(dev);
            if (sectors == 0) {
                kprintf("[data] blk%d does not report a capacity; refusing to "
                        "put a filesystem on it\n", dev);
                continue;
            }

            /* A device with no superblock of the candidate's kind is
             * formatted ONLY if the safety gate allows it (FSFULL F1).  The
             * gate is off by default, so a blank disk is refused with a
             * sentence a user can act on instead of being overwritten. */
            if (cand[c].init(dev) != 0) {
                kprintf("[data] %s on blk%d: %s\n", cand[c].name, dev,
                        fs_format_allowed()
                            ? "unusable volume; not mounted"
                            : "no readable volume (auto-format is OFF: "
                              "pass -fw_cfg name=opt/auralite.fsformat,"
                              "string=1 or build with FS_MOUNT_FORMAT=1 to "
                              "create one)");
                continue;
            }

            /* Mount it at /data.  The driver keeps its own fs_data; the VFS
             * reaches it through the ops table, exactly as /btrfs would. */
            if (vfs_mount(DATAVOL_PATH, cand[c].ops, NULL) != 0) {
                kprintf("[data] %s on blk%d initialised but did not mount at "
                        "%s\n", cand[c].name, dev, DATAVOL_PATH);
                continue;
            }

            if (probe_writable() != 0) {
                kprintf("[data] %s on blk%d mounted but cannot hold a file; "
                        "refusing it\n", cand[c].name, dev);
                /* Leave it mounted: unmounting a half-proven filesystem is
                 * how state gets lost.  Report and move on. */
                continue;
            }

            g_backing = DATAVOL_PATH;
            g_fs_name = cand[c].name;
            kprintf("[data] PASS: %s mounted at %s on blk%d, %llu MiB, "
                    "write/read/unlink verified -- PERSISTENT\n",
                    cand[c].name, DATAVOL_PATH, dev,
                    (unsigned long long)(sectors / (1024 * 1024 / 512)));
            return 0;
        }
    }

    kprintf("[data] no free disk for a persistent volume; /data not mounted "
            "(attach one: -drive file=data.img,format=raw,if=none,id=d "
            "-device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0)\n");
    return -ENODEV;
}
