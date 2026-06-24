/* socket.c — small process-owned socket table for AuraLite.
 *
 * The first implementation exposes socket-style handles and ownership checks
 * while reusing the existing tcp.c transport.  tcp.c is still a single active
 * TCP connection internally; this layer is the compatibility bridge that lets
 * userspace stop depending on anonymous global net_* calls.
 */

#include <stdint.h>
#include "kernel/net/socket.h"
#include "kernel/net/tcp.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define SOCKET_MAX 32

typedef enum {
    SOCK_SLOT_FREE = 0,
    SOCK_SLOT_OPEN,
    SOCK_SLOT_CONNECTED,
} socket_state_t;

typedef struct {
    socket_state_t state;
    uint64_t owner_pid;
    int domain;
    int type;
    int protocol;
    uint32_t peer_ip;
    uint16_t peer_port;
} socket_t;

static socket_t sockets[SOCKET_MAX];
static int active_tcp_sid = -1;

static uint64_t current_pid(void) {
    tcb_t *cur = sched_current();
    return cur ? cur->id : 0;
}

static socket_t *get_owned_socket(int sid) {
    if (sid < 0 || sid >= SOCKET_MAX) return 0;
    socket_t *s = &sockets[sid];
    if (s->state == SOCK_SLOT_FREE) return 0;
    if (s->owner_pid != current_pid()) return 0;
    return s;
}

int64_t socket_create(int domain, int type, int protocol) {
    if (domain != AURA_AF_INET || type != AURA_SOCK_STREAM) return -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].state == SOCK_SLOT_FREE) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].state = SOCK_SLOT_OPEN;
            sockets[i].owner_pid = current_pid();
            sockets[i].domain = domain;
            sockets[i].type = type;
            sockets[i].protocol = protocol;
            return i;
        }
    }
    return -1;
}

int64_t socket_connect(int sid, uint32_t ip, uint16_t port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_OPEN) return -1;
    if (active_tcp_sid >= 0 && active_tcp_sid != sid) {
        kprintf("[socket] TCP transport busy (active socket %d)\n", active_tcp_sid);
        return -1;
    }
    if (tcp_connect(ip, port) != 0) return -1;
    s->state = SOCK_SLOT_CONNECTED;
    s->peer_ip = ip;
    s->peer_port = port;
    active_tcp_sid = sid;
    return 0;
}

int64_t socket_send(int sid, const void *buf, uint32_t len) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_CONNECTED) return -1;
    if (active_tcp_sid != sid) return -1;
    return tcp_send(buf, len);
}

int64_t socket_recv(int sid, void *buf, uint32_t len) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_CONNECTED) return -1;
    if (active_tcp_sid != sid) return -1;
    return tcp_recv(buf, len);
}

int64_t socket_close(int sid) {
    socket_t *s = get_owned_socket(sid);
    if (!s) return -1;
    if (s->state == SOCK_SLOT_CONNECTED && active_tcp_sid == sid) {
        tcp_close();
        active_tcp_sid = -1;
    }
    memset(s, 0, sizeof(*s));
    return 0;
}

void socket_close_process(uint64_t owner_pid) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].state == SOCK_SLOT_FREE) continue;
        if (sockets[i].owner_pid != owner_pid) continue;
        if (sockets[i].state == SOCK_SLOT_CONNECTED && active_tcp_sid == i) {
            tcp_close();
            active_tcp_sid = -1;
        }
        memset(&sockets[i], 0, sizeof(sockets[i]));
    }
}
