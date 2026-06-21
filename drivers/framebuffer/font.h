#ifndef AURALITE_DRIVERS_FRAMEBUFFER_FONT_H
#define AURALITE_DRIVERS_FRAMEBUFFER_FONT_H

/*
 * 8x8 monochrome bitmap font (Public Domain, font8x8 by Daniel Hepper, based on
 * the IBM public-domain VGA font). The data table lives in font8x8_basic.h and
 * is included exactly once (in font.c) to avoid multiple-definition errors.
 *
 * Each glyph is 8 rows of 8 pixels; within a row byte, bit x (1 << x) selects
 * the pixel at column x, with x=0 being the leftmost pixel.
 */

extern char font8x8_basic[128][8];

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_FONT_H */
