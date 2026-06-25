#ifndef AURALITE_DRIVERS_AUDIO_PCSPKR_H
#define AURALITE_DRIVERS_AUDIO_PCSPKR_H

#include <stdint.h>

/* PC Speaker audio driver. */

void pcspkr_init(void);
void pcspkr_beep(uint32_t freq, uint32_t duration_ms);

#endif /* AURALITE_DRIVERS_AUDIO_PCSPKR_H */
