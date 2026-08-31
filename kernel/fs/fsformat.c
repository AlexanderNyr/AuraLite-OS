/* fsformat.c — auto-format gate state (FSFULL_PLAN.md F1).
 *
 * Trivial by design: the interesting logic lives in the five drivers'
 * init paths, which consult fs_format_allowed() before ever calling
 * their format_* functions.  The state here is a build default plus an
 * optional fw_cfg override, mirroring kernel/lib/selftest.c (OPT O2).
 */

#include "kernel/fs/fsformat.h"

static int         fs_format_allow = FS_MOUNT_FORMAT_DEFAULT;
static const char *fs_format_src   = "build";

int fs_format_allowed(void) {
    return fs_format_allow;
}

const char *fs_format_source(void) {
    return fs_format_src;
}

void fs_format_set(int allowed, const char *source) {
    fs_format_allow = allowed ? 1 : 0;
    fs_format_src   = source ? source : "build";
}
