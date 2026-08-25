/*
 * wv_inflate.h — DEFLATE (RFC 1951) + zlib wrapper (RFC 1950).
 *
 * Written for PNG IDAT.  No heap: the caller supplies the output buffer.
 * The same file is host-tested and compiled into gbrowser.
 */
#ifndef AURALITE_WV_INFLATE_H
#define AURALITE_WV_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Inflate a raw DEFLATE stream into out[0..out_cap).  Returns the number
 * of bytes written, or 0 on error. */
size_t wv_inflate_raw(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap);

/* Inflate a zlib-wrapped stream (CMF/FLG + DEFLATE [+ ADLER32]). */
size_t wv_inflate_zlib(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_cap);

#endif
