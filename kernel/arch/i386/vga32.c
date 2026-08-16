/* kernel/arch/i386/vga32.c -- VGA text-mode console (I386_PLAN I7). */

#include <stdint.h>

#include "kernel/arch/i386/vga32.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/portio.h"

#define VGA_PHYS   0xB8000u
#define VGA_COLS   80
#define VGA_ROWS   25
#define VGA_ATTR   0x07        /* light grey on black */

static volatile uint16_t *vram;
static int col, row;
static int active;

static void move_cursor(void)
{
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static void scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vram[(r - 1) * VGA_COLS + c] = vram[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        vram[(VGA_ROWS - 1) * VGA_COLS + c] = (VGA_ATTR << 8) | ' ';
    row = VGA_ROWS - 1;
}

void vga32_init(void)
{
    vram = (volatile uint16_t *)p2v_32(VGA_PHYS);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        vram[i] = (VGA_ATTR << 8) | ' ';
    col = row = 0;
    move_cursor();
    active = 1;
}

int vga32_active(void)
{
    return active;
}

void vga32_putc(char c)
{
    if (!active)
        return;

    switch (c) {
    case '\n':
        col = 0;
        if (++row >= VGA_ROWS)
            scroll();
        break;
    case '\r':
        col = 0;
        break;
    case '\b':
        if (col > 0) {
            col--;
            vram[row * VGA_COLS + col] = (VGA_ATTR << 8) | ' ';
        }
        break;
    case '\t':
        col = (col + 8) & ~7;
        if (col >= VGA_COLS) {
            col = 0;
            if (++row >= VGA_ROWS)
                scroll();
        }
        break;
    default:
        vram[row * VGA_COLS + col] = (uint16_t)((VGA_ATTR << 8) | (uint8_t)c);
        if (++col >= VGA_COLS) {
            col = 0;
            if (++row >= VGA_ROWS)
                scroll();
        }
        break;
    }
    move_cursor();
}
