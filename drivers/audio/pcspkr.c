/* pcspkr.c — PC Speaker audio driver. */
#include <stdint.h>
#include "drivers/audio/pcspkr.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/lib/kprintf.h"
#include "drivers/timer/pit.h"

void pcspkr_init(void) {
    kprintf("[pcspkr] PC Speaker audio driver initialised (port 0x61 / PIT ch 2)\n");
}

void pcspkr_beep(uint32_t freq, uint32_t duration_ms) {
    if (freq == 0) {
        /* Silence */
        uint8_t tmp = inb(0x61) & 0xFC;
        outb(0x61, tmp);
        if (duration_ms > 0) timer_sleep_ms(duration_ms);
        return;
    }

    uint32_t div = 1193180 / freq;
    outb(0x43, 0xB6); /* Set command byte: PIT ch 2, square wave */
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)(div >> 8));

    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3);
    }

    if (duration_ms > 0) {
        timer_sleep_ms(duration_ms);
        tmp = inb(0x61) & 0xFC;
        outb(0x61, tmp);
    }
}
