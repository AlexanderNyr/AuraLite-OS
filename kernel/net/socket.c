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
#include "kernel/net/net.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define SOCKET_MAX 32

typedef enum {
    SOCK_SLOT_FREE = 0,
    SOCK_SLOT_OPEN,
    SOCK_SLOT_BOUND,
    SOCK_SLOT_LISTENING,
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
    uint32_t local_ip;
    uint16_t local_port;
    int tcp_handle;            /* per-connection TCP handle, -1 if none */
} socket_t;

static socket_t sockets[SOCKET_MAX];
static uint16_t next_udp_ephemeral = 49152;

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
    if (domain != AURA_AF_INET) return -1;
    if (type != AURA_SOCK_STREAM && type != AURA_SOCK_DGRAM) return -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].state == SOCK_SLOT_FREE) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].state = SOCK_SLOT_OPEN;
            sockets[i].owner_pid = current_pid();
            sockets[i].domain = domain;
            sockets[i].type = type;
            sockets[i].protocol = protocol;
            sockets[i].tcp_handle = -1;
            return i;
        }
    }
    return -1;
}

int64_t socket_connect(int sid, uint32_t ip, uint16_t port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_OPEN) return -1;
    if (s->type != AURA_SOCK_STREAM) return -1;
    int h = tcp_open(ip, port);
    if (h < 0) return -1;
    s->state = SOCK_SLOT_CONNECTED;
    s->peer_ip = ip;
    s->peer_port = port;
    s->tcp_handle = h;
    return 0;
}

int64_t socket_send(int sid, const void *buf, uint32_t len) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_CONNECTED || s->type != AURA_SOCK_STREAM) return -1;
    return tcp_send_h(s->tcp_handle, buf, len);
}

int64_t socket_recv(int sid, void *buf, uint32_t len) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_CONNECTED || s->type != AURA_SOCK_STREAM) return -1;
    return tcp_recv_h(s->tcp_handle, buf, len);
}

int64_t socket_close(int sid) {
    socket_t *s = get_owned_socket(sid);
    if (!s) return -1;
    if ((s->state == SOCK_SLOT_CONNECTED || s->state == SOCK_SLOT_LISTENING) && s->tcp_handle >= 0) {
        tcp_close_h(s->tcp_handle);
    }
    memset(s, 0, sizeof(*s));
    s->tcp_handle = -1;
    return 0;
}

int64_t socket_bind(int sid, uint32_t ip, uint16_t port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_OPEN) return -1;
    s->state = SOCK_SLOT_BOUND;
    s->local_ip = ip;
    s->local_port = port;
    s->peer_ip = ip;
    s->peer_port = port;
    return 0;
}

int64_t socket_listen(int sid, int backlog) {
    (void)backlog;
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_BOUND || s->type != AURA_SOCK_STREAM) return -1;
    int h = tcp_listen(s->local_port);
    if (h < 0) return -1;
    s->state = SOCK_SLOT_LISTENING;
    s->tcp_handle = h;
    return 0;
}

int64_t socket_accept(int sid, uint32_t *ip, uint16_t *port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->state != SOCK_SLOT_LISTENING || s->type != AURA_SOCK_STREAM) return -1;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int new_h = tcp_accept(s->tcp_handle, &peer_ip, &peer_port);
    if (new_h < 0) return -1;

    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].state == SOCK_SLOT_FREE) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].state = SOCK_SLOT_CONNECTED;
            sockets[i].owner_pid = current_pid();
            sockets[i].domain = s->domain;
            sockets[i].type = s->type;
            sockets[i].protocol = s->protocol;
            sockets[i].peer_ip = peer_ip;
            sockets[i].peer_port = peer_port;
            sockets[i].tcp_handle = new_h;
            if (ip) *ip = peer_ip;
            if (port) *port = peer_port;
            return i;
        }
    }
    tcp_close_h(new_h);
    return -1;
}


static uint16_t udp_auto_bind(socket_t *s) {
    if (s->local_port != 0) return s->local_port;
    uint16_t port = next_udp_ephemeral++;
    if (next_udp_ephemeral < 49152) next_udp_ephemeral = 49152;
    s->local_port = port;
    if (s->state == SOCK_SLOT_OPEN) s->state = SOCK_SLOT_BOUND;
    return port;
}

int64_t socket_sendto(int sid, const void *buf, uint32_t len,
                      uint32_t dst_ip, uint16_t dst_port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->type != AURA_SOCK_DGRAM) return -1;
    if (s->state != SOCK_SLOT_OPEN && s->state != SOCK_SLOT_BOUND &&
        s->state != SOCK_SLOT_CONNECTED) return -1;
    uint16_t src_port = udp_auto_bind(s);
    if (net_udp_sendto(dst_ip, dst_port, src_port, buf, len) != 0) return -1;
    s->peer_ip = dst_ip;
    s->peer_port = dst_port;
    return (int64_t)len;
}

int64_t socket_recvfrom(int sid, void *buf, uint32_t len,
                        uint32_t *src_ip, uint16_t *src_port) {
    socket_t *s = get_owned_socket(sid);
    if (!s || s->type != AURA_SOCK_DGRAM) return -1;
    if (s->state != SOCK_SLOT_OPEN && s->state != SOCK_SLOT_BOUND &&
        s->state != SOCK_SLOT_CONNECTED) return -1;
    uint16_t local_port = udp_auto_bind(s);
    return net_udp_recvfrom(local_port, src_ip, src_port, buf, len, 200);
}

void socket_close_process(uint64_t owner_pid) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].state == SOCK_SLOT_FREE) continue;
        if (sockets[i].owner_pid != owner_pid) continue;
        if ((sockets[i].state == SOCK_SLOT_CONNECTED || sockets[i].state == SOCK_SLOT_LISTENING) && sockets[i].tcp_handle >= 0) {
            tcp_close_h(sockets[i].tcp_handle);
        }
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].tcp_handle = -1;
    }
}
