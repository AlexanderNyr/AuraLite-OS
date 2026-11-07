/* msi_caps.h — MSI / MSI-X capability parsing and programming
 * (RESIDUE3 C4, ledger RES-36).
 *
 * The logic lives here, in the portable driver tree, and touches the device
 * ONLY through a small accessor table: config space is whatever the transport
 * speaks (the x86_64 virtio-blk path uses port I/O through `pci_config_read`;
 * the modern virtio-pci path uses ECAM), and the MSI-X table is a BAR the
 * caller maps.  That is what makes the whole thing host-testable
 * (tests/unit/test_msi_caps.c drives it against a synthetic config space),
 * which matters because a capability walker that is only ever run on QEMU is
 * a walker whose edge cases nobody has seen.
 *
 * Scope, measured on QEMU (2026-11-03):
 *   - cap id 0x05 = MSI, 0x11 = MSI-X.  MSI-X is preferred when both exist.
 *   - the message address for the local APIC is 0xFEE00000 | (dest << 12);
 *     the message data is the vector.  No DMAR/IRQ-remapping is enabled on
 *     this tree, so the classic encoding is the correct one.
 *   - every write is READ BACK: a device that refuses the enable bit is
 *     reported, not assumed to have taken it.
 */
#ifndef AURALITE_DRIVERS_PCI_MSI_CAPS_H
#define AURALITE_DRIVERS_PCI_MSI_CAPS_H

#include <stdint.h>

#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_MSIX  0x11

#define MSI_KIND_NONE  0
#define MSI_KIND_MSI   1
#define MSI_KIND_MSIX  2

/* The local APIC's message address, and the shape of a message. */
#define MSI_APIC_ADDR_BASE 0xFEE00000u

/* The caller supplies everything that is transport-specific.  Any of the
 * writers may be NULL on a read-only transport; programming then fails
 * loudly instead of silently doing nothing. */
typedef struct {
    void *ctx;
    uint8_t  (*cfg8) (void *ctx, uint16_t off);
    uint16_t (*cfg16)(void *ctx, uint16_t off);
    uint32_t (*cfg32)(void *ctx, uint16_t off);
    void     (*w16)  (void *ctx, uint16_t off, uint16_t v);
    void     (*w32)  (void *ctx, uint16_t off, uint32_t v);
    /* Map [bar:off, bar:off+len) into CPU-addressable memory.  0 = cannot. */
    volatile uint8_t *(*bar_map)(void *ctx, uint8_t bar, uint32_t off,
                                 uint32_t len);
} msi_pci_io_t;

typedef struct {
    int      kind;          /* MSI_KIND_*                                     */
    uint8_t  cap_off;       /* config-space offset of the capability          */
    uint16_t n_vectors;     /* MSI-X: table entries; MSI: 1 << MMC            */
    uint8_t  bir;           /* MSI-X: BAR indicator of the table              */
    uint32_t table_off;     /* MSI-X: table offset inside that BAR            */
    uint8_t  cap64;         /* MSI: 64-bit address capable                    */
} msi_caps_t;

/* Walk the capability list and describe what the device offers.
 * Returns MSI_KIND_MSIX / MSI_KIND_MSI / MSI_KIND_NONE. */
int msi_scan(const msi_pci_io_t *io, msi_caps_t *out);

/* Program vector @vector to be delivered to APIC id @dest for the device
 * described by @caps.  Returns 0 on success (and only after reading the
 * enable bit back), -1 when there is no capability, -2 when the device
 * refused, -3 when the transport cannot write or map.  @why receives a
 * one-line reason on failure (may be NULL). */
int msi_program(const msi_pci_io_t *io, const msi_caps_t *caps,
                uint8_t vector, uint8_t dest_apic_id,
                char *why, unsigned why_n);

#endif /* AURALITE_DRIVERS_PCI_MSI_CAPS_H */
