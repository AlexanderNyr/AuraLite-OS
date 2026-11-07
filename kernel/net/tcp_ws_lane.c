/* tcp_ws_lane.c — the 1 MiB window-scaling lane (RESIDUE3 C7).
 *
 * Moves a megabyte to a host peer over a real TCP connection and prints what
 * the connection did, twice:
 *
 *   BIG    the peer leaves its receive window large (a scaled window), so the
 *          send scheduler may put more than 65535 bytes in flight — which is
 *          impossible with an unscaled 16-bit window, and is therefore the
 *          proof that scaling is in force.
 *   CLAMP  the same peer clamps its window to 65535 (TCP_WINDOW_CLAMP), so
 *          scaling cannot help it.  The negative control: if the guest
 *          reports more than 65535 in flight here, the guest is inventing
 *          window and the measurement above means nothing.
 *
 * Both runs send the same payload to the same peer script; only the peer's
 * socket options differ.  See tests/integration/lib/paced_peer.py.
 */
#include "kernel/net/tcp_ws_lane.h"

#include <stdint.h>

#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/net/netl3.h"
#include "kernel/net/tcp.h"
#include "drivers/timer/pit.h"

#define LANE_BYTES   (1024u * 1024u)
#define LANE_CHUNK   16384u
/* The 16-bit ceiling an unscaled connection can never exceed. */
#define LANE_16BIT   65535u

/* fw_cfg, reached the way the rest of the portable tree reaches it: a local
 * extern declaration rather than an include of an arch header (the width
 * sweep's ratchet counts direct x86_64 includes from portable code, and it is
 * right to).  kernel/kernel.c does the same for the other probes. */
extern int fwcfg_read_opt(const char *wanted, char *out, int out_max);

/* 10.0.2.2 — SLIRP's host alias, the same peer net_self_test() pings. */
#define LANE_HOST_V4 0x0A000202u

static uint8_t lane_buf[LANE_CHUNK];

static int lane_once(const char *name, uint16_t port, int expect_scaled) {
    kprintf("[tcp] wscale lane (%s): connecting to 10.0.2.2:%u for %u bytes\n",
            name, (unsigned)port, (unsigned)LANE_BYTES);

    tcp_handle_t h = tcp_open(LANE_HOST_V4, port);
    if (h < 0) {
        kprintf("[tcp] wscale lane (%s): connect failed (%d)\n", name, h);
        return -1;
    }
    if (tcp_state_h(h) != TCP_ESTABLISHED) {
        kprintf("[tcp] wscale lane (%s): handshake did not complete (state %d)\n",
                name, (int)tcp_state_h(h));
        tcp_close_h(h);
        return -1;
    }

    uint8_t shift = tcp_snd_shift_h(h);
    uint32_t wnd = tcp_snd_wnd_h(h);
    kprintf("[tcp] wscale lane (%s): ESTABLISHED, peer shift %u, snd_wnd %u\n",
            name, (unsigned)shift, wnd);

    memset(lane_buf, 'A', sizeof(lane_buf));
    uint64_t t0 = timer_get_ticks();
    uint32_t sent = 0;
    while (sent < LANE_BYTES) {
        uint32_t chunk = LANE_BYTES - sent;
        if (chunk > LANE_CHUNK) chunk = LANE_CHUNK;
        int rc = tcp_send_h(h, lane_buf, chunk);
        if (rc <= 0) {
            kprintf("[tcp] wscale lane (%s): send failed at %u bytes (%d)\n",
                    name, sent, rc);
            tcp_close_h(h);
            return -1;
        }
        sent += (uint32_t)rc;
    }
    uint64_t t1 = timer_get_ticks();
    uint32_t peak = tcp_inflight_peak_h(h);
    uint32_t ms = (uint32_t)((t1 - t0) * (1000u / 100u));   /* PIT is 100 Hz */

    kprintf("[tcp] wscale lane (%s): sent %u bytes in %u ms, "
            "peak in-flight %u bytes, peer window %u (shift %u)\n",
            name, sent, ms, peak, tcp_snd_wnd_h(h), (unsigned)shift);

    int ok = 1;
    if (expect_scaled) {
        /* The whole point: more than a 16-bit window can express was in
         * flight, so the peer's advertised window was genuinely scaled. */
        if (peak > LANE_16BIT)
            kprintf("[tcp] wscale lane (%s): PASS %u bytes were in flight at "
                    "once — above the %u-byte unscaled ceiling\n",
                    name, peak, LANE_16BIT);
        else {
            kprintf("[tcp] wscale lane (%s): FAIL peak in-flight %u never "
                    "cleared the %u-byte unscaled ceiling\n",
                    name, peak, LANE_16BIT);
            ok = 0;
        }
    } else {
        /* Negative control: clamped peer, so nothing above 16 bits may
         * appear.  If it does, the BIG result is an artefact. */
        if (peak <= LANE_16BIT)
            kprintf("[tcp] wscale lane (%s): PASS peak in-flight %u stayed "
                    "inside the %u-byte window the peer actually granted\n",
                    name, peak, LANE_16BIT);
        else {
            kprintf("[tcp] wscale lane (%s): FAIL peak in-flight %u exceeds "
                    "the %u-byte window the peer granted — the guest is "
                    "inventing window\n", name, peak, LANE_16BIT);
            ok = 0;
        }
    }

    tcp_close_h(h);
    return ok ? 0 : -1;
}

int tcp_ws_lane_run(uint16_t big_port, uint16_t clamp_port) {
    uint64_t t0 = timer_get_ticks();
    int a = lane_once("big", big_port, 1);
    int b = lane_once("clamped", clamp_port, 0);
    uint32_t ms = (uint32_t)((timer_get_ticks() - t0) * (1000u / 100u));

    if (a == 0 && b == 0) {
        kprintf("[tcp] wscale lane: PASS 1 MiB over a scaled window, and a "
                "clamped peer held the same transfer inside 16 bits (%u ms "
                "total)\n", ms);
        return 0;
    }
    kprintf("[tcp] wscale lane: FAIL (big=%d clamped=%d)\n", a, b);
    return -1;
}

/* Read `opt/auralite.tcp_ws` = "<big>,<clamped>" and run the lane.  Absent
 * (or unparseable) means the lane does not run and prints nothing: a boot
 * that was not asked for this measurement must not pretend to have made it. */
void tcp_ws_lane_boot_probe(void) {
    char val[32];
    if (!fwcfg_read_opt("opt/auralite.tcp_ws", val, (int)sizeof(val))) return;

    uint16_t ports[2] = { 0, 0 };
    int n = 0;
    const char *p = val;
    while (*p && n < 2) {
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535u) return;         /* refuse, do not guess */
        ports[n++] = (uint16_t)v;
        if (*p == ',' || *p == ' ' || *p == ':') p++;
        else break;
    }
    if (n != 2) {
        kprintf("[tcp] wscale lane: fw_cfg opt/auralite.tcp_ws=%s is not "
                "big,clamped; lane not run\n", val);
        return;
    }
    kprintf("[tcp] wscale lane: asked for by fw_cfg (%s)\n", val);
    tcp_ws_lane_run(ports[0], ports[1]);
}
