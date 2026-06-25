/* audio.c — High-level Audio Subsystem core. */
#include <stdint.h>
#include "kernel/audio/audio.h"
#include "drivers/audio/pcspkr.h"
#include "drivers/audio/ac97.h"
#include "kernel/lib/kprintf.h"

void audio_init(void) {
    kprintf("[audio] initialising audio subsystem...\n");
    pcspkr_init();
    ac97_init();
    kprintf("[audio] audio subsystem online (PC Speaker + AC97 backends)\n");
}

void audio_play_tone(uint32_t freq, uint32_t duration_ms) {
    kprintf("[audio] playing tone %u Hz for %u ms\n", freq, duration_ms);
    pcspkr_beep(freq, duration_ms);
}

void audio_write_buffer(const uint8_t *buf, uint32_t len) {
    ac97_write_buffer(buf, len);
}
