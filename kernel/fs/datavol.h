/* kernel/fs/datavol.h — FSMODERN M1: /data, the persistent volume.
 *
 * Why this exists, in one sentence: before M1 this OS had no filesystem that
 * was BOTH mounted on a default boot AND survived a reboot.  `make run`
 * attaches two AHCI disks and gets /disk (a flat 8-slot store), /fat and
 * /ext2 — but everything a user would call storage (/home, /opt,
 * /documents) is tmpfs, and the tree's most interesting filesystems
 * (/btrfs, /ext4, /f2fs) mount only as the 4th/5th/6th disk, which no
 * default boot attaches.  The best work was unreachable and the important
 * directories were volatile, which is the inversion FSMODERN_PLAN §1.4
 * names.
 *
 * M1 mounts ONE CoW filesystem as `/data` on a disk of its own, in the
 * default recipe, and proves it with a write-reboot-read-back test.  The
 * choice is the CoW one (btrfs-style: every block copy-on-write, every block
 * checksummed) because that is the substrate M6's AuraFS evolves from —
 * making a different filesystem durable first would mean proving durability
 * twice.
 */

#ifndef AURALITE_FS_DATAVOL_H
#define AURALITE_FS_DATAVOL_H

/* The stable path.  Applications must use this, never a device name. */
#define DATAVOL_PATH "/data"

/* Mount the persistent volume.  Returns 0 on success, a negative errno
 * otherwise.  Called from kmain AFTER every bulk filesystem has had its
 * chance to claim a disk id and BEFORE /home, so a future home can live on
 * it.  Never formats a device it cannot identify (FSFULL F1's rule);
 * reports exactly which of the three states it found:
 *   - no spare disk        -> /data not mounted
 *   - a volume it can read -> mounted
 *   - a foreign/unreadable -> refused, by name, and nothing written
 */
int datavol_init(void);

/* The backing mount point and the filesystem's user-visible name, or NULL
 * before a successful init.  Used by the boot receipt and by tests. */
const char *datavol_backing(void);
const char *datavol_fs_name(void);

/* Non-zero when DATAVOL_PATH survives a reboot.  This is the whole point of
 * the module: it is 1 only when a real on-disk filesystem is mounted, and 0
 * when /data is absent (never a "probably fine"). */
int datavol_is_durable(void);

#endif /* AURALITE_FS_DATAVOL_H */
