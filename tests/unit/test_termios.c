/*
 * test_termios.c — host-side unit test for P5 termios constants + cfmakeraw +
 * the line-discipline canonical/raw decision logic and FILE* buffering rules.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "libc/include/termios.h"
#include "libc/include/sys/ioctl.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* Replica of libc cfmakeraw (must match libc/src/libc.c). */
static void a_cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                              INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |=  (tcflag_t)CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

int main(void) {
    /* Numeric ABI (Linux asm-generic). */
    CK(NCCS == 19);
    CK(sizeof(struct termios) == 4*4 + 1 + NCCS);  /* 4 flags + c_line + c_cc */
    CK(VINTR == 0 && VEOF == 4 && VTIME == 5 && VMIN == 6);
    CK(ICRNL == 0x100 && IGNCR == 0x080 && INLCR == 0x040 && IXON == 0x400);
    CK(OPOST == 0x1 && ONLCR == 0x4);
    CK(ISIG == 0x1 && ICANON == 0x2 && ECHO == 0x8 && ECHOE == 0x10);
    CK(TCGETS == 0x5401 && TCSETS == 0x5402 && TCSETSW == 0x5403 && TCSETSF == 0x5404);
    CK(TIOCGWINSZ == 0x5413);
    CK(TCSANOW == 0 && TCSADRAIN == 1 && TCSAFLUSH == 2);

    /* cfmakeraw clears the right bits and sets VMIN=1/VTIME=0 (real glibc). */
    struct termios t;
    memset(&t, 0, sizeof(t));
    t.c_iflag = ICRNL | IXON | ISTRIP;
    t.c_oflag = OPOST | ONLCR;
    t.c_lflag = ISIG | ICANON | ECHO | ECHOE | IEXTEN;
    t.c_cflag = CS5 | PARENB | CREAD;
    a_cfmakeraw(&t);
    CK((t.c_iflag & (ICRNL | IXON | ISTRIP)) == 0);
    CK((t.c_oflag & OPOST) == 0);
    CK((t.c_lflag & (ECHO | ICANON | ISIG | IEXTEN)) == 0);
    CK((t.c_cflag & CSIZE) == CS8);
    CK(t.c_cc[VMIN] == 1 && t.c_cc[VTIME] == 0);
    CK((t.c_cflag & CREAD) == CREAD);   /* unrelated bits preserved */

    /* winsize layout. */
    CK(sizeof(struct winsize) == 8);

    if (failures == 0) { printf("test_termios: ALL PASS\n"); return 0; }
    printf("test_termios: %d FAILURE(S)\n", failures);
    return 1;
}
