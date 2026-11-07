/* msc_csw.h — USB BOT command-status-wrapper validation, as pure data.
 *
 * WHY THIS IS A SEPARATE MODULE (RESIDUE3 C5b).
 *
 * A CSW is exactly 13 bytes (USB Mass Storage Class Bulk-Only Transport 1.0
 * §6.2.3).  msc.c used to zero a packed struct, ask the bulk-IN endpoint for
 * sizeof(struct) and then check the signature -- so a device that answered
 * with 4 bytes produced "bad CSW signature 0x0", which names the symptom one
 * layer below the cause (a short read, not a corrupt signature).  C5 changed
 * that ordering inside msc.c; C5b moves the whole check out here so it can be
 * driven on the host with byte arrays, including the truncated wrappers no
 * QEMU device ever sends.
 *
 * The fields are decoded as little-endian explicitly rather than by memcpy
 * into a packed struct: this kernel also builds for aarch64 and riscv64, and
 * a big-endian port would otherwise read a valid signature as garbage.
 */
#ifndef AURALITE_DRIVERS_USB_MSC_CSW_H
#define AURALITE_DRIVERS_USB_MSC_CSW_H

#include <stdint.h>

#define MSC_CSW_LEN       13u
#define MSC_CSW_SIGNATURE 0x53425355u   /* "USBS" */

typedef enum {
    MSC_CSW_OK = 0,        /* 13 bytes, signature and tag match, status 0 */
    MSC_CSW_SHORT,         /* fewer than 13 bytes: nothing else can be trusted */
    MSC_CSW_LONG,          /* more than 13 bytes: protocol error, BOT 6.2.3 */
    MSC_CSW_BAD_SIGNATURE, /* 13 bytes, wrong dCSWSignature */
    MSC_CSW_BAD_TAG,       /* dCSWTag does not match the CBW tag */
    MSC_CSW_BAD_STATUS,    /* bCSWStatus != 0 (failed or phase error) */
} msc_csw_result_t;

typedef struct {
    msc_csw_result_t result;
    uint32_t signature;    /* decoded, 0 when the buffer was too short */
    uint32_t tag;
    uint32_t residue;      /* dCSWDataResidue, 0 when the buffer was short */
    uint8_t  status;       /* bCSWStatus */
} msc_csw_check_t;

/* Validate a CSW read from the bulk-IN endpoint.  `bytes` is what the transfer
 * actually returned; `expect_tag` is the tag of the CBW just sent.  Never
 * reads past `bytes`. */
void msc_csw_validate(const uint8_t *buf, int bytes, uint32_t expect_tag,
                      msc_csw_check_t *out);

/* Stable, allocation-free name for log messages and tests. */
const char *msc_csw_result_name(msc_csw_result_t r);

#endif /* AURALITE_DRIVERS_USB_MSC_CSW_H */
