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

    /* RESIDUE2 T4 — column tracking.  line_cols[i] records how many screen
     * columns line[i] occupied when it was echoed (^X -> 2, tab -> to the
     * next 8-column stop, printable -> 1), so VERASE/VKILL erase EXACTLY
     * what was drawn instead of a fixed 1-2 columns.  column is the shared
     * output model updated by tty_out_char (tabs advance to stops, \n/\r
     * reset, \b decrements), which is what makes tab-stop math correct even
     * when a program's own writes moved the cursor first. */
    signed char line_cols[TTY_CANON_MAX];
    int      column;

    /* RESIDUE2 T4 — VTIME support.  Kernel tick stamp of the LAST byte
     * committed to rbuf; the read path's inter-byte timer (VMIN>0,VTIME>0)
     * and overall timer (VMIN==0,VTIME>0) read it through
     * tty_last_rx_ticks().  0 means "nothing yet this boot". */
    uint64_t last_rx_ticks;

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

/* RESIDUE2 T4 — the blocking-read inputs.  tty_rx_count() exposes the raw
 * committed-byte count (the VMIN comparison the syscall layer runs); the
 * callers must not reach into the struct because canonical mode's
 * "readable" is line-based, not count-based.  tty_last_rx_ticks() feeds
 * the VTIME inter-byte timer (kernel tick stamp of the last commit). */
int      tty_rx_count(struct tty *t);
uint64_t tty_last_rx_ticks(struct tty *t);

/* Write @len bytes through OPOST processing to the output sink. */
int tty_write(struct tty *t, const char *buf, int len);

/* ioctl handler for a tty: TCGETS / TCSETS family / TIOCGWINSZ / etc.  @arg is
 * a kernel pointer to the already-copied-in/out structure.  Returns 0 or
 * a negative errno. */
int tty_ioctl(struct tty *t, unsigned long cmd, void *arg);

/* The system console tty (/dev/tty0). */
struct tty *tty_console(void);

/* RESIDUE2 T4: the serial tty (/dev/ttyS0) — same discipline, output sink
 * is the UART.  Input is polled from the UART by the ttyS0 read path (the
 * console stdin path keeps its own poll; serial RX stays interrupt-free so
 * every existing serial-driven test sees the same byte stream). */
struct tty *tty_serial(void);

/* Deliver @signo to @t's foreground process group (or the current task if no
 * foreground group is set).  Used by the console stdin path for Ctrl+C. */
void tty_send_signal_fg(struct tty *t, int signo);

#endif /* AURALITE_KERNEL_TTY_TTY_H */
