#include "drivers/framebuffer/psf.h"
#include "drivers/framebuffer/psf_font.h"
static struct psf_font font;
void psf_init(void) {
    font.width = PSF_FONT_WIDTH;
    font.height = PSF_FONT_HEIGHT;
    font.bytes_per_row = 1;
    font.num_glyphs = PSF_FONT_GLYPHS;
    font.data = psf_font;
}
const struct psf_font *psf_get_font(void) { return &font; }
