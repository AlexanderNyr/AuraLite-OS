/* kernel/arch/i386/kbd32.h -- PS/2 keyboard + console input queue
 * (I386_PLAN I7).
 *
 * Two producers, one queue: the PS/2 keyboard (IRQ 1, scancode set 1
 * translated to ASCII) and the polled UART RX (drained opportunistically
 * from the read path).  One consumer: SYS_READ on fd 0, which blocks
 * the calling thread's Ring 3 syscall by hlt-waiting in kernel context
 * until a line is available -- cooked mode, echo on, backspace honoured.
 * Raw mode and termios arrive with the full tty port (plan §6 residue);
 * a shell needs cooked lines, so cooked lines are what I7 ships.
 */

#ifndef AURALITE_ARCH_I386_KBD32_H
#define AURALITE_ARCH_I386_KBD32_H

#include <stdint.h>

void kbd32_init(void);

/* Non-blocking: -1 when the queue is empty, else the next byte. */
int  cons32_getc(void);

/* Blocking cooked-line read; echoes, handles backspace, returns the
 * number of bytes written to buf (newline included when it fits). */
uint32_t cons32_readline(char *buf, uint32_t cap);

#endif /* AURALITE_ARCH_I386_KBD32_H */
