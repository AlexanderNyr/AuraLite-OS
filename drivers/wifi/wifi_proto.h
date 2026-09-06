#ifndef AURALITE_DRIVERS_WIFI_WIFI_PROTO_H
#define AURALITE_DRIVERS_WIFI_WIFI_PROTO_H

/*
 * wifi_proto.h — the IEEE 802.11 wire protocol, pure and host-testable
 * (RESIDUE2 T6, ledger RES-39; the tcp_cc.h / bt_hci.h precedent).
 *
 * QEMU has no 802.11 radio to emulate, so a "real chipset backend" can
 * never be QEMU-gated (D2 loud-skip for silicon).  What CAN be proven:
 * the byte layouts and state-machine transitions this OS actually
 * speaks.  They live here as pure builders/parsers, pinned by
 * tests/unit/test_wifi_proto.c on the host, and wifi.c consumes them;
 * the reference backend (wifi_virt.c, a deterministic virtual AP)
 * drives the same parsers through the wifi_driver_t seam end to end.
 */

#include <stdint.h>

/* Freestanding-safe: no <string.h> here — the header is compiled into
 * both the kernel (clang -ffreestanding) and the host unit test. */
static inline void wifi_memset(void *dst, uint8_t v, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = v;
}
static inline void wifi_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

/* ---- constants (single source of truth; wifi.h includes this) -------- */

#define WIFI_MAX_SSID_LEN   32
#define WIFI_MAX_SCAN_RESULTS 32
#define WIFI_ETH_HDR_LEN    14

/* Frame Control field types. */
#define WIFI_FRAME_TYPE_MGMT    0x00
#define WIFI_FRAME_TYPE_CTRL    0x04
#define WIFI_FRAME_TYPE_DATA    0x08

/* Management subtypes. */
#define WIFI_MGMT_ASSOC_REQ     0x00
#define WIFI_MGMT_ASSOC_RESP    0x01
#define WIFI_MGMT_REASSOC_REQ   0x02
#define WIFI_MGMT_REASSOC_RESP  0x03
#define WIFI_MGMT_PROBE_REQ     0x04
#define WIFI_MGMT_PROBE_RESP    0x05
#define WIFI_MGMT_BEACON        0x08
#define WIFI_MGMT_AUTH          0x0B
#define WIFI_MGMT_DEAUTH        0x0C

/* Auth algorithm. */
#define WIFI_AUTH_OPEN          0
#define WIFI_AUTH_SHARED_KEY    1

/* Status codes. */
#define WIFI_STATUS_SUCCESS     0
#define WIFI_STATUS_UNSPEC      1

/* Information Element IDs. */
#define WIFI_IE_SSID            0
#define WIFI_IE_RATES           1
#define WIFI_IE_DS_PARAM        3
#define WIFI_IE_RSN             48   /* WPA2 */

/* Capability bits (beacon/assoc). */
#define WIFI_CAP_ESS            0x0001
#define WIFI_CAP_PRIVACY        0x0010

/* ---- 802.11 frame structures ------------------------------------------ */

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif

/* Frame Control (2 bytes). */
struct wifi_frame_ctrl {
    uint8_t  protocol    : 2;
    uint8_t  type        : 2;
    uint8_t  subtype     : 4;
    uint8_t  to_ds       : 1;
    uint8_t  from_ds     : 1;
    uint8_t  more_frag   : 1;
    uint8_t  retry       : 1;
    uint8_t  pwr_mgmt    : 1;
    uint8_t  more_data   : 1;
    uint8_t  protected_  : 1;
    uint8_t  order       : 1;
} __attribute__((packed));

/* Management frame header (24 bytes). */
struct wifi_mgmt_hdr {
    struct wifi_frame_ctrl fc;
    uint16_t duration;
    uint8_t  addr1[6];   /* destination / RA */
    uint8_t  addr2[6];   /* source / TA */
    uint8_t  addr3[6];   /* BSSID / DA */
    uint16_t seq_ctrl;
} __attribute__((packed));

/* Beacon / Probe Response fixed fields (12 bytes after the mgmt header). */
struct wifi_beacon_fixed {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capability;
} __attribute__((packed));

/* Authentication frame body (6 bytes). */
struct wifi_auth_body {
    uint16_t auth_alg;
    uint16_t auth_transaction;
    uint16_t status_code;
} __attribute__((packed));

/* Association Request fixed fields (4 bytes). */
struct wifi_assoc_req_body {
    uint16_t capability;
    uint16_t listen_interval;
} __attribute__((packed));

/* Association Response body (6 bytes). */
struct wifi_assoc_resp_body {
    uint16_t capability;
    uint16_t status_code;
    uint16_t aid;
} __attribute__((packed));

#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ---- parsed BSS description (scan output) ------------------------------ */

typedef struct {
    uint8_t  bssid[6];
    char     ssid[WIFI_MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    uint16_t capability;
    int      wpa2;
} wifi_bss_desc_t;

/* ---- builders ------------------------------------------------------------ */

/* The 8-rate set Probe Request and Association Request advertise. */
static const uint8_t wifi_proto_rates[8] =
    {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x18, 0x30, 0x60};

static inline void wifi_proto_mgmt_hdr(struct wifi_mgmt_hdr *h,
                                       uint8_t subtype,
                                       const uint8_t dst[6],
                                       const uint8_t src[6],
                                       const uint8_t bssid[6]) {
    wifi_memset(h, 0, sizeof(*h));
    h->fc.type = 0;             /* Mgmt */
    h->fc.subtype = subtype;
    wifi_memcpy(h->addr1, dst, 6);
    wifi_memcpy(h->addr2, src, 6);
    wifi_memcpy(h->addr3, bssid, 6);
}

/* Probe Request IEs: wildcard SSID + supported rates + DS channel.
 * Returns the IE block length. */
static inline int wifi_proto_probe_ies(uint8_t *buf, uint8_t channel) {
    int pos = 0;
    buf[pos++] = WIFI_IE_SSID;
    buf[pos++] = 0;
    buf[pos++] = WIFI_IE_RATES;
    buf[pos++] = 8;
    wifi_memcpy(buf + pos, wifi_proto_rates, 8);
    pos += 8;
    buf[pos++] = WIFI_IE_DS_PARAM;
    buf[pos++] = 1;
    buf[pos++] = channel;
    return pos;
}

/* Association Request IEs: SSID + supported rates. Returns block length. */
static inline int wifi_proto_assoc_ies(uint8_t *buf, const char *ssid,
                                       uint8_t ssid_len) {
    int pos = 0;
    buf[pos++] = WIFI_IE_SSID;
    buf[pos++] = ssid_len;
    wifi_memcpy(buf + pos, ssid, ssid_len);
    pos += ssid_len;
    buf[pos++] = WIFI_IE_RATES;
    buf[pos++] = 8;
    wifi_memcpy(buf + pos, wifi_proto_rates, 8);
    pos += 8;
    return pos;
}

/* ---- parsers (validate length; -1 on malformed) -------------------------- */

static inline int wifi_proto_is_mgmt(const uint8_t *frame, int len,
                                     uint8_t subtype_a, uint8_t subtype_b) {
    if (!frame || len < (int)sizeof(struct wifi_mgmt_hdr)) return 0;
    const struct wifi_mgmt_hdr *h = (const struct wifi_mgmt_hdr *)frame;
    if (h->fc.type != 0) return 0;
    return h->fc.subtype == subtype_a || h->fc.subtype == subtype_b;
}

/* Parse a Beacon or Probe Response into a BSS description.
 * The SSID is NUL-terminated in out->ssid; wpa2 is set when an RSN IE
 * is present; the channel comes from the DS Parameter Set (defaults
 * to 0 when absent). */
static inline int wifi_proto_parse_beacon(const uint8_t *frame, int len,
                                          const uint8_t *bssid_override,
                                          wifi_bss_desc_t *out) {
    if (!wifi_proto_is_mgmt(frame, len, WIFI_MGMT_BEACON, WIFI_MGMT_PROBE_RESP))
        return -1;

    const struct wifi_mgmt_hdr *h = (const struct wifi_mgmt_hdr *)frame;
    int off = (int)sizeof(struct wifi_mgmt_hdr);
    if (len < off + (int)sizeof(struct wifi_beacon_fixed)) return -1;
    const struct wifi_beacon_fixed *fx =
        (const struct wifi_beacon_fixed *)(frame + off);
    off += (int)sizeof(struct wifi_beacon_fixed);

    wifi_memset(out, 0, sizeof(*out));
    if (bssid_override) {
        wifi_memcpy(out->bssid, bssid_override, 6);
    } else {
        wifi_memcpy(out->bssid, h->addr3, 6);    /* BSSID */
    }
    out->capability = fx->capability;

    while (off + 2 <= len) {
        uint8_t id = frame[off];
        uint8_t ie_len = frame[off + 1];
        if (off + 2 + ie_len > len) break;      /* truncated IE: stop */
        const uint8_t *body = frame + off + 2;
        if (id == WIFI_IE_SSID && ie_len <= WIFI_MAX_SSID_LEN) {
            wifi_memcpy(out->ssid, body, ie_len);
            out->ssid[ie_len] = '\0';
            out->ssid_len = ie_len;
        } else if (id == WIFI_IE_DS_PARAM && ie_len >= 1) {
            out->channel = body[0];
        } else if (id == WIFI_IE_RSN && ie_len >= 1) {
            out->wpa2 = 1;
        }
        off += 2 + ie_len;
    }
    return 0;
}

/* Authentication Response: success only when it answers with
 * transaction 2 and status 0. Returns the status code, or -1 when the
 * frame is not an auth response. */
static inline int wifi_proto_parse_auth_resp(const uint8_t *frame, int len) {
    if (!wifi_proto_is_mgmt(frame, len, WIFI_MGMT_AUTH, WIFI_MGMT_AUTH))
        return -1;
    int off = (int)sizeof(struct wifi_mgmt_hdr);
    if (len < off + (int)sizeof(struct wifi_auth_body)) return -1;
    const struct wifi_auth_body *a =
        (const struct wifi_auth_body *)(frame + off);
    if (a->auth_transaction != 2) return -1;
    return (int)a->status_code;
}

/* Association Response (0x01 or Reassociation Response 0x03): 0 on
 * success with the AID (low 14 bits — the top two are flags) in *aid;
 * otherwise the status code; -1 when malformed. */
static inline int wifi_proto_parse_assoc_resp(const uint8_t *frame, int len,
                                              uint16_t *aid) {
    if (!wifi_proto_is_mgmt(frame, len, WIFI_MGMT_ASSOC_RESP,
                            WIFI_MGMT_REASSOC_RESP))
        return -1;
    int off = (int)sizeof(struct wifi_mgmt_hdr);
    if (len < off + (int)sizeof(struct wifi_assoc_resp_body)) return -1;
    const struct wifi_assoc_resp_body *r =
        (const struct wifi_assoc_resp_body *)(frame + off);
    if (r->status_code != WIFI_STATUS_SUCCESS) return (int)r->status_code;
    if (aid) *aid = r->aid & 0x3FFF;
    return 0;
}

/* ---- 802.11 ↔ Ethernet translation --------------------------------------- */

/* Encapsulate an Ethernet frame as a Data frame (ToDS=1, from us to the
 * AP): LLC/SNAP carries the ethertype. Returns the 802.11 frame length
 * or -1. buf must hold len + 18 bytes. */
static inline int wifi_proto_encap_eth(uint8_t *buf, const uint8_t *eth,
                                       uint32_t len,
                                       const uint8_t bssid[6],
                                       const uint8_t our_mac[6]) {
    if (!buf || !eth || len < WIFI_ETH_HDR_LEN) return -1;

    struct wifi_mgmt_hdr *h = (struct wifi_mgmt_hdr *)buf;
    wifi_memset(h, 0, sizeof(*h));
    h->fc.type = 2;              /* Data */
    h->fc.to_ds = 1;
    wifi_memcpy(h->addr1, bssid, 6);  /* RA = the AP */
    wifi_memcpy(h->addr2, our_mac, 6);
    wifi_memcpy(h->addr3, eth, 6);    /* DA = final destination */

    uint8_t *snap = buf + sizeof(*h);
    snap[0] = 0xAA; snap[1] = 0xAA; snap[2] = 0x03;
    snap[3] = 0x00; snap[4] = 0x00; snap[5] = 0x00;
    snap[6] = eth[12];
    snap[7] = eth[13];
    wifi_memcpy(snap + 8, eth + WIFI_ETH_HDR_LEN, len - WIFI_ETH_HDR_LEN);
    return (int)sizeof(*h) + 8 + (int)(len - WIFI_ETH_HDR_LEN);
}

/* The reverse: a Data frame from the DS (FromDS=1) back to Ethernet.
 * eth_buf must hold frame_len bytes. Returns the Ethernet length. */
static inline int wifi_proto_decap_data(const uint8_t *frame, int len,
                                        uint8_t *eth_buf) {
    if (!frame || len < (int)sizeof(struct wifi_mgmt_hdr) + 8) return -1;
    const struct wifi_mgmt_hdr *h = (const struct wifi_mgmt_hdr *)frame;
    if (h->fc.type != 2 || !h->fc.from_ds || h->fc.to_ds) return -1;
    const uint8_t *snap = frame + sizeof(*h);
    if (snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03) return -1;

    /* FromDS 3-address mapping: addr1 = recipient (DA), addr2 = the AP
     * (transmitter), addr3 = the original source (SA). */
    wifi_memcpy(eth_buf, h->addr1, 6);          /* DA */
    wifi_memcpy(eth_buf + 6, h->addr3, 6);      /* SA */
    eth_buf[12] = snap[6];
    eth_buf[13] = snap[7];
    int payload = len - (int)sizeof(*h) - 8;
    if (payload < 0) return -1;
    wifi_memcpy(eth_buf + 14, snap + 8, payload);
    return 14 + payload;
}

#endif /* AURALITE_DRIVERS_WIFI_WIFI_PROTO_H */
