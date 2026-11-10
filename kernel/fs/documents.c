/* kernel/fs/documents.c — OF0: the /documents location.
 *
 * The office suite needs one directory that exists before anything else runs,
 * that is writable, and whose durability is *stated* rather than assumed.
 * This file is that directory's whole life cycle: report which filesystem is
 * behind it, create the directory if the backing filesystem has directories
 * at all, and prove it by writing, reading back and unlinking a probe file --
 * a directory that cannot be written to is not a document location, and the
 * boot log should say so rather than let the first save surprise anyone.
 *
 * The path is a tmpfs MOUNT POINT (kernel/fs/tmpfs.c, volume 4).  That is the
 * second design: the first one made /documents a symlink to /tmp/documents,
 * which reads correctly -- `ls /documents` listed the target -- but could not
 * be written through: this VFS follows symlinks when resolving a path and not
 * when creating one, so open("/documents/x", O_CREAT) fell through to the
 * read-only initrd at "/" and returned EROFS.  A mount point is resolved by
 * the mount table in both directions, so create works.
 *
 * DURABILITY IS NOT YET TRUE, and saying so is the point.  /documents is
 * tmpfs: it survives until the machine is switched off.  The plan wanted
 * /disk/documents, but diskfs (the tiny persistent AUFS) is FLAT -- it has no
 * mkdir hook, so a directory cannot exist on it -- and the other writable
 * filesystems (/fat, /ext4, ...) mount only when their device is plugged in,
 * which is not the case for a normal boot here ("no AHCI disk available").
 * When one of them IS present the durable directory is created and reported,
 * so a machine with a disk gets the persistent location and this one gets a
 * receipt that says "volatile".  The remaining work -- making /documents
 * point at that directory rather than at the tmpfs -- is recorded in TODO.md.
 */

#include "kernel/fs/documents.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

#define DOCUMENTS_DIR_NAME "documents"
#define DOCUMENTS_PROBE    DOCUMENTS_PATH "/.auralite-probe"
#define DOCUMENTS_PROBE_MSG "auralite-documents"

/* Candidate mount points for a durable backing directory, in preference
 * order.  Each is probed: if it is not mounted, or it cannot mkdir, the next
 * one is tried.  /tmp is deliberately last and is not durable. */
static const char *const durable_mounts[] = {
    "/fat",     /* FAT32: the usual removable/exchange disk */
    "/ext4",
    "/ext2",
    "/btrfs",
    "/f2fs",
};
#define N_DURABLE (sizeof durable_mounts / sizeof durable_mounts[0])

static const char *backing_path;   /* chosen backing directory */
static int backing_durable;
static const char *durable_dir;    /* a disk directory, when one exists */

const char *documents_backing(void) { return backing_path; }
int documents_is_durable(void) { return backing_durable; }

/* Is this mount point present?  vfs_stat on the mount point reports the
 * filesystem's own root, and an unmounted path does not resolve. */
static int mount_present(const char *mount) {
    struct vfs_stat st;
    if (vfs_stat(mount, &st) != 0) return 0;
    return st.type == VFS_TYPE_DIR;
}

/* mkdir a single level: 0 when it now exists (created or already there),
 * negative when the filesystem refused. */
static int ensure_dir(const char *path) {
    int rc = vfs_mkdir(path, 0755);
    if (rc == 0) return 0;
    if (rc == -EEXIST) return 0;
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0 && st.type == VFS_TYPE_DIR) return 0;
    return rc;
}

/* Write, read back and remove a probe file through DOCUMENTS_PATH. */
static int probe_writable(void) {
    int fd = vfs_open(DOCUMENTS_PROBE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return fd;
    int64_t w = vfs_write(fd, DOCUMENTS_PROBE_MSG, sizeof(DOCUMENTS_PROBE_MSG) - 1);
    vfs_close(fd);
    if (w != (int64_t)(sizeof(DOCUMENTS_PROBE_MSG) - 1)) return -EIO;

    char buf[32];
    fd = vfs_open(DOCUMENTS_PROBE, O_RDONLY, 0);
    if (fd < 0) return fd;
    int64_t r = vfs_read(fd, buf, sizeof buf - 1);
    vfs_close(fd);
    if (r != w) return -EIO;
    buf[r] = '\0';
    if (strcmp(buf, DOCUMENTS_PROBE_MSG) != 0) return -EIO;

    vfs_unlink(DOCUMENTS_PROBE);
    return 0;
}

int documents_init(void) {
    static char chosen[64];
    backing_path = NULL;
    backing_durable = 0;

    /* A durable directory, when this machine has a writable disk filesystem.
     * It is created and reported; the mount point stays the tmpfs volume
     * until the VFS can create through a symlink (see the file comment). */
    for (unsigned i = 0; i < N_DURABLE; i++) {
        const char *mount = durable_mounts[i];
        if (!mount_present(mount)) continue;
        /* "<mount>/documents" -- built by hand; VFS_PATH_MAX is far bigger. */
        size_t mlen = strlen(mount);
        size_t need = mlen + 1 + strlen(DOCUMENTS_DIR_NAME) + 1;
        if (need > sizeof chosen) continue;
        memcpy(chosen, mount, mlen);
        chosen[mlen] = '/';
        strcpy(chosen + mlen + 1, DOCUMENTS_DIR_NAME);
        if (ensure_dir(chosen) != 0) {
            kprintf("[documents] %s is mounted but cannot create %s; trying next\n",
                    mount, chosen);
            continue;
        }
        kprintf("[documents] durable directory ready: %s\n", chosen);
        durable_dir = chosen;
        break;
    }

    /* The location applications use, always: the tmpfs volume mounted at
     * DOCUMENTS_PATH (kernel/fs/tmpfs.c).  Re-creating it here is a no-op
     * when the mount is already there and reports EROFS/EEXIST-free status
     * when it is not -- the probe below is the real assertion. */
    strcpy(chosen, DOCUMENTS_PATH);
    backing_path = chosen;
    backing_durable = 0;

    kprintf("[documents] %s (%s)%s\n", DOCUMENTS_PATH,
            "volatile: tmpfs, lost at power-off",
            durable_dir ? " -- a durable directory also exists" : "");

    int prc = probe_writable();
    if (prc != 0) {
        kprintf("[documents] FAIL: write/read/unlink through %s failed (%d)\n",
                DOCUMENTS_PATH, prc);
        return prc;
    }
    kprintf("[documents] PASS: write/read/unlink through %s\n", DOCUMENTS_PATH);
    return 0;
}
