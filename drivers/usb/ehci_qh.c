/* ehci_qh.c — EHCI queue-head capability words (RESIDUE3 C5b).
 *
 * Bit positions are from EHCI 1.0 §3.6 (queue head layout) and §4.12
 * (split transactions).  They are written as named shifts rather than
 * literals so the unit test can assert both the value and the position.
 */
#include "drivers/usb/ehci_qh.h"

/* ep_caps1 (Endpoint Characteristics) */
#define QH1_DEVADDR_SHIFT  0     /* bits 0-6   */
#define QH1_DEVADDR_MASK   0x7Fu
#define QH1_EPNUM_SHIFT    8     /* bits 8-11  */
#define QH1_EPNUM_MASK     0x0Fu
#define QH1_EPS_SHIFT      12    /* bits 12-13 */
#define QH1_DTC            (1u << 14)
#define QH1_MPL_SHIFT      16    /* bits 16-26 */
#define QH1_MPL_MASK       0x7FFu

/* ep_caps2 (Endpoint Capabilities) */
#define QH2_SMASK_SHIFT    0     /* bits 0-7   */
#define QH2_CMASK_SHIFT    8     /* bits 8-15  */
#define QH2_HUBADDR_SHIFT  16    /* bits 16-22 */
#define QH2_HUBADDR_MASK   0x7Fu
#define QH2_HUBPORT_SHIFT  23    /* bits 23-29 */
#define QH2_HUBPORT_MASK   0x7Fu
#define QH2_MULT_SHIFT     30    /* bits 30-31 */

/* Complete-split microframes: three consecutive ones, starting after the
 * start-split.  EHCI 1.0 §4.12.3.2 requires at least one C-mask bit and lets a
 * complete-split fall in the following micro-frame; when the start lands late
 * in the frame the mask wraps to bits 0.. (which the controller reads as the
 * next frame's first micro-frames) rather than dropping the C-split entirely.
 * Dropping it was a real bug in the first cut of this function: a start in
 * micro-frame 7 produced an empty C-mask and the endpoint never completed. */
#define QH2_CMASK_RUN      0x07u

int ehci_qh_needs_split(uint8_t speed, uint8_t hub_addr) {
    if (hub_addr == 0) return 0;
    return (speed == EHCI_QH_SPEED_FULL || speed == EHCI_QH_SPEED_LOW) ? 1 : 0;
}

void ehci_qh_build_caps(const ehci_qh_params_t *p, ehci_qh_caps_t *out) {
    if (!p || !out) return;

    uint32_t caps1 = ((uint32_t)(p->dev_addr & QH1_DEVADDR_MASK) << QH1_DEVADDR_SHIFT) |
                     ((uint32_t)(p->ep_num   & QH1_EPNUM_MASK)   << QH1_EPNUM_SHIFT) |
                     ((uint32_t)(p->speed & 0x3u)                << QH1_EPS_SHIFT) |
                     QH1_DTC |
                     ((uint32_t)(p->max_packet & QH1_MPL_MASK)   << QH1_MPL_SHIFT);

    uint8_t mult = p->mult ? p->mult : 1;
    uint8_t s_mask = (uint8_t)(1u << (p->start_microframe & 0x7u));
    uint8_t c_mask = 0;
    uint32_t hub_addr = 0, hub_port = 0;
    int split = ehci_qh_needs_split(p->speed, p->hub_addr);

    if (split) {
        /* Start-splits live in the S-mask; complete-splits follow it, in the
         * three micro-frames after the start, wrapping past bit 7. */
        uint8_t base = (uint8_t)((p->start_microframe + 1u) & 0x7u);
        c_mask = (uint8_t)(((QH2_CMASK_RUN << base) |
                            (QH2_CMASK_RUN >> (8u - base))) & 0xFFu);
        c_mask &= (uint8_t)~s_mask;
        hub_addr = p->hub_addr & QH2_HUBADDR_MASK;
        hub_port = p->hub_port & QH2_HUBPORT_MASK;
    }

    uint32_t caps2 = ((uint32_t)s_mask          << QH2_SMASK_SHIFT) |
                     ((uint32_t)c_mask          << QH2_CMASK_SHIFT) |
                     (hub_addr                  << QH2_HUBADDR_SHIFT) |
                     (hub_port                  << QH2_HUBPORT_SHIFT) |
                     ((uint32_t)(mult & 0x3u)   << QH2_MULT_SHIFT);

    out->ep_caps1 = caps1;
    out->ep_caps2 = caps2;
    out->is_split = split;
    out->s_mask   = s_mask;
    out->c_mask   = c_mask;
}
