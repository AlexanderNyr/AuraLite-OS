/* Compatibility names for the web-view image decoder.  The implementation
 * lives in libaodoc/aoinflate.c so ZIP method 8 and PNG share one inflater. */
#include "wv_inflate.h"
#include "../../office/libaodoc/aoinflate.h"

size_t wv_inflate_raw(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap) {
    size_t n = 0, consumed = 0;
    if (ao_inflate_raw(in, in_len, out, out_cap, &n, &consumed) != AO_INFLATE_OK)
        return 0;
    return n;
}

size_t wv_inflate_zlib(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap) {
    size_t n = 0;
    if (ao_inflate_zlib(in, in_len, out, out_cap, &n) != AO_INFLATE_OK)
        return 0;
    return n;
}
