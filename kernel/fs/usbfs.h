#ifndef AURALITE_FS_USBFS_H
#define AURALITE_FS_USBFS_H

#include "kernel/fs/vfs.h"

/* usbfs — small VFS view of the currently active USB Mass Storage device.
 * Mounted at /usb.  It is intentionally policy-light: it exposes a readable
 * info file plus raw sector views so hotplug media can be inspected even before
 * a full FAT/ext2 automounter is selected.
 */

void usbfs_init(void);
void usbfs_notify_attach(void);
void usbfs_notify_detach(void);

extern const struct vfs_ops usbfs_ops;

#endif /* AURALITE_FS_USBFS_H */
