/* W32A-11 guest sender for stale-token/owner-ACL regression. This is an
 * independent native process, not a shared address-space unit test.
 * SPDX-License-Identifier: Apache-2.0 */
#include "auragui.h"
#include "fcntl.h"
#include "unistd.h"
#include "time.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include <stdint.h>

#define RECEIVER "/tests/w32a11_token_receiver"
#define READY1 "/tmp/a11-token-ready1"
#define READY2 "/tmp/a11-token-ready2"
#define READY3 "/tmp/a11-token-ready3"
#define ACK    "/tmp/a11-token-ack"
#define PATH   "/tests/w32a11_payload.txt"

static int signal_ack(void) {
    int fd = open(ACK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, "checked\n", 8) == 8;
    close(fd);
    return ok ? 0 : -1;
}
static int await_file(const char *name, char *buf, size_t len) {
    for (int i = 0; i < 100; ++i) {
        int fd = open(name, O_RDONLY);
        if (fd >= 0) {
            int n = (int)read(fd, buf, len - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; return 0; }
        }
        usleep(100000);
    }
    return -1;
}
static int send_path(int source, pid_t receiver) {
    for (int i = 0; i < 100; ++i) {
        if (ag_send_file_drop(source, PATH) == 0) return 0;
        int status = 0;
        if (waitpid(receiver, &status, WNOHANG) != 0) return -1;
        usleep(100000);
    }
    return -1;
}
int main(void) {
    unlink(READY1); unlink(READY2); unlink(READY3); unlink(ACK);
    /* Become a GUI participant before requesting the global mouse position. */
    int source = ag_window_create(0, 0, 80, 50, "token source",
                                   AG_WIN_NO_DECOR | AG_WIN_BORDERLESS);
    if (source < 0) goto fail;
    int32_t mx = 0, my = 0;
    uint32_t width = 0, height = 0;
    if (ag_mouse_position(&mx, &my) != 0 ||
        ag_screen_size(&width, &height) != 0) goto fail;
    int32_t sx = mx < (int32_t)(width / 2) ? (int32_t)width - 80 : 0;
    int32_t sy = my < (int32_t)(height / 2) ? (int32_t)height - 50 : 0;
    if (ag_window_move(source, sx, sy) != 0 || ag_window_show(source) != 0)
        goto fail;
    char *argv[] = {RECEIVER, NULL};
    pid_t child = spawnv(argv[0], argv);
    if (child <= 0) goto fail;
    char mark[64];
    if (await_file(READY1, mark, sizeof mark) != 0 ||
        send_path(source, child) != 0 ||
        await_file(READY2, mark, sizeof mark) != 0 ||
        send_path(source, child) != 0 ||
        await_file(READY3, mark, sizeof mark) != 0) goto fail;
    int wid = -1;
    unsigned token = 0;
    if (sscanf(mark, "%d %u", &wid, &token) != 2 || wid < 0 ||
        token == 0 || token > UINT16_MAX) goto fail;
    ag_event_t stolen = {0};
    if (ag_poll_event(wid, &stolen) != -1) goto fail;
    char path[AG_DROP_PATH_MAX];
    memset(path, 'X', sizeof path);
    if (ag_take_file_drop(wid, (uint16_t)token, path) == 0 || path[0] != 'X')
        goto fail;
    printf("A11-TOKEN-ACL-OK\n");
    if (signal_ack() != 0) goto fail;
    int status = 0;
    pid_t done = 0;
    for (int i = 0; i < 100; ++i) {
        done = waitpid(child, &status, WNOHANG);
        if (done != 0) break;
        usleep(100000);
    }
    ag_window_destroy(source);
    if (done != child || !WIFEXITED(status) || WEXITSTATUS(status) != 78)
        goto fail;
    printf("A11-TOKEN-SENDER-OK\n");
    return 78;
fail:
    printf("A11-TOKEN-FAIL: sender\n");
    return 79;
}
