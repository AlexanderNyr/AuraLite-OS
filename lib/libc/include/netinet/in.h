#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    uint16_t        sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define INADDR_ANY       0x00000000
#define INADDR_LOOPBACK  0x7f000001
#define INADDR_BROADCAST 0xffffffff

#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

#endif /* _NETINET_IN_H */