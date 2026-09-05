/* readline.h — the in-tree line editor (RESIDUE2 T4).
 *
 * The TTY bundle's "readline line editor": a small, dependency-light
 * editor with kill/erase, cursor motion and history, a deliberately tiny
 * cousin of GNU readline rather than a clone of its 4k-line surface.
 *
 * Two layers:
 *   - rl_feed(): a PURE state machine (kernel-free, host-tested): feed it
 *     bytes, it updates the edit buffer and emits the terminal byte
 *     sequences that keep the screen in sync;
 *   - readline(): the loop — prompt, read bytes from fd 0 (or the fd the
 *     caller set), drive rl_feed, return a malloc'd line without the
 *     newline.  History is up/down arrow when the input fd is raw; in
 *     canonical mode the discipline already edits, and rl_feed's
 *     printable/erase/kill handling stays correct for the bytes it sees.
 *
 * Echo policy (documented, not guessed): readline() consults tcgetattr();
 * when ECHO is already on it does NOT echo (the terminal side is doing
 * it); when ECHO is off it echoes itself, so a program that turned echo
 * off for password-style input still sees what it types — unless it also
 * sets rl_noecho.
 */

#ifndef AURALITE_LIBC_READLINE_H
#define AURALITE_LIBC_READLINE_H

#define RL_BUF_MAX 256
#define RL_HIST_MAX 16

/* The edit state.  Plain struct on purpose: host tests poke it directly. */
typedef struct {
    char  buf[RL_BUF_MAX];       /* the line being edited (no newline)     */
    int   len;                   /* bytes in buf                           */
    int   pos;                   /* cursor: 0..len                         */
    int   noecho;                /* 1: rl_feed emits nothing (set by user) */
    int   dirty;                 /* 1: buf changed since last rl_reset     */
} rl_line_t;

/* One byte of input.  @out (may be NULL) receives the terminal byte
 * sequence that repaints the change (erase, kill, cursor move, echo);
 * returns the number of bytes written to @out, never fails.  Handles:
 *   printable      insert at cursor
 *   BS / DEL(0x7F) delete before cursor
 *   ESC [ D / C    cursor left / right
 *   ESC [ A / B    history prev / next (via the rl_hist_* helpers)
 *   ESC [ H / F, ESC O H / F   home / end  (also HOME/END raw forms)
 *   ^U (0x15)      kill to start of line
 *   ^W (0x17)      kill to start of word
 *   ^A / ^E        home / end
 *   CR / LF        accepted by readline(), not rl_feed (left in stream)
 * Other control bytes are ignored. */
int rl_feed(rl_line_t *rl, unsigned char c, char *out, int out_max);

/* ---- history ring (pure, host-tested) ---- */
/* Push a line onto the history ring (newest last-navigation state resets). */
void rl_hist_add(const char *line);
/* Navigate: @dir -1 = older (up), +1 = newer (down).  Fills @out with the
 * entry at the new position; returns 1 when the position moved, 0 at the
 * ends (and @out untouched). */
int rl_hist_step(int dir, char *out, int out_max);
/* Test/diagnostic access to the ring position. */
int rl_hist_pos(void);
int rl_hist_count(void);

/* ---- the loop layer ---- */
/* Read one line from fd 0 with prompt @prompt (NULL/"" = none).  Returns a
 * malloc'd string WITHOUT the newline (free() it), or NULL on EOF with an
 * empty line / read error.  See the header comment for the echo policy. */
char *readline(const char *prompt);

#endif /* AURALITE_LIBC_READLINE_H */
