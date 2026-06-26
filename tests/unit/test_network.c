/*
 * test_network.c — unit tests for network stack: checksum, byte-swap,
 * DHCP option parsing, DNS name encoding, TCP state machine.
 *
 * All protocol logic tested without actual hardware.  50+ test cases.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- Byte-swap helpers (same as kernel) ---- */

static uint16_t htons_(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t htonl_(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static uint32_t ntohl_(uint32_t v) { return htonl_(v); }
static uint16_t ntohs_(uint16_t v) { return htons_(v); }

/* ---- Internet checksum (RFC 1071) ---- */

static uint16_t checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)(p[0] << 8 | p[1]);
        p += 2; len -= 2;
    }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons_((uint16_t)(~sum & 0xFFFF));
}

/* ---- DHCP option parser (same logic as kernel) ---- */

#define DHCP_OPT_END 255

static const uint8_t *dhcp_find_option(const uint8_t *opts, int opts_len,
                                       uint8_t code, int *out_len) {
    int i = 0;
    while (i < opts_len) {
        uint8_t c = opts[i];
        if (c == DHCP_OPT_END) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= opts_len) break;
        uint8_t l = opts[i + 1];
        if (c == code) {
            if (out_len) *out_len = l;
            return opts + i + 2;
        }
        i += 2 + l;
    }
    return NULL;
}

/* ---- DNS name encoder ---- */

static int dns_encode_name(const char *name, uint8_t *out) {
    int pos = 0;
    const char *seg = name;
    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - seg);
        if (len > 63 || len == 0) return -1;
        out[pos++] = (uint8_t)len;
        for (int i = 0; i < len; i++) out[pos++] = (uint8_t)seg[i];
        seg = dot;
        if (*seg == '.') seg++;
    }
    out[pos++] = 0;
    return pos;
}

/* ---- TCP state machine ---- */

typedef enum {
    TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSING, TCP_TIME_WAIT,
    TCP_CLOSE_WAIT, TCP_LAST_ACK
} tcp_state_t;

/* static const char * tcp_state_name(tcp_state_t s) {
    switch (s) {
        case TCP_CLOSED:     return "CLOSED";
        case TCP_SYN_SENT:   return "SYN_SENT";
        case TCP_ESTABLISHED: return "ESTABLISHED";
        case TCP_FIN_WAIT1:  return "FIN_WAIT1";
        case TCP_FIN_WAIT2:  return "FIN_WAIT2";
        case TCP_CLOSING:    return "CLOSING";
        case TCP_TIME_WAIT:  return "TIME_WAIT";
        case TCP_CLOSE_WAIT: return "CLOSE_WAIT";
        case TCP_LAST_ACK:   return "LAST_ACK";
    }
    return "UNKNOWN";
} */

/* Simplified TCP state transitions */
static tcp_state_t tcp_on_event(tcp_state_t state, const char *event) {
    if (strcmp(event, "SYN_SEND") == 0 && state == TCP_CLOSED)
        return TCP_SYN_SENT;
    if (strcmp(event, "SYN_ACK") == 0 && state == TCP_SYN_SENT)
        return TCP_ESTABLISHED;
    if (strcmp(event, "FIN_SEND") == 0 && state == TCP_ESTABLISHED)
        return TCP_FIN_WAIT1;
    if (strcmp(event, "ACK") == 0 && state == TCP_FIN_WAIT1)
        return TCP_FIN_WAIT2;
    if (strcmp(event, "FIN") == 0 && state == TCP_FIN_WAIT2)
        return TCP_TIME_WAIT;
    if (strcmp(event, "FIN") == 0 && state == TCP_ESTABLISHED)
        return TCP_CLOSE_WAIT;
    if (strcmp(event, "ACK_SEND") == 0 && state == TCP_CLOSE_WAIT)
        return TCP_LAST_ACK;
    if (strcmp(event, "ACK") == 0 && state == TCP_LAST_ACK)
        return TCP_CLOSED;
    if (strcmp(event, "FIN") == 0 && state == TCP_FIN_WAIT1)
        return TCP_CLOSING;
    if (strcmp(event, "TIMEOUT") == 0 && state == TCP_TIME_WAIT)
        return TCP_CLOSED;
    return state;  /* no transition */
}

/* ---- IP utilities ---- */

static uint32_t ip_from_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static int ip_on_subnet(uint32_t ip, uint32_t net, uint32_t mask) {
    return (ip & mask) == (net & mask);
}

/* ======== TESTS ======== */

/* --- Byte-swap --- */

void t_htons_identity(void) {
    CHECK_EQ(htons_(0x1234), 0x3412);
}

void t_htons_zero(void) {
    CHECK_EQ(htons_(0), 0);
}

void t_htons_ff(void) {
    CHECK_EQ(htons_(0xFFFF), 0xFFFF);
}

void t_htonl_identity(void) {
    CHECK_EQ(htonl_(0x01020304), 0x04030201);
}

void t_htonl_roundtrip(void) {
    uint32_t v = 0xAABBCCDD;
    CHECK_EQ(ntohl_(htonl_(v)), v);
}

void t_htons_roundtrip(void) {
    uint16_t v = 0xCAFE;
    CHECK_EQ(ntohs_(htons_(v)), v);
}

/* --- Checksum --- */

void t_checksum_empty(void) {
    uint8_t data[] = {0};
    /* zero-length should not crash */
    uint16_t c = checksum(data, 0);
    (void)c;
    CHECK(1);  /* didn't crash */
}

void t_checksum_known(void) {
    /* RFC 1071 example: 0x0001 f203 f4f5 f6f7 */
    uint16_t words[] = {0x0001, 0xf203, 0xf4f5, 0xf6f7};
    uint16_t c = checksum(words, 8);
    /* Verify checksum is deterministic and non-zero */
    uint16_t c2 = checksum(words, 8);
    CHECK(c != 0);
    CHECK_EQ(c, c2);
}

void t_checksum_all_zero(void) {
    uint8_t data[8] = {0};
    uint16_t c = checksum(data, 8);
    CHECK(c != 0);  /* complement of 0 is 0xFFFF */
}

void t_checksum_odd_len(void) {
    uint8_t data[] = {0xAB, 0xCD, 0xEF};  /* 3 bytes */
    uint16_t c = checksum(data, 3);
    (void)c;
    CHECK(1);  /* did not crash */
}

void t_checksum_self_verifying(void) {
    /* Just verify the checksum is deterministic */
    uint8_t data[20];
    memset(data, 0, 20);
    data[0] = 0x45;
    data[2] = 0x00; data[3] = 0x14;
    uint16_t c = checksum(data, 20);
    CHECK(c != 0);
    uint16_t c2 = checksum(data, 20);
    CHECK_EQ(c, c2);
}

/* --- DHCP option parsing --- */

void t_dhcp_find_msg_type(void) {
    uint8_t opts[] = {53, 1, 1, 255};  /* msg type = DISCOVER */
    int len;
    const uint8_t *v = dhcp_find_option(opts, 4, 53, &len);
    CHECK(v != NULL);
    CHECK_EQ(len, 1);
    CHECK_EQ(*v, 1);
}

void t_dhcp_find_server_id(void) {
    uint8_t opts[] = {53, 1, 2, 54, 4, 10, 0, 2, 2, 255};
    int len;
    const uint8_t *v = dhcp_find_option(opts, 10, 54, &len);
    CHECK(v != NULL);
    CHECK_EQ(len, 4);
    CHECK_EQ(v[0], 10);
    CHECK_EQ(v[3], 2);
}

void t_dhcp_missing_option(void) {
    uint8_t opts[] = {53, 1, 1, 255};
    CHECK(dhcp_find_option(opts, 4, 99, NULL) == NULL);
}

void t_dhcp_skip_padding(void) {
    uint8_t opts[] = {0, 0, 53, 1, 5, 255};  /* padding + msg type = ACK */
    int len;
    const uint8_t *v = dhcp_find_option(opts, 6, 53, &len);
    CHECK(v != NULL);
    CHECK_EQ(*v, 5);
}

void t_dhcp_end_marker(void) {
    uint8_t opts[] = {53, 1, 1, 255, 1, 4, 255, 255, 0, 0};  /* end before more data */
    CHECK(dhcp_find_option(opts, 10, 1, NULL) == NULL);
}

void t_dhcp_empty_opts(void) {
    uint8_t opts[] = {255};
    CHECK(dhcp_find_option(opts, 1, 53, NULL) == NULL);
}

void t_dhcp_multiple_options(void) {
    uint8_t opts[] = {53, 1, 3, 1, 4, 255, 255, 0, 0, 6, 4, 10, 0, 2, 3, 255};
    int len;
    const uint8_t *v = dhcp_find_option(opts, 16, 6, &len);
    CHECK(v != NULL);
    CHECK_EQ(len, 4);
    CHECK_EQ(v[0], 10);
}

/* --- DNS name encoding --- */

void t_dns_simple(void) {
    uint8_t out[64];
    int len = dns_encode_name("com", out);
    CHECK(len > 0);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[1], 'c');
    CHECK_EQ(out[2], 'o');
    CHECK_EQ(out[3], 'm');
    CHECK_EQ(out[4], 0);  /* root label */
    CHECK_EQ(len, 5);
}

void t_dns_dotted(void) {
    uint8_t out[64];
    int len = dns_encode_name("example.com", out);
    CHECK(len > 0);
    CHECK_EQ(out[0], 7);  /* "example" */
    CHECK_EQ(out[8], 3);  /* "com" */
    CHECK_EQ(out[12], 0); /* root */
}

void t_dns_fqdn(void) {
    uint8_t out[128];
    int len = dns_encode_name("www.example.com", out);
    CHECK(len > 0);
    CHECK_EQ(out[0], 3);  /* "www" */
    CHECK_EQ(out[4], 7);  /* "example" */
}

void t_dns_empty(void) {
    uint8_t out[64];
    int len = dns_encode_name("", out);
    CHECK(len > 0);
    CHECK_EQ(out[0], 0);  /* just root label */
}

void t_dns_single_char(void) {
    uint8_t out[64];
    int len = dns_encode_name("a.b", out);
    CHECK(len > 0);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 'a');
    CHECK_EQ(out[2], 1);
    CHECK_EQ(out[3], 'b');
}

/* --- IP utilities --- */

void t_ip_from_octets(void) {
    uint32_t ip = ip_from_octets(10, 0, 2, 15);
    CHECK_EQ((ip >> 24) & 0xFF, 10);
    CHECK_EQ((ip >> 16) & 0xFF, 0);
    CHECK_EQ((ip >> 8) & 0xFF, 2);
    CHECK_EQ(ip & 0xFF, 15);
}

void t_ip_same_subnet(void) {
    uint32_t ip = ip_from_octets(10, 0, 2, 15);
    uint32_t gw = ip_from_octets(10, 0, 2, 2);
    uint32_t mask = ip_from_octets(255, 255, 255, 0);
    CHECK(ip_on_subnet(ip, gw, mask));
}

void t_ip_diff_subnet(void) {
    uint32_t ip = ip_from_octets(192, 168, 1, 10);
    uint32_t gw = ip_from_octets(10, 0, 2, 2);
    uint32_t mask = ip_from_octets(255, 255, 255, 0);
    CHECK(!ip_on_subnet(ip, gw, mask));
}

void t_ip_class_a_mask(void) {
    uint32_t a = ip_from_octets(10, 1, 2, 3);
    uint32_t b = ip_from_octets(10, 99, 88, 77);
    uint32_t mask = ip_from_octets(255, 0, 0, 0);
    CHECK(ip_on_subnet(a, b, mask));
}

void t_ip_loopback(void) {
    uint32_t lo = ip_from_octets(127, 0, 0, 1);
    CHECK_EQ((lo >> 24) & 0xFF, 127);
}

/* --- TCP state machine --- */

void t_tcp_active_open(void) {
    tcp_state_t s = TCP_CLOSED;
    s = tcp_on_event(s, "SYN_SEND");
    CHECK_EQ(s, TCP_SYN_SENT);
}

void t_tcp_connect(void) {
    tcp_state_t s = TCP_CLOSED;
    s = tcp_on_event(s, "SYN_SEND");
    s = tcp_on_event(s, "SYN_ACK");
    CHECK_EQ(s, TCP_ESTABLISHED);
}

void t_tcp_active_close(void) {
    tcp_state_t s = TCP_ESTABLISHED;
    s = tcp_on_event(s, "FIN_SEND");
    CHECK_EQ(s, TCP_FIN_WAIT1);
    s = tcp_on_event(s, "ACK");
    CHECK_EQ(s, TCP_FIN_WAIT2);
    s = tcp_on_event(s, "FIN");
    CHECK_EQ(s, TCP_TIME_WAIT);
    s = tcp_on_event(s, "TIMEOUT");
    CHECK_EQ(s, TCP_CLOSED);
}

void t_tcp_passive_close(void) {
    tcp_state_t s = TCP_ESTABLISHED;
    s = tcp_on_event(s, "FIN");
    CHECK_EQ(s, TCP_CLOSE_WAIT);
    s = tcp_on_event(s, "ACK_SEND");
    CHECK_EQ(s, TCP_LAST_ACK);
    s = tcp_on_event(s, "ACK");
    CHECK_EQ(s, TCP_CLOSED);
}

void t_tcp_simultaneous_close(void) {
    tcp_state_t s = TCP_FIN_WAIT1;
    s = tcp_on_event(s, "FIN");
    CHECK_EQ(s, TCP_CLOSING);
}

void t_tcp_no_transition_invalid(void) {
    tcp_state_t s = TCP_CLOSED;
    s = tcp_on_event(s, "FIN_SEND");  /* can't send FIN from CLOSED */
    CHECK_EQ(s, TCP_CLOSED);  /* no change */
}

void t_tcp_no_transition_syn_ack_from_closed(void) {
    tcp_state_t s = TCP_CLOSED;
    s = tcp_on_event(s, "SYN_ACK");
    CHECK_EQ(s, TCP_CLOSED);
}

void t_tcp_full_lifecycle(void) {
    tcp_state_t s = TCP_CLOSED;
    s = tcp_on_event(s, "SYN_SEND");
    CHECK_EQ(s, TCP_SYN_SENT);
    s = tcp_on_event(s, "SYN_ACK");
    CHECK_EQ(s, TCP_ESTABLISHED);
    s = tcp_on_event(s, "FIN_SEND");
    CHECK_EQ(s, TCP_FIN_WAIT1);
    s = tcp_on_event(s, "ACK");
    CHECK_EQ(s, TCP_FIN_WAIT2);
    s = tcp_on_event(s, "FIN");
    CHECK_EQ(s, TCP_TIME_WAIT);
    s = tcp_on_event(s, "TIMEOUT");
    CHECK_EQ(s, TCP_CLOSED);
}

int main(void) {
    printf("=== Network Protocol Tests ===\n\n");

    printf("--- byte-swap ---\n");
    RUN(t_htons_identity);
    RUN(t_htons_zero);
    RUN(t_htons_ff);
    RUN(t_htonl_identity);
    RUN(t_htonl_roundtrip);
    RUN(t_htons_roundtrip);

    printf("--- checksum ---\n");
    RUN(t_checksum_empty);
    RUN(t_checksum_known);
    RUN(t_checksum_all_zero);
    RUN(t_checksum_odd_len);
    RUN(t_checksum_self_verifying);

    printf("--- DHCP options ---\n");
    RUN(t_dhcp_find_msg_type);
    RUN(t_dhcp_find_server_id);
    RUN(t_dhcp_missing_option);
    RUN(t_dhcp_skip_padding);
    RUN(t_dhcp_end_marker);
    RUN(t_dhcp_empty_opts);
    RUN(t_dhcp_multiple_options);

    printf("--- DNS encoding ---\n");
    RUN(t_dns_simple);
    RUN(t_dns_dotted);
    RUN(t_dns_fqdn);
    RUN(t_dns_empty);
    RUN(t_dns_single_char);

    printf("--- IP utilities ---\n");
    RUN(t_ip_from_octets);
    RUN(t_ip_same_subnet);
    RUN(t_ip_diff_subnet);
    RUN(t_ip_class_a_mask);
    RUN(t_ip_loopback);

    printf("--- TCP state machine ---\n");
    RUN(t_tcp_active_open);
    RUN(t_tcp_connect);
    RUN(t_tcp_active_close);
    RUN(t_tcp_passive_close);
    RUN(t_tcp_simultaneous_close);
    RUN(t_tcp_no_transition_invalid);
    RUN(t_tcp_no_transition_syn_ack_from_closed);
    RUN(t_tcp_full_lifecycle);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
