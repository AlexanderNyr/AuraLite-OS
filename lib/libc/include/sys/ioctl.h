#ifndef AURALITE_LIBC_SYS_IOCTL_H
#define AURALITE_LIBC_SYS_IOCTL_H

/*
 * sys/ioctl.h — device control (POSIX/Linux subset for the TTY).
 * Values match kernel/tty/termios.h.
 */

#include <stdint.h>

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

int ioctl(int fd, unsigned long request, void *arg);

#endif /* AURALITE_LIBC_SYS_IOCTL_H */
