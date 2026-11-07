/* kernel/arch/x86_64/msi.c — the local-APIC half of MSI/MSI-X (RES-36).
 * See msi.h for the vector map and for why the allocator is a block.
 *
 * EOI discipline: a message-signalled interrupt is an LAPIC interrupt like
 * any other, so its handler owes the LAPIC an EOI.  It is written HERE, after
 * the device handler ran, and not in irq_dispatch's early-EOI path: that path
 * belongs to the IOAPIC/PIC lines, where the early EOI is deliberate (it
 * reopens the line before the handler finishes so a second edge is not
 * lost).  A per-vector EOI for the MSI block is the same rule the IOAPIC
 * lines get, applied to the vectors that have no IOAPIC redirection entry.
 */

#include <stdint.h>

#include "kernel/arch/x86_64/msi.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/lib/kprintf.h"

struct msi_slot {
    int      used;
    void   (*fn)(void *cookie);
    void    *cookie;
    const char *name;
    uint32_t delivered;          /* interrupts this vector has taken */
};

static struct msi_slot slots[MSI_VECTOR_COUNT];
static uint32_t next_slot;
static uint32_t total_delivered;
static int      msi_on = 1;      /* build default until the knob says otherwise */

int msi_enabled(void) { return msi_on; }

void msi_set_enabled(int on, const char *source) {
    msi_on = on ? 1 : 0;
    if (!msi_on) {
        /* Say it once, at boot, naming who asked: a lane that expects INTx
         * needs to be able to tell "the knob worked" from "there was never a
         * capability here". */
        kprintf("[msi] disabled by %s -- devices keep their INTx/polling "
                "path\n", source ? source : "(unknown source)");
    }
}

int msi_alloc_vector(void (*fn)(void *cookie), void *cookie, const char *name) {
    if (!msi_on || !fn) return -1;
    while (next_slot < MSI_VECTOR_COUNT && slots[next_slot].used) next_slot++;
    if (next_slot >= MSI_VECTOR_COUNT) return -1;
    uint32_t i = next_slot++;
    slots[i].used   = 1;
    slots[i].fn     = fn;
    slots[i].cookie = cookie;
    slots[i].name   = name;
    slots[i].delivered = 0;
    return (int)(MSI_VECTOR_BASE + i);
}

int msi_dispatch(uint8_t vector) {
    if (vector < MSI_VECTOR_BASE ||
        vector >= MSI_VECTOR_BASE + MSI_VECTOR_COUNT)
        return 0;
    uint32_t i = (uint32_t)(vector - MSI_VECTOR_BASE);
    struct msi_slot *s = &slots[i];
    if (s->used && s->fn) {
        s->fn(s->cookie);
        s->delivered++;
        total_delivered++;
    }
    /* EOI whether or not a handler was bound: an unclaimed vector in this
     * block is a device we programmed and then forgot, and leaving the ISR
     * in service would wedge every vector of lower priority on this CPU. */
    lapic_eoi();
    return 1;
}

int msi_attach_pci(const msi_pci_io_t *io, void (*fn)(void *cookie),
                   void *cookie, const char *name, uint32_t *vector_out) {
    if (!msi_on) return -4;

    msi_caps_t caps;
    int kind = msi_scan(io, &caps);
    if (kind == MSI_KIND_NONE) {
        /* No capability: this is the INTx case, and it is not an error. */
        return -1;
    }

    int vec = msi_alloc_vector(fn, cookie, name);
    if (vec < 0) {
        kprintf("[msi] %s: vector block exhausted -- keeping INTx\n", name);
        return -4;
    }

    /* Destination = this CPU's APIC id.  A device may name any CPU; naming
     * the one that owns the driver keeps the handler cache-hot and, more
     * importantly, keeps the EOI on the same LAPIC that took the interrupt. */
    uint8_t dest = (uint8_t)lapic_read_id();

    char why[64];
    why[0] = '\0';
    int rc = msi_program(io, &caps, (uint8_t)vec, dest, why, sizeof(why));
    if (rc != 0) {
        /* The vector stays reserved (it was never handed out, and reusing it
         * would hand the same number to a second device while the first one
         * may still be partially programmed).  The device keeps INTx. */
        kprintf("[msi] %s: %s refused (%s) -- keeping INTx\n",
                name, (kind == MSI_KIND_MSIX) ? "MSI-X" : "MSI",
                why[0] ? why : "no reason given");
        return -2;
    }

    if (vector_out) *vector_out = (uint32_t)vec;

    /* The receipt the plan names.  "1 msg" is the multiple-message count we
     * programmed (one message, one vector); the table size is printed because
     * a device that offers 2 entries but needs 16 tells you its queue shape
     * at a glance. */
    kprintf("[msi] %s: %s vector %u (1 msg, %u entr%s)\n",
            name, (kind == MSI_KIND_MSIX) ? "MSI-X" : "MSI",
            (unsigned)vec, (unsigned)caps.n_vectors,
            (caps.n_vectors == 1) ? "y" : "ies");
    return 0;
}

uint32_t msi_vectors_allocated(void) { return next_slot; }
uint32_t msi_interrupts_delivered(void) { return total_delivered; }

/* Per-device delivered counter, for the drivers' own receipts. */
uint32_t msi_vector_delivered(uint32_t vector) {
    if (vector < MSI_VECTOR_BASE || vector >= MSI_VECTOR_BASE + MSI_VECTOR_COUNT)
        return 0;
    return slots[vector - MSI_VECTOR_BASE].delivered;
}
