/* guiacl.c — RESIDUE2 T7 gate program: the GUI-global state ACL.
 *
 * The T7 permission model draws one line in the kernel (gui_syscalls.c,
 * require_gui_participant): per-window ops require owning THAT window, and
 * mutations of GUI-GLOBAL state -- the kernel clipboard, the desktop theme,
 * taskbar notifications, desktop icons -- require owning at least one live
 * window.  This probe walks all three states of that contract from one
 * process:
 *
 *   1. windowless  : every global mutation AND clipboard read denied (-1),
 *                    while the theme READ stays open (carries no secrets);
 *   2. one window  : the same ops succeed, clipboard round-trips;
 *   3. after destroy: participant status is DYNAMIC -- denied again.
 *
 * Output is plain printf, so the serial log pins every check:
 *   "ACLTEST PASS: ..." xN and a final "ACLTEST DONE: N/N".
 * Exit status 0 only when nothing failed.
 */
#include "auragui.h"
#include "stdio.h"
#include "string.h"

static int fails = 0;
static int total = 0;

static void check(const char *name, int cond) {
    total++;
    if (cond) {
        printf("ACLTEST PASS: %s\n", name);
    } else {
        printf("ACLTEST FAIL: %s\n", name);
        fails++;
    }
}

int main(void) {
    char buf[64];

    /* ---- Phase 1: a windowless process is not a GUI participant ---- */
    check("windowless set_clipboard denied", ag_set_clipboard("SNEAK") != 0);
    buf[0] = 0;
    check("windowless get_clipboard denied", ag_get_clipboard(buf, sizeof buf) != 0);
    check("windowless clipboard stayed unread", buf[0] == 0);

    ag_theme_t t;
    int got = ag_theme_get(&t);
    check("theme read open to everyone", got == 0);
    check("windowless theme_set denied", ag_theme_set(&t) != 0);
    check("windowless notify denied", ag_notify("SNEAK", AG_BLUE, 100) != 0);
    check("windowless add_icon denied", ag_add_icon(8, 8, "SNEAK", 0) != 0);

    /* ---- Phase 2: owning one window grants the global ops ---- */
    int wid = ag_window_create(48, 48, 140, 90, "ACL probe", AG_WIN_DEFAULT);
    check("window created", wid >= 0);
    if (wid >= 0)
        ag_window_show(wid);

    check("participant set_clipboard ok", ag_set_clipboard("ACL-TOKEN") == 0);
    buf[0] = 0;
    check("participant get_clipboard ok", ag_get_clipboard(buf, sizeof buf) == 0);
    check("clipboard round-trip", strcmp(buf, "ACL-TOKEN") == 0);
    check("participant theme_set ok", ag_theme_set(&t) == 0);
    check("participant notify ok (slot >= 0)", ag_notify("ACL probe", AG_BLUE, 100) >= 0);

    /* ---- Phase 3: destroying the window revokes participant status ---- */
    if (wid >= 0)
        ag_window_destroy(wid);
    check("after destroy set_clipboard denied again",
          ag_set_clipboard("SNEAK2") != 0);

    if (fails == 0)
        printf("ACLTEST DONE: %d/%d\n", total, total);
    else
        printf("ACLTEST DONE: %d/%d (%d FAILED)\n", total - fails, total, fails);
    return fails ? 1 : 0;
}
