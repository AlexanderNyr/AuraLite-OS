#ifndef AURALITE_LIBC_TERMIOS_H
#define AURALITE_LIBC_TERMIOS_H

/*
 * termios.h — POSIX.1-2017 terminal interface for AuraLite user programs.
 *
 * The struct layout and flag values match the kernel (kernel/tty/termios.h),
 * Linux x86_64 asm-generic ABI, NCCS = 19.
 */

#include <stdint.h>

#define NCCS 19

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
};

/* c_cc subscripts. */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

/* c_iflag. */
#define IGNBRK 0x0001
#define BRKINT 0x0002
#define IGNPAR 0x0004
#define PARMRK 0x0008
#define INPCK 0x0010
#define ISTRIP 0x0020
#define INLCR 0x0040
#define IGNCR 0x0080
#define ICRNL 0x0100
#define IUCLC 0x0200
#define IXON 0x0400
#define IXANY 0x0800
#define IXOFF 0x1000
#define IMAXBEL 0x2000
#define IUTF8 0x4000

/* c_oflag. */
#define OPOST 0x0001
#define OLCUC 0x0002
#define ONLCR 0x0004
#define OCRNL 0x0008
#define ONOCR 0x0010
#define ONLRET 0x0020

/* c_cflag. */
#define CSIZE 0x0030
#define CS5 0x0000
#define CS6 0x0010
#define CS7 0x0020
#define CS8 0x0030
#define CSTOPB 0x0040
#define CREAD 0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL 0x0400
#define CLOCAL 0x0800

/* c_lflag. */
#define ISIG 0x00001
#define ICANON 0x00002
#define ECHO 0x00008
#define ECHOE 0x00010
#define ECHOK 0x00020
#define ECHONL 0x00040
#define NOFLSH 0x00080
#define TOSTOP 0x00100
#define ECHOCTL 0x00200
#define ECHOKE 0x00800
#define IEXTEN 0x08000

/* tcsetattr optional_actions. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int   tcgetattr(int fd, struct termios *t);
int   tcsetattr(int fd, int optional_actions, const struct termios *t);
void  cfmakeraw(struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int   cfsetispeed(struct termios *t, speed_t speed);
int   cfsetospeed(struct termios *t, speed_t speed);

#endif /* AURALITE_LIBC_TERMIOS_H */
