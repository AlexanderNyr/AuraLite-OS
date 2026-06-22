#ifndef AURALITE_DRIVERS_VM_VIRTUAL_DRIVERS_H
#define AURALITE_DRIVERS_VM_VIRTUAL_DRIVERS_H

/* Probe and report virtual hardware commonly exposed by QEMU, VirtualBox and
 * VMware. This is a compatibility registry: devices with an implemented data
 * path are reported as active, while known-but-not-yet-implemented devices are
 * reported with a clear status so VM settings can be corrected quickly. */
void virtual_drivers_init(void);

#endif /* AURALITE_DRIVERS_VM_VIRTUAL_DRIVERS_H */
