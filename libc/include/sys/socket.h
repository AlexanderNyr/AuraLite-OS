#ifndef AURALITE_LIBC_SYS_SOCKET_H
#define AURALITE_LIBC_SYS_SOCKET_H

#include "libc/include/sys/types.h"
#include "libc/include/netinet/in.h"

/* Address families. */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    10

#define PF_INET     AF_INET

/* Socket types. */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/* setsockopt levels / options. */
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_ERROR     4

/* send/recv flags. */
#define MSG_PEEK     0x02
#define MSG_DONTWAIT 0x40

typedef unsigned int socklen_t;

struct sockaddr {
    unsigned short sa_family;
    char           sa_data[14];
};

/* The AuraLite socket primitives socket()/connect()/send()/recv()/
 * closesocket() use kernel-specific signatures and are declared in
 * <unistd.h>.  The POSIX-shaped server-side calls live here. */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen);

#endif /* AURALITE_LIBC_SYS_SOCKET_H */
