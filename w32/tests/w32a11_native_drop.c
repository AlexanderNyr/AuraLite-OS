/* W32A-11 guest native sender: different address space from the PE receiver.
 * Kernel compositor owns the bounded file path between processes; no pointer
 * (or fake cross-process HDROP) is sent to the receiver. The PE constructs
 * and owns its own HDROP when it takes the token. SPDX-License-Identifier:
 * Apache-2.0 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "time.h"
#include "sys/wait.h"
#include <stdint.h>

#define PATH "/tests/w32a11_payload.txt"
#define CONTENT "W32A11-DROP-FILE-CONTENTS\n"

int main(void) {
    char buffer[sizeof CONTENT];
    int fd = open(PATH, O_RDONLY);
    if (fd < 0 || read(fd, buffer, sizeof CONTENT - 1) != sizeof CONTENT - 1 ||
        memcmp(buffer, CONTENT, sizeof CONTENT - 1) != 0) {
        printf("A11-SENDER-FAIL: payload unreadable\n");
        if (fd >= 0) close(fd);
        return 79;
    }
    close(fd);
    char *argv[] = {"/apps/w32run", "/tests/w32a11_file_receiver.exe", NULL};
    pid_t child = spawnv(argv[0], argv);
    if (child <= 0) {
        printf("A11-SENDER-FAIL: spawnv\n"); return 79;
    }
    /* GET_MOUSE and GET_SCREEN require a GUI participant. Create the hidden
     * source FIRST, then move it away from the actual pointer before showing
     * it; the receiver centers its own popup on that pointer. */
    int source = ag_window_create(0, 0, 80, 50, "native drop source",
                                   AG_WIN_NO_DECOR | AG_WIN_BORDERLESS);
    if (source < 0) { printf("A11-SENDER-FAIL: source window\n"); return 79; }
    int32_t mx = 0, my = 0;
    uint32_t width = 0, height = 0;
    if (ag_mouse_position(&mx, &my) != 0 ||
        ag_screen_size(&width, &height) != 0) {
        printf("A11-SENDER-FAIL: mouse/screen\n");
        ag_window_destroy(source); return 79;
    }
    int32_t sx = mx < (int32_t)(width / 2) ? (int32_t)width - 80 : 0;
    int32_t sy = my < (int32_t)(height / 2) ? (int32_t)height - 50 : 0;
    if (ag_window_move(source, sx, sy) != 0 || ag_window_show(source) != 0) {
        printf("A11-SENDER-FAIL: source move/show\n");
        ag_window_destroy(source); return 79;
    }
    int status = 0, sent = 0;
    /* No wall-clock-only assertion: retry until a different live window is
     * actually under the pointer, and stop if the child exits prematurely. */
    for (int i = 0; i < 100; ++i) {
        if (ag_send_file_drop(source, PATH) == 0) { sent = 1; break; }
        if (waitpid(child, &status, WNOHANG) == child) break;
        usleep(100000);
    }
    if (!sent) {
        printf("A11-SENDER-FAIL: compositor refused drop\n");
        ag_window_destroy(source);
        return 79;
    }
    printf("A11-SENDER-DELIVERED\n");
    pid_t done = 0;
    for (int i = 0; i < 100; ++i) {
        done = waitpid(child, &status, WNOHANG);
        if (done != 0) break;
        usleep(100000);
    }
    ag_window_destroy(source);
    if (done != child || !WIFEXITED(status) || WEXITSTATUS(status) != 78) {
        printf("A11-SENDER-FAIL: receiver did not complete\n"); return 79;
    }
    printf("A11-SENDER-OK\n");
    return 78;
}
