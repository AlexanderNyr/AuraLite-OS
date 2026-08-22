/* netglue32.h -- RESIDUE R3: shared TCP over the netdev seam on i386. */
#ifndef AURALITE_ARCH_I386_NETGLUE32_H
#define AURALITE_ARCH_I386_NETGLUE32_H

/* Register net32's rings with the netdev seam, init the shared TCP,
 * run one honest round-trip (or print the honest skip). */
void net32_tcp_bringup(void);

#endif /* AURALITE_ARCH_I386_NETGLUE32_H */
