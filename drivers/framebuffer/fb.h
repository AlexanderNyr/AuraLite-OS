#ifndef AURALITE_DRIVERS_FRAMEBUFFER_FB_H
#define AURALITE_DRIVERS_FRAMEBUFFER_FB_H

#include <stdint.h>

/*
 * Linear framebuffer console. Limine hands us a VBE/GOP framebuffer; since that
 * is what is actually displayed on screen (not the legacy 0xB8000 text buffer),
 * we render text into it ourselves with the 8x8 font. This supersedes classic
 * VGA text mode for the Limine boot path and underpins the GUI phase later.
 *
 * Only 32-bpp RGB framebuffers are supported for now; fb_putchar becomes a
 * no-op if no suitable framebuffer was provided.
 */

void fb_init(void);
void fb_putchar(char c);
void fb_clear(void);

/* Disable / enable framebuffer console output (UART logging continues).
 * The GUI compositor owns the framebuffer once it starts, so the console
 * stops scribbling on top of windows. */
void fb_set_console_enabled(int on);
int  fb_console_enabled(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_FB_H */
