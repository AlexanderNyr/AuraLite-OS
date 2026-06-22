#ifndef AURALITE_DRIVERS_FRAMEBUFFER_BOOTSPLASH_H
#define AURALITE_DRIVERS_FRAMEBUFFER_BOOTSPLASH_H

#include <stdint.h>

/* Boot splash screen.
 * Shows an animated AuraLite OS boot logo with a progress bar.
 * Call bootsplash_init() once after gfx_init(), then
 * bootsplash_set_stage(stage, total, message) as subsystems come online.
 * Call bootsplash_finish() when boot is complete to fade out.
 */

void bootsplash_init(void);
void bootsplash_set_stage(uint32_t stage, uint32_t total, const char *message);
void bootsplash_tick(void);  /* animate spinner */
void bootsplash_finish(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_BOOTSPLASH_H */