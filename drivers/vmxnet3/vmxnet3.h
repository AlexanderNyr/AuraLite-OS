#ifndef AURALITE_DRIVERS_VMXNET3_H
#define AURALITE_DRIVERS_VMXNET3_H

/*
 * vmxnet3 — VMware paravirtual network adapter (RESIDUE2 T6, RES-46).
 *
 * A vmxnet3 revision-1 data-path driver: one TX queue + one RX queue
 * over shared descriptor rings, INTx interrupt with auto-mask, and the
 * same netdev/sw-queue contract as e1000 (including the RESIDUE2 T5
 * idle-drain + blocking-recv hooks).  The register/queue layout follows
 * Linux's drivers/net/vmxnet3/vmxnet3_defs.h (the device-side spec).
 */

int vmxnet3_init(void);
void vmxnet3_register_netdev(void);

#endif
