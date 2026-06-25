#ifndef AURALITE_DRIVERS_AUDIO_AC97_H
#define AURALITE_DRIVERS_AUDIO_AC97_H

#include <stdint.h>

/* Intel AC97 / HDAudio virtual sound driver. */

void ac97_init(void);
void ac97_write_buffer(const uint8_t *buf, uint32_t len);

#endif /* AURALITE_DRIVERS_AUDIO_AC97_H */
