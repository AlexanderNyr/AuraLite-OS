/*
 * wv_image.h — decode a fetched image into XRGB8888 (top-left origin).
 *
 * Formats: PNG (8-bit gray/rgb/palette/rgba, no Adam7), baseline JPEG
 * (SOF0), GIF87a/89a first frame, BMP 24/32-bpp.  The caller frees *px
 * with free().  Same source on the host (unit tests) and in gbrowser.
 */
#ifndef AURALITE_WV_IMAGE_H
#define AURALITE_WV_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define WV_IMAGE_MAX_W 800
#define WV_IMAGE_MAX_H 600

/* 0 on success (*px is w*h XRGB8888, malloc'd).  Negative on refuse. */
int wv_image_decode(const uint8_t *data, size_t n,
                    uint32_t **px, int *w, int *h);

/* Decode a data: URL (data:image/png;base64,...).  0 on success. */
int wv_image_decode_data_url(const char *url,
                             uint32_t **px, int *w, int *h);

#endif
