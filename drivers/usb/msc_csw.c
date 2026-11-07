/* msc_csw.c — USB BOT command-status-wrapper validation (RESIDUE3 C5b). */
#include "drivers/usb/msc_csw.h"

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *msc_csw_result_name(msc_csw_result_t r) {
    switch (r) {
    case MSC_CSW_OK:        return "ok";
    case MSC_CSW_SHORT:     return "short CSW";
    case MSC_CSW_LONG:      return "over-long CSW";
    case MSC_CSW_BAD_SIGNATURE: return "bad CSW signature";
    case MSC_CSW_BAD_TAG:       return "CSW tag mismatch";
    case MSC_CSW_BAD_STATUS:    return "CSW status";
    }
    return "unknown";
}

void msc_csw_validate(const uint8_t *buf, int bytes, uint32_t expect_tag,
                      msc_csw_check_t *out) {
    if (!out) return;
    out->result = MSC_CSW_SHORT;
    out->signature = 0;
    out->tag = 0;
    out->residue = 0;
    out->status = 0;

    if (!buf || bytes <= 0) return;                 /* nothing arrived */
    if (bytes < (int)MSC_CSW_LEN) return;           /* short: stop here */
    if (bytes > (int)MSC_CSW_LEN) {
        /* The length is checked before the payload: a 14-byte answer means
         * the device and the host disagree about the phase, and the 13 bytes
         * that follow are not necessarily a CSW. */
        out->result = MSC_CSW_LONG;
        out->signature = le32(buf + 0);
        return;
    }

    out->signature = le32(buf + 0);
    out->tag       = le32(buf + 4);
    out->residue   = le32(buf + 8);
    out->status    = buf[12];

    if (out->signature != MSC_CSW_SIGNATURE) { out->result = MSC_CSW_BAD_SIGNATURE; return; }
    if (out->tag != expect_tag)              { out->result = MSC_CSW_BAD_TAG;       return; }
    if (out->status != 0)                    { out->result = MSC_CSW_BAD_STATUS;    return; }
    out->result = MSC_CSW_OK;
}
