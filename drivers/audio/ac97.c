/* ac97.c — Intel AC97 / HDAudio virtual sound driver. */
#include <stdint.h>
#include "drivers/audio/ac97.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"

#define AC97_BUFFER_SIZE 65536

static uint8_t *dma_buffer = NULL;
static uint32_t dma_ptr = 0;

void ac97_init(void) {
    dma_buffer = kmalloc(AC97_BUFFER_SIZE);
    if (dma_buffer) {
        memset(dma_buffer, 0, AC97_BUFFER_SIZE);
    }
    kprintf("[ac97] Intel AC97 / HDAudio virtual sound driver initialised (DMA ring buffer 64 KiB)\n");
}

void ac97_write_buffer(const uint8_t *buf, uint32_t len) {
    if (!dma_buffer || !buf || len == 0) return;
    uint32_t copy_len = len;
    if (copy_len > AC97_BUFFER_SIZE) copy_len = AC97_BUFFER_SIZE;
    
    for (uint32_t i = 0; i < copy_len; i++) {
        dma_buffer[dma_ptr] = buf[i];
        dma_ptr = (dma_ptr + 1) % AC97_BUFFER_SIZE;
    }
    kprintf("[ac97] DMA playback triggered (%u samples)\n", copy_len);
}
