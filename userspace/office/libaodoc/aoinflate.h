/* userspace/office/libaodoc/aoinflate.h — shared raw DEFLATE/zlib inflater.
 *
 * The implementation was first written for the web-view PNG decoder. OF7
 * promotes it into libaodoc so ZIP method 8 and PNG use one bounded decoder.
 * No heap: input and output are caller-owned byte ranges.
 */
#ifndef AURALITE_OFFICE_AOINFLATE_H
#define AURALITE_OFFICE_AOINFLATE_H

#include <stddef.h>
#include <stdint.h>

#define AO_INFLATE_OK       0
#define AO_INFLATE_E_ARG   -1
#define AO_INFLATE_E_FORMAT -2
#define AO_INFLATE_E_CHECK  -3
#define AO_INFLATE_E_FULL  -4

/* Inflate one complete raw DEFLATE stream. `consumed` reports compressed
 * bytes read (including the final byte's padding bits); callers such as ZIP
 * require it to equal the declared compressed size. Empty output is valid. */
int ao_inflate_raw(const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t out_cap,
                   size_t *out_len, size_t *consumed);

/* Inflate one complete RFC 1950 zlib stream, including Adler-32 verification. */
int ao_inflate_zlib(const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

#endif
