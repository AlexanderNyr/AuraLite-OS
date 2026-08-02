/* progpath.c — resolving a program name to a path.
 *
 * Phase F2 of FSLAYOUT_PLAN.md.
 *
 * `run calc` should work wherever calc lives.  This exists BEFORE the layout
 * move in F3 rather than after it, so that the same command works on both
 * sides of the move — which is the evidence the move is safe.
 *
 * WHY IN LIBC AND NOT IN THE SHELL
 *
 * The shell is not the only thing that launches programs: the GUI launcher
 * does too, from its own hardcoded table.  Two copies of a search list is two
 * things to update in F3, and the second one is the one that gets forgotten.
 * One implementation, one list, both callers.
 */

#include "unistd.h"
#include "fcntl.h"
#include "string.h"

/*
 * The search order.
 *
 *   /bin    core system programs
 *   /apps   applications
 *   /demos  demonstrations
 *   /tests  test programs
 *   /opt    installed packages
 *   /       the flat layout that exists today
 *
 * The first five are the layout phase F3 introduces; "/" is where everything
 * still is.  Searching a directory that does not exist costs one failed
 * lookup, so this list can name the future without waiting for it, and F3
 * becomes a packaging change rather than a flag day.
 *
 * "/" is last deliberately: once F3 ships aliases at the root, a program in
 * its proper directory should win over its own compatibility alias.
 */
static const char *const prog_search_path[] = {
    "/bin", "/apps", "/demos", "/tests", "/opt", "/",
};

#define PROG_SEARCH_COUNT \
    ((int)(sizeof(prog_search_path) / sizeof(prog_search_path[0])))

int prog_path_count(void) { return PROG_SEARCH_COUNT; }

const char *prog_path_entry(int index) {
    if (index < 0 || index >= PROG_SEARCH_COUNT) return 0;
    return prog_search_path[index];
}

int prog_resolve(const char *name, char *out, int out_len) {
    if (!name || !*name || !out || out_len < 2) return 0;

    /* A name containing '/' is a path, not a name.  Use it as given: an
     * explicit path always bypasses the search, and reporting "not found on
     * the search path" for a file the caller named specifically would be
     * misleading. */
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            int i = 0;
            while (name[i] && i < out_len - 1) { out[i] = name[i]; i++; }
            out[i] = '\0';
            return 1;
        }
    }

    for (int d = 0; d < PROG_SEARCH_COUNT; d++) {
        const char *dir = prog_search_path[d];
        int i = 0;
        for (const char *c = dir; *c && i < out_len - 1; c++) out[i++] = *c;
        /* "/" already ends in a separator; every other entry needs one. */
        if (i > 1 && i < out_len - 1) out[i++] = '/';
        for (const char *c = name; *c && i < out_len - 1; c++) out[i++] = *c;
        out[i] = '\0';

        /* Existence is tested by opening.  There is no access(X_OK) worth
         * consulting here: initrd entries carry no mode bits, so a permission
         * test would refuse every shipped program. */
        int fd = open(out, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
    }
    return 0;
}
