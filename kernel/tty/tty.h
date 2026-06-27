/*
 * tty.h — TTY / N_TTY line discipline (P5).
 *
 * A `struct tty` couples a `struct termios` with an input ring buffer and the
 * canonical line-editing state.  Input bytes are pushed in via tty_input()
 * (from the keyboard/UART IRQ path); tty_read() drains completed lines (canon)
 * or raw bytes (~ICANON) per the termios settings, applying ECHO and ISIG.
 *
 * Output goes through tty_write(), which applies OPOST/ONLCR.
 */
#ifndef AURALITE_KERNEL_TTY_TTY_H
#define AURALITE_KERNEL_TTY_TTY_H

#include <stdint.h>
#include "kernel/tty/termios.h"

#define TTY_IBUF_SIZE 1024   /* raw input ring */
#define TTY_CANON_MAX 256    /* max bytes in a single canonical line */

struct tty {
    struct termios termios;

    /* Cooked/committed bytes ready for read() (canonical lines, or raw bytes). */
    char     rbuf[TTY_IBUF_SIZE];
    int      rbuf_head;       /* next write index */
    int      rbuf_tail;       /* next read index */
    int      rbuf_count;      /* committed bytes available to read() */

    /* Canonical line being edited (not yet committed). */
    char     line[TTY_CANON_MAX];
    int      line_len;

    /* Foreground process group (P6).  ISIG signals route here; 0 = the
     * current/session task as a degenerate interim policy. */
    int      fg_pgid;

    struct winsize winsize;

    /* Output sink: one char to the console/UART. */
    void   (*out)(char c);
};

/* Initialise a tty with sane cooked-mode defaults and output sink @out. */
void tty_init(struct tty *t, void (*out)(char c));

/* Feed one raw input byte through the line discipline (IRQ/poll context).
 * Applies input flags, editing, ISIG, and echo; commits completed lines. */
void tty_input(struct tty *t, unsigned char c);

/* Drain up to @count bytes into @kbuf per canonical/raw + VMIN/VTIME rules.
 * Returns bytes read (0 = canonical EOF / poll-empty), or a negative errno.
 * Does NOT block here; the syscall layer loops + yields when 0 bytes and the
 * request must block (mirrors the existing stdin yield model). */
int tty_read_available(struct tty *t, char *kbuf, int count);

/* True if a canonical read() would currently complete (a committed line is
 * available, or raw mode has >= VMIN bytes / VMIN==0). */
int tty_readable(struct tty *t);

/* Write @len bytes through OPOST processing to the output sink. */
int tty_write(struct tty *t, const char *buf, int len);

/* ioctl handler for a tty: TCGETS / TCSETS family / TIOCGWINSZ / etc.  @arg is
 * a kernel pointer to the already-copied-in/out structure.  Returns 0 or
 * a negative errno. */
int tty_ioctl(struct tty *t, unsigned long cmd, void *arg);

/* The system console tty (/dev/tty0). */
struct tty *tty_console(void);

/* Deliver @signo to @t's foreground process group (or the current task if no
 * foreground group is set).  Used by the console stdin path for Ctrl+C. */
void tty_send_signal_fg(struct tty *t, int signo);

#endif /* AURALITE_KERNEL_TTY_TTY_H */
