/*
 * test_gpu_syscall.c — host-side unit tests for the GPU command-stream
 * validator (phase K1 of GL_PLAN.md).
 *
 * WHY THIS TEST EXISTS SEPARATELY
 *
 * gpu_validate_cmd_stream() is the single function standing between a hostile
 * user process and the host GPU.  Everything else in the syscall path can be
 * exercised in QEMU, but a validator deserves direct, exhaustive testing with
 * deliberately malformed input — the cases that matter are precisely the ones
 * a normal run never produces.
 *
 * The validator is pure (no kernel state, no allocation), so it is compiled
 * standalone here.  A small shim supplies the handful of declarations it needs
 * without dragging in the rest of the kernel.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The function under test, copied in by declaration only — the real
 * implementation is linked from kernel/gpu/gpu_syscalls.c via the Makefile
 * rule, so this test cannot drift from the shipping code. */
int gpu_validate_cmd_stream(const uint32_t *cmd, uint32_t dwords);

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

/* Build a VirGL packet header: the low 16 bits are the payload length in
 * dwords, not counting the header itself. */
static uint32_t hdr(uint32_t opcode, uint32_t len_dwords) {
    return ((opcode & 0xFFu) << 24) | (len_dwords & 0xFFFFu);
}

/* ------------------------------------------------------------ well-formed - */

/* A single empty packet: header only, no payload. */
static int t_single_empty_packet(void) {
    uint32_t s[1] = { hdr(1, 0) };
    return gpu_validate_cmd_stream(s, 1) == 0;
}

static int t_single_packet_with_payload(void) {
    uint32_t s[4] = { hdr(4, 3), 0xAAAA, 0xBBBB, 0xCCCC };
    return gpu_validate_cmd_stream(s, 4) == 0;
}

static int t_several_packets(void) {
    uint32_t s[8] = {
        hdr(1, 1), 0x1111,
        hdr(2, 0),
        hdr(3, 3), 0x2222, 0x3333, 0x4444,
        hdr(5, 0),
    };
    return gpu_validate_cmd_stream(s, 8) == 0;
}

/* A realistic clear-then-draw shape. */
static int t_realistic_stream(void) {
    uint32_t s[16] = {
        hdr(4, 8),                                   /* CLEAR              */
        0x1, 0, 0, 0, 0x3F800000, 0, 0, 0,
        hdr(5, 4),                                   /* DRAW_VBO           */
        4, 0, 3, 0,
        hdr(3, 1),                                   /* SET_FRAMEBUFFER    */
        1,
    };
    return gpu_validate_cmd_stream(s, 16) == 0;
}

/* ---------------------------------------------------------- malformed ----- */

/* THE attack: a length that claims more than the buffer holds. */
static int t_length_overruns_buffer(void) {
    uint32_t s[4] = { hdr(1, 100), 0, 0, 0 };
    return gpu_validate_cmd_stream(s, 4) != 0;
}

/* Off by exactly one — the boundary a naive `<` versus `<=` gets wrong. */
static int t_length_overruns_by_one(void) {
    uint32_t s[4] = { hdr(1, 3), 0, 0 };   /* needs 4 dwords, only 3 given */
    return gpu_validate_cmd_stream(s, 3) != 0;
}

/* Exactly filling the buffer must PASS: this is the other side of the same
 * boundary, and rejecting it would break every legitimate stream. */
static int t_length_exactly_fills(void) {
    uint32_t s[4] = { hdr(1, 3), 0, 0, 0 };
    return gpu_validate_cmd_stream(s, 4) == 0;
}

/* The maximum encodable length, on a buffer far too small for it. */
static int t_max_length_field(void) {
    uint32_t s[2] = { hdr(1, 0xFFFF), 0 };
    return gpu_validate_cmd_stream(s, 2) != 0;
}

/* A valid first packet followed by a lying second one: the stream must be
 * rejected whole, not accepted up to the good part. */
static int t_second_packet_lies(void) {
    uint32_t s[5] = { hdr(1, 1), 0x1111, hdr(2, 50), 0, 0 };
    return gpu_validate_cmd_stream(s, 5) != 0;
}

/* A length field near UINT16_MAX at the very end of the buffer, which is where
 * a 32-bit `i + 1 + len` would overflow and wrap to a small value.  The
 * validator computes in 64-bit precisely to survive this. */
static int t_no_integer_overflow(void) {
    uint32_t s[2] = { hdr(1, 0), hdr(2, 0xFFFF) };
    return gpu_validate_cmd_stream(s, 2) != 0;
}

/* ------------------------------------------------------------- degenerate - */

static int t_null_pointer(void) {
    return gpu_validate_cmd_stream(NULL, 4) != 0;
}

static int t_zero_length(void) {
    uint32_t s[1] = { 0 };
    /* An empty submission is meaningless; refusing it keeps the driver from
     * being handed a zero-byte command buffer. */
    return gpu_validate_cmd_stream(s, 0) != 0;
}

/* A buffer of all zeroes is a sequence of empty packets, which is
 * well-formed even though it does nothing. */
static int t_all_zero_stream(void) {
    uint32_t s[64];
    memset(s, 0, sizeof s);
    return gpu_validate_cmd_stream(s, 64) == 0;
}

/* A long run of empty packets must terminate rather than spinning: the
 * validator caps the packet count. */
static int t_many_empty_packets_terminate(void) {
    static uint32_t s[70000];
    memset(s, 0, sizeof s);
    /* 70 000 empty packets exceeds the 65 536 cap, so this is rejected — but
     * the point of the test is that it RETURNS at all. */
    int rc = gpu_validate_cmd_stream(s, 70000);
    return rc != 0;
}

/* Just under the cap must still be accepted. */
static int t_under_packet_cap(void) {
    static uint32_t s[60000];
    memset(s, 0, sizeof s);
    return gpu_validate_cmd_stream(s, 60000) == 0;
}

/* The opcode field is not interpreted by the validator: an unknown opcode with
 * an honest length is structurally valid, and rejecting it here would mean
 * teaching this function every VirGL command. */
static int t_unknown_opcode_accepted(void) {
    uint32_t s[3] = { hdr(0xFE, 2), 0, 0 };
    return gpu_validate_cmd_stream(s, 3) == 0;
}

/* A stream that is one giant packet exactly filling the buffer. */
static int t_one_large_packet(void) {
    static uint32_t s[1024];
    memset(s, 0, sizeof s);
    s[0] = hdr(7, 1023);
    return gpu_validate_cmd_stream(s, 1024) == 0;
}

/* ...and the same packet one dword short. */
static int t_one_large_packet_short(void) {
    static uint32_t s[1024];
    memset(s, 0, sizeof s);
    s[0] = hdr(7, 1024);          /* needs 1025 dwords */
    return gpu_validate_cmd_stream(s, 1024) != 0;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== gpu command-stream validator (K1) unit tests ===\n");

    printf("--- well-formed streams ---\n");
    RUN(t_single_empty_packet); RUN(t_single_packet_with_payload);
    RUN(t_several_packets); RUN(t_realistic_stream);
    RUN(t_length_exactly_fills); RUN(t_all_zero_stream);
    RUN(t_unknown_opcode_accepted); RUN(t_one_large_packet);
    RUN(t_under_packet_cap);

    printf("--- malformed streams ---\n");
    RUN(t_length_overruns_buffer); RUN(t_length_overruns_by_one);
    RUN(t_max_length_field); RUN(t_second_packet_lies);
    RUN(t_no_integer_overflow); RUN(t_one_large_packet_short);

    printf("--- degenerate input ---\n");
    RUN(t_null_pointer); RUN(t_zero_length);
    RUN(t_many_empty_packets_terminate);

    printf("\ntest_gpu_syscall: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}
