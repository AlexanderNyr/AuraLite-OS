/* fsformat.h — auto-format gate for the experimental filesystems
 * (FSFULL_PLAN.md F1).
 *
 * Three of the five experimental drivers (ext4, f2fs, btrfs) used to
 * auto-format any device whose superblock was absent or foreign — a disk
 * containing user data that is not that filesystem was overwritten at
 * boot, with no confirmation and no refusal mode.  This gate makes
 * formatting explicit:
 *
 *   1. QEMU fw_cfg: -fw_cfg name=opt/auralite.fsformat,string=1
 *      (kernel/arch/x86_64/fwcfg.c — the same channel as the selftest
 *      knob, OPT O2), which is how the integration cases drive the
 *      format lane.
 *   2. The build default: make FS_MOUNT_FORMAT=1, the SELFTEST/KEYMAP
 *      precedent — which is also all real hardware gets.
 *
 * Default: OFF.  A foreign or unreadable superblock is refused loudly
 * and not a single sector is written.  See FSFULL_PLAN.md F1 and D3.
 */
#ifndef KERNEL_FS_FSFORMAT_H
#define KERNEL_FS_FSFORMAT_H

/* Build-time default.  `make FS_MOUNT_FORMAT=1` defines this to 1 via
 * the Makefile; absent that, the safe value (refuse) stands. */
#ifndef FS_MOUNT_FORMAT_DEFAULT
#define FS_MOUNT_FORMAT_DEFAULT 0
#endif

/* May a filesystem driver auto-format a device whose signature is absent
 * or foreign? */
int  fs_format_allowed(void);

/* Override the gate (fw_cfg probe).  `source` names the override, e.g.
 * "fw_cfg"; NULL restores the "build" label. */
void fs_format_set(int allowed, const char *source);

/* Where the current value came from: "build" or the override source. */
const char *fs_format_source(void);

#endif /* KERNEL_FS_FSFORMAT_H */
