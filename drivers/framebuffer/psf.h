#ifndef AURALITE_DRIVERS_FRAMEBUFFER_PSF_H
#define AURALITE_DRIVERS_FRAMEBUFFER_PSF_H

#include <stdint.h>

/*
 * PSF (PC Screen Font) font support.
 * Provides access to the embedded lat0-16.psf 8x16 font.
 * Rendering is done by the framebuffer console (fb.c).
 */

struct psf_font {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t num_glyphs;
    const uint8_t *data;
};

void psf_init(void);
const struct psf_font *psf_get_font(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_PSF_H */
