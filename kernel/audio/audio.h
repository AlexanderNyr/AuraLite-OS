#ifndef AURALITE_KERNEL_AUDIO_H
#define AURALITE_KERNEL_AUDIO_H

#include <stdint.h>

/* High-level Audio Subsystem core. */

void audio_init(void);
void audio_play_tone(uint32_t freq, uint32_t duration_ms);
void audio_write_buffer(const uint8_t *buf, uint32_t len);

#endif /* AURALITE_KERNEL_AUDIO_H */
