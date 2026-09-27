/* W32A-11 guest regression: a GUI file-drop pathname token must not survive
 * destruction/reuse of the same compositor window slot. A different native
 * task sends each pathname; the receiver owns and consumes only the new one.
 * SPDX-License-Identifier: Apache-2.0 */
#include "auragui.h"
#include "fcntl.h"
#include "unistd.h"
#include "time.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>

#define READY1 "/tmp/a11-token-ready1"
#define READY2 "/tmp/a11-token-ready2"
#define READY3 "/tmp/a11-token-ready3"
#define ACK    "/tmp/a11-token-ack"
#define PATH   "/tests/w32a11_payload.txt"
#define BYTES  "W32A11-DROP-FILE-CONTENTS\n"

static int signal_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int n = (int)strlen(text);
    int ok = write(fd, text, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}
static int await_file(const char *path) {
    for (int i = 0; i < 100; ++i) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { close(fd); return 0; }
        usleep(100000);
    }
    return -1;
}
static int await_token(int wid, uint16_t *token) {
    for (int i = 0; i < 100; ++i) {
        ag_event_t event;
        int rc;
        while ((rc = ag_poll_event(wid, &event)) > 0) {
            if (event.type == AG_EVT_DROP) {
                *token = event.data;
                return *token ? 0 : -1;
            }
        }
        if (rc < 0) return -1;
        usleep(100000);
    }
    return -1;
}
static int create_under_pointer(void) {
    /* Global mouse queries require ownership of at least one GUI window. */
    int wid = ag_window_create(0, 0, 160, 90, "token receiver",
                               AG_WIN_NO_DECOR | AG_WIN_BORDERLESS);
    if (wid < 0) return -1;
    int32_t mx = 0, my = 0;
    if (ag_mouse_position(&mx, &my) != 0 ||
        ag_window_move(wid, mx > 40 ? mx - 40 : mx,
                           my > 20 ? my - 20 : my) != 0 ||
        ag_window_show(wid) != 0) {
        ag_window_destroy(wid);
        return -1;
    }
    return wid;
}
int main(void) {
    char path[AG_DROP_PATH_MAX];
    int first = create_under_pointer();
    if (first < 0 || signal_file(READY1, "ready\n") != 0) goto fail;
    uint16_t old = 0;
    if (await_token(first, &old) != 0) goto fail;
    /* Do not take old: destroying the window must invalidate a still-busy
     * pathname slot and must not reset its generation on HWND reuse. */
    if (ag_window_destroy(first) != 0) goto fail;
    int second = create_under_pointer();
    if (second != first || signal_file(READY2, "ready\n") != 0) goto fail;
    uint16_t fresh = 0;
    if (await_token(second, &fresh) != 0 || fresh == old) goto fail;
    memset(path, 'X', sizeof path);
    if (ag_take_file_drop(second, old, path) == 0 || path[0] != 'X') goto fail;
    printf("A11-TOKEN-REUSE-OK\n");
    char share[64];
    snprintf(share, sizeof share, "%d %u\n", second, (unsigned)fresh);
    if (signal_file(READY3, share) != 0 || await_file(ACK) != 0) goto fail;
    /* A cross-task attempt must not have consumed our live token. */
    if (ag_take_file_drop(second, fresh, path) != 0 || strcmp(path, PATH) != 0)
        goto fail;
    if (ag_take_file_drop(second, fresh, path) == 0) goto fail;
    char content[sizeof BYTES];
    int fd = open(path, O_RDONLY);
    if (fd < 0) goto fail;
    int got = (int)read(fd, content, sizeof BYTES - 1);
    close(fd);
    if (got != sizeof BYTES - 1 || memcmp(content, BYTES, sizeof BYTES - 1))
        goto fail;
    ag_window_destroy(second);
    printf("A11-TOKEN-OK\n");
    return 78;
fail:
    printf("A11-TOKEN-FAIL: receiver\n");
    return 79;
}
