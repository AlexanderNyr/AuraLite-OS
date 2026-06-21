#ifndef AURALITE_LIB_KPRINTF_H
#define AURALITE_LIB_KPRINTF_H

#include <stddef.h>

/*
 * Kernel formatted output. Each character is fanned out to every active
 * console sink (UART serial + framebuffer). Supports:
 *   %s  string            %c  character       %%  literal '%'
 *   %d  signed decimal    %u  unsigned decimal
 *   %x  lowercase hex     %X  uppercase hex   %p  pointer (0x...)
 * Length modifiers l/ll/z/j/t are accepted and treated as 64-bit.
 */

void kprintf(const char *fmt, ...);
void kputchar(char c);
void kputs(const char *s);

#endif /* AURALITE_LIB_KPRINTF_H */
