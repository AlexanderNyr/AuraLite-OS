/*
 * tty.c — N_TTY line discipline.
 *
 * POSIX.1-2017 §11 "General Terminal Interface".  Input bytes are processed
 * per c_iflag, then either edited into a canonical line (ICANON) or committed
 * raw.  ISIG-recognised characters generate signals via the foreground-pgid
 * indirection (tty_signal_fg) and are discarded.  Echo honours ECHO/ECHOE/
 * ECHOCTL and passes through the same OPOST output path.
 */

#include <stdint.h>
#include "kernel/tty/tty.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/proc/signal.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/time.h"        /* RESIDUE2 T4: ktime_ticks() for VTIME */

/* ---- output (OPOST) ---- */

/*
 * RESIDUE2 T4 — the column model.  Every OUTPUT byte moves it, not just
 * echo: a program printing "abc\t" leaves the cursor at column 8, and an
 * erase after that must still be right.  Tabs advance to the next 8-column
 * stop, \b steps back one, \r homes the line, \n (and the ONLCR \r\n pair)
 * starts a new one.  Column math caps at 0 so backspacing at column 0
 * cannot underflow.
 */
#define TTY_TAB_STOP 8

static void tty_out_char(struct tty *t, char c) {
    if (t->termios.c_oflag & OPOST) {
        if (c == '\n' && (t->termios.c_oflag & ONLCR)) {
            t->out('\r');
            t->out('\n');
            t->column = 0;
            return;
        }
    }
    t->out(c);
    if (c == '\n') {
        t->column = 0;
    } else if (c == '\r') {
        t->column = 0;
    } else if (c == '\b') {
        if (t->column > 0) t->column--;
    } else if (c == '\t') {
        t->column = (t->column / TTY_TAB_STOP + 1) * TTY_TAB_STOP;
    } else {
        t->column++;   /* printable + CP437 byte: one column each */
    }
}

int tty_write(struct tty *t, const char *buf, int len) {
    if (!t || !buf || len < 0) return -EINVAL;
    for (int i = 0; i < len; i++) tty_out_char(t, buf[i]);
    return len;
}

/*
 * Echo one input character, rendering control chars as ^X when ECHOCTL.
 * RESIDUE2 T4: returns the number of SCREEN COLUMNS the echo occupied, so
 * the canonical editor can record it per character (line_cols[]) and later
 * erase exactly what was drawn.  The ^X pair is written through t->out
 * directly (both bytes are one width-2 unit; routing them through
 * tty_out_char would double-count).
 */
static int tty_echo(struct tty *t, char c) {
    if (!(t->termios.c_lflag & ECHO)) {
        /* ECHONL: a newline is echoed even with ECHO off. */
        if (c == '\n' && (t->termios.c_lflag & ECHONL)) tty_out_char(t, '\n');
        return 0;
    }
    unsigned char uc = (unsigned char)c;
    if ((t->termios.c_lflag & ECHOCTL) && uc < 0x20 && c != '\n' && c != '\t') {
        t->out('^');
        t->out((char)(uc + 0x40));
        t->column += 2;
        return 2;
    }
    /* Tab width is decided by the column the tab is struck AT. */
    int width = (c == '\t') ? TTY_TAB_STOP - (t->column % TTY_TAB_STOP) : 1;
    tty_out_char(t, c);           /* advances t->column by the same width */
    return width;
}

/*
 * Visually erase the last echoed character (ECHOE): backspace, space,
 * backspace, @cols times — @cols is the RECORDED width from tty_echo(),
 * which is what makes a tab erase its full 1..8 columns and a ^X its two.
 */
static void tty_echo_erase(struct tty *t, int cols) {
    if (!(t->termios.c_lflag & ECHO) || !(t->termios.c_lflag & ECHOE)) return;
    if (cols <= 0) return;
    for (int i = 0; i < cols; i++) { t->out('\b'); t->out(' '); t->out('\b'); }
    if (t->column >= cols) t->column -= cols; else t->column = 0;
}

/* ---- signal routing (interim: degenerate foreground group) ---- */

static void tty_signal_fg(struct tty *t, int signo) {
    /* Deliver to every process in the terminal's foreground process group
     * (P6).  If no foreground group has been set yet, fall back to the current
     * task (the reader/shell) so Ctrl+C still works before job control runs. */
    if (t->fg_pgid > 0) {
        signal_send_group(t->fg_pgid, signo);
    } else {
        tcb_t *cur = sched_current();
        if (cur) signal_send(cur, signo);
    }
}

void tty_send_signal_fg(struct tty *t, int signo) { tty_signal_fg(t, signo); }

static void tty_flush_input(struct tty *t) {
    t->rbuf_head = t->rbuf_tail = t->rbuf_count = 0;
    t->line_len = 0;
}

/* ---- committing bytes to the read buffer ---- */

static void rbuf_push(struct tty *t, char c) {
    if (t->rbuf_count >= TTY_IBUF_SIZE) return;   /* overflow: drop */
    t->rbuf[t->rbuf_head] = c;
    t->rbuf_head = (t->rbuf_head + 1) % TTY_IBUF_SIZE;
    t->rbuf_count++;
    /* RESIDUE2 T4: VTIME's inter-byte clock runs on the arrival of the
     * LAST committed byte, so stamp every push (cheap) rather than
     * tracking it per-reader. */
    t->last_rx_ticks = ktime_ticks();
}

/* Commit the current canonical line (including any delimiter already appended)
 * to the read buffer and reset the edit buffer. */
static void commit_line(struct tty *t) {
    for (int i = 0; i < t->line_len; i++) rbuf_push(t, t->line[i]);
    t->line_len = 0;
}

/* ---- input processing ---- */

void tty_input(struct tty *t, unsigned char c) {
    struct termios *tm = &t->termios;

    /* Input flag CR/NL translation — classify the ORIGINAL byte once. */
    if (c == '\r') {
        if (tm->c_iflag & IGNCR) return;            /* drop CR entirely */
        if (tm->c_iflag & ICRNL) c = '\n';
    } else if (c == '\n') {
        if (tm->c_iflag & INLCR) c = '\r';
    }

    /* ISIG: signal-generating characters (discarded, queues flushed). */
    if (tm->c_lflag & ISIG) {
        int sig = 0;
        if (c == tm->c_cc[VINTR]) sig = SIGINT;
        else if (c == tm->c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == tm->c_cc[VSUSP]) sig = SIGTSTP;
        if (sig) {
            tty_echo(t, (char)c);                   /* echo ^C before flush */
            if (!(tm->c_lflag & NOFLSH)) tty_flush_input(t);
            tty_signal_fg(t, sig);
            return;
        }
    }

    if (tm->c_lflag & ICANON) {
        /* Canonical editing. */
        if (c == tm->c_cc[VERASE]) {
            if (t->line_len > 0) {
                t->line_len--;
                tty_echo_erase(t, t->line_cols[t->line_len]);
            }
            return;
        }
        if (c == tm->c_cc[VKILL]) {
            while (t->line_len > 0) {
                t->line_len--;
                tty_echo_erase(t, t->line_cols[t->line_len]);
            }
            return;
        }
        if (c == tm->c_cc[VEOF]) {
            /* ^D: make the current line available without a newline.  If the
             * line is empty this commits 0 bytes -> read() returns 0 (EOF).
             * The ^D byte is discarded either way. */
            commit_line(t);
            return;
        }
        if (c == '\n' || c == tm->c_cc[VEOL]) {
            /* Line delimiter: echo + include in the committed line. */
            if (t->line_len < TTY_CANON_MAX) {
                t->line[t->line_len] = (char)c;
                t->line_cols[t->line_len++] = 0;   /* delimiter: erased as a unit */
            }
            tty_echo(t, (char)c);
            commit_line(t);
            return;
        }
        /* Ordinary character: append to the edit buffer + echo, recording
         * the echoed width for a true-width erase later (RESIDUE2 T4). */
        if (t->line_len < TTY_CANON_MAX - 1) {
            t->line[t->line_len] = (char)c;
            t->line_cols[t->line_len++] = (signed char)tty_echo(t, (char)c);
        }
        return;
    }

    /* Non-canonical (raw): commit the byte immediately + optional echo. */
    rbuf_push(t, (char)c);
    tty_echo(t, (char)c);
}

/* ---- reads ---- */

int tty_readable(struct tty *t) {
    if (t->termios.c_lflag & ICANON) {
        return t->rbuf_count > 0;
    }
    /* Raw: VMIN==0 means a read never blocks; otherwise need >= VMIN bytes. */
    int vmin = t->termios.c_cc[VMIN];
    if (vmin == 0) return 1;
    return t->rbuf_count >= vmin;
}

int tty_read_available(struct tty *t, char *kbuf, int count) {
    if (!t || !kbuf) return -EINVAL;
    if (count <= 0) return 0;

    if (t->termios.c_lflag & ICANON) {
        /* Canonical: return bytes up to and including the next line delimiter,
         * capped at @count.  rbuf already holds committed lines. */
        int got = 0;
        while (got < count && t->rbuf_count > 0) {
            char c = t->rbuf[t->rbuf_tail];
            t->rbuf_tail = (t->rbuf_tail + 1) % TTY_IBUF_SIZE;
            t->rbuf_count--;
            kbuf[got++] = c;
            if (c == '\n' || c == t->termios.c_cc[VEOL]) break;  /* one line */
        }
        return got;
    }

    /* Raw: return min(available, count). */
    int got = 0;
    while (got < count && t->rbuf_count > 0) {
        kbuf[got++] = t->rbuf[t->rbuf_tail];
        t->rbuf_tail = (t->rbuf_tail + 1) % TTY_IBUF_SIZE;
        t->rbuf_count--;
    }
    return got;
}

/* ---- ioctl ---- */

int tty_ioctl(struct tty *t, unsigned long cmd, void *arg) {
    switch (cmd) {
    case TCGETS:
        *(struct termios *)arg = t->termios;
        return 0;
    case TCSETS:    /* TCSANOW */
    case TCSETSW:   /* TCSADRAIN — no real TX queue, apply now */
        t->termios = *(struct termios *)arg;
        return 0;
    case TCSETSF:   /* TCSAFLUSH — discard unread input, then apply */
        tty_flush_input(t);
        t->termios = *(struct termios *)arg;
        return 0;
    case TIOCGWINSZ:
        *(struct winsize *)arg = t->winsize;
        return 0;
    case TIOCSWINSZ:
        t->winsize = *(struct winsize *)arg;
        tty_signal_fg(t, SIGWINCH);   /* notify foreground of resize */
        return 0;
    case TIOCGPGRP:
        *(int *)arg = t->fg_pgid;
        return 0;
    case TIOCSPGRP:
        t->fg_pgid = *(int *)arg;
        return 0;
    default:
        return -ENOTTY;   /* unknown terminal ioctl */
    }
}

/* ---- defaults + console singleton ---- */

void tty_init(struct tty *t, void (*out)(char c)) {
    memset(t, 0, sizeof(*t));   /* line_cols/column/last_rx_ticks start at 0 */
    t->out = out;
    struct termios *tm = &t->termios;
    /* Sane cooked-mode defaults (Linux-like). */
    tm->c_iflag = ICRNL | IXON;
    tm->c_oflag = OPOST | ONLCR;
    tm->c_cflag = CS8 | CREAD | CLOCAL;
    tm->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN;
    tm->c_cc[VINTR]  = 3;     /* ^C */
    tm->c_cc[VQUIT]  = 28;    /* ^\ */
    tm->c_cc[VERASE] = 127;   /* DEL */
    tm->c_cc[VKILL]  = 21;    /* ^U */
    tm->c_cc[VEOF]   = 4;     /* ^D */
    tm->c_cc[VEOL]   = 0;
    tm->c_cc[VSUSP]  = 26;    /* ^Z */
    tm->c_cc[VMIN]   = 1;
    tm->c_cc[VTIME]  = 0;
    t->winsize.ws_row = 25;
    t->winsize.ws_col = 80;
}

static struct tty g_console;
static int g_console_ready = 0;

/* Defined in kernel/lib/kprintf.c. */
extern void kputchar(char c);

struct tty *tty_console(void) {
    if (!g_console_ready) {
        tty_init(&g_console, kputchar);
        /* RESIDUE2 T4: the keyboard now mirrors decoded bytes here as well
         * as into the legacy kb ring.  The legacy fd-0 stdin path (which
         * the shell uses) owns ECHO and ISIG for those same bytes, so the
         * console tty ships with both OFF: opening /dev/tty0 must not
         * double every keystroke onto the screen or double-deliver ^C.  A
         * program that wants its own line editor with echo sets TCSETS. */
        g_console.termios.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ISIG);
        g_console_ready = 1;
    }
    return &g_console;
}

/* ---- RESIDUE2 T4: the serial tty (/dev/ttyS0) ----
 *
 * Same discipline, UART output sink.  Input is POLLed from the UART by the
 * ttyS0 read path (drivers/uart stays RX-interrupt-free: the console stdin
 * path polls the same RBR, and an IRQ consumer would race bytes away from
 * every serial-driven integration test).  ECHO/ISIG default off for the
 * same reason as the console: the shell's stdin path echoes serial bytes
 * itself while it is the reader. */
extern void uart_putchar(char c);   /* drivers/uart/uart.h, kept out for deps */

static struct tty g_serial;
static int g_serial_ready = 0;

struct tty *tty_serial(void) {
    if (!g_serial_ready) {
        tty_init(&g_serial, uart_putchar);
        g_serial.termios.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ISIG);
        g_serial_ready = 1;
    }
    return &g_serial;
}

/* ---- RESIDUE2 T4: the blocking-read inputs (see tty.h) ---- */

int tty_rx_count(struct tty *t) {
    return t ? t->rbuf_count : 0;
}

uint64_t tty_last_rx_ticks(struct tty *t) {
    return t ? t->last_rx_ticks : 0;
}
