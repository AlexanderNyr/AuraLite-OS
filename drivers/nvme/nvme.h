#ifndef AURALITE_DRIVERS_NVME_NVME_H
#define AURALITE_DRIVERS_NVME_NVME_H

#include <stdint.h>

/*
 * NVMe (NVM Express) driver over PCIe — RESIDUE3 C7b, RES-46's storage
 * successor.
 *
 * SCOPE, STATED UP FRONT.  This is a polled, one-command-in-flight driver for
 * a single namespace: an admin queue pair and one I/O queue pair, created
 * explicitly (no Set Features negotiation), no interrupts, no PRP lists, no
 * SGLs, no ZNS/ZASL, no multi-namespace.  Every one of those omissions is
 * visible in the code rather than implied:
 *
 *   - transfers move through a 4 KiB bounce buffer, so a command never needs
 *     a PRP list and the caller's buffer never needs to be physically
 *     contiguous (it says what it costs: one memcpy per 4 KiB);
 *   - completion is a poll on the phase bit with a bounded spin, so a wedged
 *     controller fails loudly instead of hanging the boot;
 *   - only a 512-byte logical block shape is registered with blkdev, because
 *     the seam below it (kernel/fs/blkdev.h) is 512-only by design.  A
 *     controller announcing 4096-byte LBAs is refused BY NAME.
 *
 * The controller's own identity is printed at bring-up (Identify Controller
 * MN/SN/FR), the way the Bluetooth driver quotes its controller's BD_ADDR:
 * a receipt a reader can tell apart from a printf.
 */

/* Probe PCI for an NVMe controller, bring up its queues, read the identity of
 * namespace 1 and register it as a blkdev.
 *
 * Returns 0 on success, negative when no controller is present (-1), when the
 * controller has a shape this driver refuses (-2: non-512-byte LBA), or when
 * bring-up failed (-3).  Never panics and never spins unbounded. */
int nvme_init(void);

/* Number of namespaces registered as blkdevs. */
int nvme_namespace_count(void);

/* Boot self-test: prints the controller identity and reads LBA 0, twice
 * (through the driver and through the blkdev seam), so the two paths are
 * shown to agree.  Silent when no controller was found. */
void nvme_self_test(void);

#endif /* AURALITE_DRIVERS_NVME_NVME_H */
