/*
 * termios.h — POSIX.1-2017 terminal attributes (kernel side).
 *
 * Layout and numeric values follow the Linux x86_64 (asm-generic) ABI so the
 * kernel `struct termios` matches what libc passes through SYS_IOCTL.
 * NCCS = 19 (kernel UAPI).  Keep in lock-step with libc/include/termios.h.
 */
#ifndef AURALITE_KERNEL_TTY_TERMIOS_H
#define AURALITE_KERNEL_TTY_TERMIOS_H

#include <stdint.h>

#define NCCS 19

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct termios {
    tcflag_t c_iflag;     /* input modes */
    tcflag_t c_oflag;     /* output modes */
    tcflag_t c_cflag;     /* control modes */
    tcflag_t c_lflag;     /* local modes */
    cc_t     c_line;      /* line discipline */
    cc_t     c_cc[NCCS];  /* control characters */
};

/* c_cc subscripts (asm-generic). */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSWTC     7
#define VSTART    8
#define VSTOP     9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag bits. */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000
#define IUTF8   0x4000

/* c_oflag bits. */
#define OPOST   0x0001
#define OLCUC   0x0002
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020

/* c_cflag bits. */
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

/* c_lflag bits. */
#define ISIG    0x00001
#define ICANON  0x00002
#define ECHO    0x00008
#define ECHOE   0x00010
#define ECHOK   0x00020
#define ECHONL  0x00040
#define NOFLSH  0x00080
#define TOSTOP  0x00100
#define ECHOCTL 0x00200
#define ECHOKE  0x00800
#define IEXTEN  0x08000

/* tcsetattr() optional_actions. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* TTY ioctls (magic 'T' = 0x54). */
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

#endif /* AURALITE_KERNEL_TTY_TERMIOS_H */
