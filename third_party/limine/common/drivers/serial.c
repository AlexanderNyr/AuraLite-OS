#if defined (BIOS)

#include <stdint.h>
#include <stdbool.h>
#include <lib/misc.h>
#include <drivers/serial.h>
#include <sys/cpu.h>

static bool serial_initialised = false;
uint32_t serial_baudrate;

static void serial_initialise(void) {
    if (serial_initialised || !serial) {
        return;
    }

    // Init com1
    outb(0x3f8 + 3, 0x00);
    outb(0x3f8 + 1, 0x00);
    outb(0x3f8 + 3, 0x80);

    uint16_t divisor = (uint16_t)(115200 / serial_baudrate);
    outb(0x3f8 + 0, divisor & 0xff);
    outb(0x3f8 + 1, (divisor >> 8) & 0xff);

    outb(0x3f8 + 3, 0x03);
    outb(0x3f8 + 2, 0xc7);
    outb(0x3f8 + 4, 0x0b);

    serial_initialised = true;
}

void serial_out(uint8_t b) {
    serial_initialise();

    while ((inb(0x3f8 + 5) & 0x20) == 0);
    outb(0x3f8, b);
}

int serial_in(void) {
    serial_initialise();

    if ((inb(0x3f8 + 5) & 0x01) == 0) {
        return -1;
    }
    return inb(0x3f8);
}

#endif
