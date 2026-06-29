#ifndef AURALITE_LIBC_ARPA_INET_H
#define AURALITE_LIBC_ARPA_INET_H

#include "libc/include/netinet/in.h"
#include "libc/include/sys/socket.h"

/* htons/htonl/ntohs/ntohl are provided as macros by <netinet/in.h>. */

int         inet_aton(const char *cp, struct in_addr *inp);
char       *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);
in_addr_t   inet_addr(const char *cp);

#endif /* AURALITE_LIBC_ARPA_INET_H */
