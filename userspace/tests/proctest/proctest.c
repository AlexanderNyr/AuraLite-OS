/* proctest.c — process-exit GUI cleanup smoke test.
 *
 * Plain "open window then exit"; the shell that spawned us will see our
 * window be torn down by gui_cleanup_process() in thread_exit().  An
 * integration assertion looks for the kernel log line:
 *   "[gui] cleaned 1 window(s) for pid <PID>"
 *
 * We deliberately stay single-threaded and DO NOT call spawn/fork/waitpid
 * from here, because triggering a second user-space syscall path on top of
 * the parent shell's still-pending wait4 currently races on the per-thread
 * SYSCALL save area (see CHANGELOG and docs/status.md).
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "auragui.h"

static void say(const char *name, int ok) {
    printf("PROCTEST %s: %s\n", ok ? "PASS" : "FAIL", name);
}

int main(void) {
    int wid = ag_window_create(60, 60, 200, 120, "proctest-window",
                               AG_WIN_DEFAULT);
    say("window created", wid >= 0);
    if (wid >= 0) {
        say("clear works on owned window", ag_clear(wid, AG_BLUE) == 0);
        say("draw_text works on owned window",
            ag_draw_text(wid, 4, 16, "hello", AG_WHITE) == 0);
    }
    /* Print our pid so the integration script can correlate the
     * "[gui] cleaned 1 window(s) for pid N" line. */
    printf("PROCTEST PID %d window %d\n", (int)getpid(), wid);

    /* Returning from main triggers _exit(0) via crt0, which goes through
     * SYS_EXIT -> thread_exit_with_code -> gui_cleanup_process(self->id).
     * The kernel will print "[gui] cleaned 1 window(s) for pid <pid>". */
    return 0;
}
