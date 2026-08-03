#ifndef _NET_IF_H
#define _NET_IF_H

#include <sys/socket.h>

#define IF_NAMESIZE 16

struct if_nameindex {
    unsigned if_index;
    char    *if_name;
};

unsigned if_nametoindex(const char *ifname);
char    *if_indextoname(unsigned ifindex, char *ifname);
struct if_nameindex *if_nameindex(void);
void     if_freenameindex(struct if_nameindex *ptr);

#endif /* _NET_IF_H */
