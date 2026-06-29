#ifndef AURALITE_NET_SOCKET_H
#define AURALITE_NET_SOCKET_H

#include <stdint.h>

/* Minimal socket-style API over AuraLite's TCP client.
 *
 * This is intentionally small: AF_INET/SOCK_STREAM client sockets only.  It
 * replaces the public syscall surface that used to expose a single anonymous
 * global TCP connection.  Internally the TCP engine is still one-connection-at-a
 * time, so attempts to connect a second stream while one is established fail
 * with -1 until tcp.c is refactored into per-connection state objects.
 */

#define AURA_AF_INET      2
#define AURA_SOCK_STREAM  1

int64_t socket_create(int domain, int type, int protocol);
int64_t socket_connect(int sid, uint32_t ip, uint16_t port);
int64_t socket_send(int sid, const void *buf, uint32_t len);
int64_t socket_recv(int sid, void *buf, uint32_t len);
int64_t socket_close(int sid);
int64_t socket_bind(int sid, uint32_t ip, uint16_t port);
int64_t socket_listen(int sid, int backlog);
int64_t socket_accept(int sid, uint32_t *ip, uint16_t *port);
void    socket_close_process(uint64_t owner_pid);

#endif /* AURALITE_NET_SOCKET_H */
