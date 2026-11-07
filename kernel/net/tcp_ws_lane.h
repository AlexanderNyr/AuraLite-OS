#ifndef AURALITE_NET_TCP_WS_LANE_H
#define AURALITE_NET_TCP_WS_LANE_H

#include <stdint.h>

/*
 * The 1 MiB window-scaling lane (RESIDUE3 C7).
 *
 * WHY A BOOT LANE AND NOT A UNIT TEST.  test_tcp_wscale.c pins the arithmetic
 * of kernel/net/tcp_wscale.h, and that is necessary but not sufficient: it
 * cannot show that tcp.c actually carries a scaled window to a peer, nor that
 * the send scheduler spends it.  This lane moves a megabyte across a real TCP
 * connection to a host peer and prints what the connection did.
 *
 * WHY THE PORT COMES FROM fw_cfg.  A lane that always runs would slow every
 * boot and fail noisily whenever no peer is listening, and a lane that runs
 * "when it feels like it" is a silent gap.  The harness that wants this
 * measurement says so on the QEMU command line:
 *
 *     -fw_cfg name=opt/auralite.tcp_ws,string=18099,18100
 *
 * two ports: the open lane and its negative control (a peer whose window is
 * clamped to 16 bits, so it can never grant more than 65535).  Absent the
 * option, the lane does not run and prints nothing at all.
 */

/* Returns 0 when both lanes behaved as their configuration demands, -1
 * otherwise.  Never called unless fw_cfg named the ports. */
int tcp_ws_lane_run(uint16_t big_port, uint16_t clamp_port);

/* fw_cfg probe: reads opt/auralite.tcp_ws and runs the lane when present. */
void tcp_ws_lane_boot_probe(void);

#endif /* AURALITE_NET_TCP_WS_LANE_H */
