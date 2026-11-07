/* test_tcp_wscale.c — host pin for kernel/net/tcp_wscale.h (RESIDUE3 C7).
 *
 * Window scaling is arithmetic on bytes that arrive from the network, so the
 * cases that matter are the ones a happy-path implementation gets wrong:
 *
 *   - a peer that never offers a shift (the window must stay unscaled),
 *   - a peer that offers shift 15..255 (clamped to 14, not trusted),
 *   - a WSCALE option with the wrong length (ignored, not acted on),
 *   - an option block that ends mid-option (parsed, reported malformed),
 *   - a zero-length option (must not spin),
 *   - a receive buffer smaller than one window (shift 0, no promise).
 *
 * Build: cc -std=c11 -Wall -Wextra -Werror -I . tests/unit/test_tcp_wscale.c
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/net/tcp_wscale.h"

static int checks = 0, fails = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fails++;                                                       \
            printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
        }                                                                  \
    } while (0)

static void t_parse_absent(void) {
    printf("parse: no WSCALE offered\n");
    tcp_ws_opt_t o;
    /* MSS only, as SLIRP sends: kind 2, len 4, mss 1460, END. */
    const uint8_t opts[] = { 2, 4, 0x05, 0xB4, 0 };
    CHECK(tcp_ws_parse(opts, sizeof opts, &o) == 0, "well-formed block");
    CHECK(o.offered == 0, "offered stays 0");
    CHECK(o.shift == 0, "shift stays 0");

    /* Empty block: legal (a SYN may carry no options at all). */
    CHECK(tcp_ws_parse(NULL, 0, &o) == 0, "empty block is well-formed");
    CHECK(o.offered == 0, "empty block offers nothing");
}

static void t_parse_present(void) {
    printf("parse: WSCALE present\n");
    tcp_ws_opt_t o;
    /* MSS, NOP, NOP, WS(3,3,7), END -- a real Linux SYN layout. */
    const uint8_t opts[] = { 2, 4, 0x05, 0xB4, 1, 1, 3, 3, 7, 0 };
    CHECK(tcp_ws_parse(opts, sizeof opts, &o) == 0, "well-formed");
    CHECK(o.offered == 1, "offered");
    CHECK(o.shift == 7, "shift 7 read from the wire");

    /* shift 0 is a legal, meaningful value: the peer scales, by nothing. */
    const uint8_t zero[] = { 3, 3, 0, 1 };
    CHECK(tcp_ws_parse(zero, sizeof zero, &o) == 0, "well-formed");
    CHECK(o.offered == 1 && o.shift == 0, "shift 0 is offered, not absent");
}

static void t_parse_hostile(void) {
    printf("parse: hostile option blocks\n");
    tcp_ws_opt_t o;

    /* Shift beyond the RFC's 14 bits: clamp, never trust. */
    const uint8_t big[] = { 3, 3, 255, 1 };
    tcp_ws_parse(big, sizeof big, &o);
    CHECK(o.offered == 1 && o.shift == TCP_WS_SHIFT_MAX,
          "shift 255 clamps to %u", TCP_WS_SHIFT_MAX);

    /* Right kind, wrong length: the bytes after it are not a shift count. */
    const uint8_t badlen[] = { 3, 4, 7, 0, 1 };
    CHECK(tcp_ws_parse(badlen, sizeof badlen, &o) == -1, "reports malformed");
    CHECK(o.offered == 0, "does not act on a malformed WSCALE");

    /* Ends mid-option: parse what is there, say it was malformed. */
    const uint8_t trunc[] = { 2, 4, 0x05 };
    CHECK(tcp_ws_parse(trunc, sizeof trunc, &o) == -1, "truncated option");
    CHECK(o.offered == 0, "nothing offered");

    /* A zero-length option must not spin: this call returning at all is the
     * assertion, and the WSCALE after it must still be found. */
    const uint8_t zerolen[] = { 2, 1, 3, 3, 5, 1 };
    CHECK(tcp_ws_parse(zerolen, sizeof zerolen, &o) == -1,
          "zero-length option reported malformed");
    CHECK(o.offered == 1 && o.shift == 5, "parser still reaches the WSCALE");

    /* All NOPs: consumable, no option. */
    const uint8_t nops[] = { 1, 1, 1, 1 };
    CHECK(tcp_ws_parse(nops, sizeof nops, &o) == 0, "NOPs are well-formed");
    CHECK(o.offered == 0, "NOPs offer nothing");
}

static void t_emit(void) {
    printf("emit: the option we put on the wire\n");
    uint8_t buf[8];
    memset(buf, 0xAA, sizeof buf);
    uint32_t n = tcp_ws_emit(buf, sizeof buf, 7);
    CHECK(n == 4, "4 bytes written (option + NOP padding)");
    CHECK(buf[0] == 3 && buf[1] == 3 && buf[2] == 7, "kind/length/shift");
    CHECK(buf[3] == 1, "padded to the 4-byte boundary");
    CHECK(buf[4] == 0xAA, "nothing written past the option");

    /* Not enough room: refuse rather than emit a truncated option. */
    memset(buf, 0xAA, sizeof buf);
    CHECK(tcp_ws_emit(buf, 3, 7) == 0, "refuses when it cannot fit");
    CHECK(buf[0] == 0xAA, "buffer untouched on refusal");

    /* Our own emit must round-trip through our own parser, with padding. */
    tcp_ws_opt_t o;
    memset(buf, 0, sizeof buf);
    n = tcp_ws_emit(buf, sizeof buf, 12);
    CHECK(tcp_ws_parse(buf, n, &o) == 0, "our own option block parses");
    CHECK(o.offered == 1 && o.shift == 12, "shift survives the round trip");
}

static void t_apply_encode(void) {
    printf("apply/encode: field <-> real window\n");
    CHECK(tcp_ws_apply(65535, 0) == 65535, "shift 0 is the identity");
    CHECK(tcp_ws_apply(65535, 7) == 65535u * 128u, "65535 << 7");
    CHECK(tcp_ws_apply(0, 7) == 0, "a zero window stays zero (peer is closed)");
    CHECK(tcp_ws_apply(0xFFFFFFFFu, 14) == 0xFFFFFFFFu, "saturates, no wrap");
    CHECK(tcp_ws_apply(100, 200) == tcp_ws_apply(100, TCP_WS_SHIFT_MAX),
          "out-of-range shift clamps");

    CHECK(tcp_ws_encode(64240, 0) == 64240, "unscaled encode");
    CHECK(tcp_ws_encode(64240u * 128u, 7) == 64240, "scaled encode");
    CHECK(tcp_ws_encode(0xFFFFFFFFu, 0) == 0xFFFF, "clamps at 16 bits");
    CHECK(tcp_ws_encode(1u << 20, 14) == 64, "1 MiB >> 14");

    /* The property that makes scaling worth having: the same 16-bit field
     * means different windows depending on the negotiated shift. */
    CHECK(tcp_ws_apply(65024, 0) == 65024, "unscaled field");
    CHECK(tcp_ws_apply(65024, 7) == 65024u * 128u, "scaled field is 128x");
    CHECK(tcp_ws_apply(65024, 7) > 64240, "and it clears the 64240 ceiling");
}

static void t_shift_for_buffer(void) {
    printf("negotiation: the shift we may honestly ask for\n");
    CHECK(tcp_ws_shift_for_buffer(0) == 0, "no buffer -> no scale");
    CHECK(tcp_ws_shift_for_buffer(1024) == 0, "tiny buffer -> no scale");
    CHECK(tcp_ws_shift_for_buffer(65535) == 0, "one window -> no scale");
    CHECK(tcp_ws_shift_for_buffer(65535) == 0, "exactly one window -> 0");
    CHECK(tcp_ws_shift_for_buffer(65536) == 0,
          "a hair over one window still -> 0 (65535 fits)");
    CHECK(tcp_ws_shift_for_buffer(131070) == 1, "two windows -> 1");
    CHECK(tcp_ws_shift_for_buffer(131069) == 0,
          "one byte short of two windows -> 0 (floor, never round up)");
    CHECK(tcp_ws_shift_for_buffer(256u * 1024u) == 2, "256 KiB -> shift 2");
    CHECK(tcp_ws_shift_for_buffer(1u << 20) == 4, "1 MiB -> shift 4");
    CHECK(tcp_ws_shift_for_buffer(0xFFFFFFFFu) == TCP_WS_SHIFT_MAX,
          "clamps at 14");

    printf("negotiation: when scaling is in force\n");
    tcp_ws_opt_t o;
    const uint8_t ws6[] = { 3, 3, 6, 1 };
    tcp_ws_parse(ws6, sizeof ws6, &o);
    CHECK(tcp_ws_engaged(1, &o) == 1, "we offered, peer offered -> engaged");
    CHECK(tcp_ws_engaged(0, &o) == 0, "we did not offer -> not engaged");
    tcp_ws_opt_init(&o);
    CHECK(tcp_ws_engaged(1, &o) == 0, "peer did not offer -> not engaged");
    CHECK(tcp_ws_engaged(1, NULL) == 0, "no peer answer -> not engaged");
}

static void t_graceful(void) {
    printf("graceful: a peer that omits the option\n");
    /* The RFC's own guidance: if the peer does not offer a shift, the
     * connection still works, it just keeps the 16-bit ceiling.  Modelled
     * here as a field -> window mapping that never exceeds 65535. */
    tcp_ws_opt_t o;
    tcp_ws_opt_init(&o);
    uint8_t shift = tcp_ws_engaged(1, &o) ? o.shift : 0;
    CHECK(shift == 0, "no shift is applied");
    CHECK(tcp_ws_apply(65535, shift) == 65535, "window stays 16-bit");
    CHECK(tcp_ws_apply(65535, shift) <= 64240u + 1295u,
          "and never clears the old ceiling (that is the point)");
}

int main(void) {
    printf("=== AuraLite OS TCP window scaling tests (RESIDUE3 C7) ===\n\n");
    t_parse_absent();
    t_parse_present();
    t_parse_hostile();
    t_emit();
    t_apply_encode();
    t_shift_for_buffer();
    t_graceful();
    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
