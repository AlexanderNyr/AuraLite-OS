/* msi_caps.c — MSI / MSI-X capability parsing and programming.
 * See msi_caps.h for the contract and for why the accessors are injectable.
 *
 * Capability layouts (PCI Local Bus 3.0 §6.8, PCIe 2.0 §7.7):
 *
 *   MSI (id 0x05)
 *     +0  cap id       +1  next
 *     +2  message control  (bit0 EN, bit3 64-bit, bit4 per-vector mask,
 *                           bits 13:11 multiple-message capable,
 *                           bits 9:7  multiple-message enable)
 *     +4  message address low
 *     +8  message address high   (only if 64-bit capable)
 *     +12 message data           (+16 .. if per-vector mask capable)
 *
 *   MSI-X (id 0x11)
 *     +0  cap id       +1  next
 *     +2  message control  (bit15 ENABLE, bit14 FUNCTION MASK,
 *                           bits 10:0 table size - 1)
 *     +4  table BIR (bits 2:0) + table offset (bits 31:3)
 *     +8  PBA  BIR + offset
 *     table entry (16 bytes): addr lo, addr hi, data, vector control (bit0
 *     = mask this vector)
 */

#include <stdint.h>

#include "drivers/pci/msi_caps.h"

static void put_reason(char *why, unsigned n, const char *s) {
    if (!why || n == 0) return;
    unsigned i = 0;
    for (; s[i] && i + 1 < n; i++) why[i] = s[i];
    why[i] = '\0';
}

static void put_u32(char *why, unsigned n, const char *pre, uint32_t v) {
    if (!why || n == 0) return;
    unsigned i = 0;
    char tmp[11];
    int t = 0;
    for (; pre[i] && i + 1 < n; i++) why[i] = pre[i];
    if (v == 0) tmp[t++] = '0';
    while (v && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (t > 0 && i + 1 < n) why[i++] = tmp[--t];
    why[i] = '\0';
}

int msi_scan(const msi_pci_io_t *io, msi_caps_t *out) {
    if (!io || !out || !io->cfg8) return MSI_KIND_NONE;
    out->kind = MSI_KIND_NONE;
    out->cap_off = 0;
    out->n_vectors = 0;
    out->bir = 0;
    out->table_off = 0;
    out->cap64 = 0;

    /* Status bit 4 = capability list present. */
    uint16_t status = 0;
    if (io->cfg16) {
        status = io->cfg16(io->ctx, 0x06);
    } else {
        status = (uint16_t)(io->cfg8(io->ctx, 0x06) |
                            (io->cfg8(io->ctx, 0x07) << 8));
    }
    if (!(status & 0x0010)) return MSI_KIND_NONE;

    uint8_t ptr = (uint8_t)(io->cfg8(io->ctx, 0x34) & 0xFC);
    int fuse = 48;                       /* a malformed list must terminate */
    msi_caps_t msix, msi;
    msix.kind = MSI_KIND_NONE;
    msi.kind  = MSI_KIND_NONE;
    while (ptr && fuse--) {
        uint8_t id  = io->cfg8(io->ctx, ptr);
        uint8_t nxt = (uint8_t)(io->cfg8(io->ctx, ptr + 1u) & 0xFC);
        if (id == PCI_CAP_ID_MSIX && msix.kind == MSI_KIND_NONE) {
            uint16_t mc = io->cfg16 ? io->cfg16(io->ctx, (uint16_t)(ptr + 2u)) : 0;
            uint32_t tbl = io->cfg32 ? io->cfg32(io->ctx, (uint16_t)(ptr + 4u)) : 0;
            msix.kind      = MSI_KIND_MSIX;
            msix.cap_off   = ptr;
            msix.n_vectors = (uint16_t)((mc & 0x07FFu) + 1u);
            msix.bir       = (uint8_t)(tbl & 0x07u);
            msix.table_off = tbl & 0xFFFFFFF8u;
            msix.cap64     = 0;
        } else if (id == PCI_CAP_ID_MSI && msi.kind == MSI_KIND_NONE) {
            uint16_t mc = io->cfg16 ? io->cfg16(io->ctx, (uint16_t)(ptr + 2u)) : 0;
            msi.kind      = MSI_KIND_MSI;
            msi.cap_off   = ptr;
            msi.n_vectors = (uint16_t)(1u << ((mc >> 11) & 0x07u));
            msi.cap64     = (uint8_t)((mc & 0x0008u) ? 1 : 0);
        }
        if (nxt == ptr) break;           /* self-referential list: stop */
        ptr = nxt;
    }

    /* Prefer MSI-X: per-vector masking and a table are what a queue per
     * device needs, and every virtio-pci device that offers both offers
     * MSI-X for a reason. */
    *out = (msix.kind == MSI_KIND_MSIX) ? msix : msi;
    return out->kind;
}

int msi_program(const msi_pci_io_t *io, const msi_caps_t *caps,
                uint8_t vector, uint8_t dest_apic_id,
                char *why, unsigned why_n) {
    if (!io || !caps || caps->kind == MSI_KIND_NONE) {
        put_reason(why, why_n, "no MSI/MSI-X capability");
        return -1;
    }
    if (!io->w16 || !io->w32) {
        put_reason(why, why_n, "transport has no config writer");
        return -3;
    }

    uint32_t msg_addr = MSI_APIC_ADDR_BASE | ((uint32_t)dest_apic_id << 12);

    if (caps->kind == MSI_KIND_MSIX) {
        if (!io->bar_map) {
            put_reason(why, why_n, "transport cannot map the MSI-X table");
            return -3;
        }
        uint32_t len = (uint32_t)caps->n_vectors * 16u;
        if (len < 16u) len = 16u;
        volatile uint8_t *tbl = io->bar_map(io->ctx, caps->bir,
                                           caps->table_off, len);
        if (!tbl) {
            put_u32(why, why_n, "MSI-X table BAR unmappable: BAR", caps->bir);
            return -3;
        }

        volatile uint32_t *e = (volatile uint32_t *)tbl;   /* entry 0 */
        e[0] = msg_addr;          /* address low  */
        e[1] = 0;                 /* address high */
        e[2] = (uint32_t)vector;  /* data         */
        e[3] = 0;                 /* unmask       */

        /* Enable, then clear the function mask, then read it back.  The
         * order matters: enabling with the mask set still delivers
         * nothing, and a device that drops the write must be seen. */
        uint16_t off = (uint16_t)(caps->cap_off + 2u);
        uint16_t mc  = io->cfg16 ? io->cfg16(io->ctx, off) : 0;
        mc |= 0x8000u;            /* MSI-X enable      */
        mc &= (uint16_t)~0x4000u; /* function mask off */
        io->w16(io->ctx, off, mc);
        uint16_t back = io->cfg16 ? io->cfg16(io->ctx, off) : 0;
        if (!(back & 0x8000u)) {
            put_reason(why, why_n, "device refused the MSI-X enable bit");
            return -2;
        }
        if (back & 0x4000u) {
            put_reason(why, why_n, "device kept the MSI-X function mask set");
            return -2;
        }
        return 0;
    }

    /* ---- MSI ---- */
    uint16_t off = (uint16_t)(caps->cap_off + 2u);
    uint16_t mc  = io->cfg16 ? io->cfg16(io->ctx, off) : 0;
    mc &= (uint16_t)~0x0700u;          /* multiple-message enable = 1 msg */
    mc &= (uint16_t)~0x0001u;          /* program while disabled          */
    io->w16(io->ctx, off, mc);

    io->w32(io->ctx, (uint16_t)(caps->cap_off + 4u), msg_addr);
    uint16_t data_off = (uint16_t)(caps->cap_off + 12u);
    if (caps->cap64) {
        io->w32(io->ctx, (uint16_t)(caps->cap_off + 8u), 0);
        data_off = (uint16_t)(caps->cap_off + 12u);
    }
    /* The data field is a WORD in the layout; write it as one. */
    io->w16(io->ctx, data_off, (uint16_t)vector);
    if (mc & 0x0010u) {
        /* Per-vector masking capable: clear every mask bit (one vector, so
         * the low 32 bits of the mask register cover it). */
        io->w32(io->ctx, (uint16_t)(caps->cap_off + 16u), 0);
    }
    mc |= 0x0001u;                     /* enable */
    io->w16(io->ctx, off, mc);
    uint16_t back = io->cfg16 ? io->cfg16(io->ctx, off) : 0;
    if (!(back & 0x0001u)) {
        put_reason(why, why_n, "device refused the MSI enable bit");
        return -2;
    }
    return 0;
}
