/* test_fsformat.c — host unit test for the auto-format gate
 * (FSFULL_PLAN.md F1).  Compiles the real kernel/fs/fsformat.c, not a
 * copy; the gate's DEFAULT value is the safety property, so it is pinned
 * here the way the FIXES plans pin behaviour rather than implementation.
 */

#include <stdio.h>
#include <string.h>

#include "kernel/fs/fsformat.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS: %s\n", msg);                                     \
        } else {                                                             \
            printf("  FAIL: %s\n", msg);                                     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void) {
    printf("[fsformat] build default is OFF (safe)\n");
    CHECK(fs_format_allowed() == 0, "default refuses auto-format");
    CHECK(strcmp(fs_format_source(), "build") == 0, "default source is 'build'");
#if FS_MOUNT_FORMAT_DEFAULT == 0
    CHECK(1, "FS_MOUNT_FORMAT_DEFAULT=0 compiled in");
#else
    CHECK(0, "FS_MOUNT_FORMAT_DEFAULT is unexpectedly nonzero");
#endif

    printf("[fsformat] explicit enable/disable round-trips\n");
    fs_format_set(1, "fw_cfg");
    CHECK(fs_format_allowed() == 1, "fs_format_set(1) allows formatting");
    CHECK(strcmp(fs_format_source(), "fw_cfg") == 0, "override source recorded");
    fs_format_set(0, "fw_cfg");
    CHECK(fs_format_allowed() == 0, "fs_format_set(0) refuses formatting");

    printf("[fsformat] value coercion and source fallback\n");
    fs_format_set(7, NULL);
    CHECK(fs_format_allowed() == 1, "any nonzero value enables");
    CHECK(strcmp(fs_format_source(), "build") == 0, "NULL source falls back to 'build'");
    fs_format_set(0, NULL);
    CHECK(fs_format_allowed() == 0, "back to refuse");
    CHECK(strcmp(fs_format_source(), "build") == 0, "source still 'build'");

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", failures);
    return 1;
}
