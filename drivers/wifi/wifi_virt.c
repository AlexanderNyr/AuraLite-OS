/*
 * wifi_virt.c — the RESIDUE2 T6 reference backend for the Wi-Fi MAC
 * layer (ledger RES-39): a deterministic VIRTUAL AP, not a radio.
 *
 * QEMU has no 802.11 device model, so a "real chipset backend" can
 * never be QEMU-gated — that claim would be exactly the "compiles
 * passes for works" the plan forbids.  What silicon would actually
 * provide is a radio that (a) transmits frames the MAC layer hands it
 * and (b) delivers received frames back.  This backend models exactly
 * that contract over the wifi_driver_t seam: every Probe Request,
 * Authentication and Association Request the MAC layer transmits is
 * ANSWERED on the spot — a standards-shaped frame built with the same
 * wifi_proto.h builders/parsers the host test pins — and delivered
 * through wifi_rx_frame(), so scan results, the auth verdict and the
 * AID all come from parsed wire bytes end to end.
 *
 * The AP lives on channel 6, SSID "AuraVirt", open authentication,
 * and grants AID 0x4005 (transmitted as 0xC005 — the AID field's top
 * two bits are flags, which the parser must mask).  Real silicon
 * remains a loud D2 skip until hardware exists to gate it.
 */

#include <stdint.h>
#include "drivers/wifi/wifi.h"
#include "drivers/wifi/wifi_virt.h"
#include "kernel/lib/string.h"

static uint8_t virt_channel = 1;
static uint32_t virt_data_bytes = 0;
static int virt_data_frames = 0;

static const uint8_t AP_BSSID[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t VIRT_MAC[6]  = {0x02, 0x50, 0x41, 0x55, 0x01, 0x01};
#define VIRT_SSID     "AuraVirt"
#define VIRT_SSID_LEN 8
#define VIRT_CHANNEL  6
/* Transmitted AID: bit 14/15 are flags; the parser keeps 0x3FFF. */
#define VIRT_AID_TX   0xC005
#define VIRT_AID      (VIRT_AID_TX & 0x3FFF)


/* ---- C6: the Ethernet-level AP ----------------------------------------- */
#define VIRT_ETH_MAX   1600
#define VIRT_RXQ_DEPTH 8
#define VIRT_ETH_ARP   0x0806
#define VIRT_ETH_IPV4  0x0800

static uint32_t virt_ap_ip;               /* host order, 0 = not configured */
static uint8_t  virt_rxq[VIRT_RXQ_DEPTH][VIRT_ETH_MAX];
static uint16_t virt_rxq_len[VIRT_RXQ_DEPTH];
static int virt_rxq_head, virt_rxq_tail, virt_rxq_count;
static int virt_arp_rx, virt_icmp_rx, virt_foreign;
static uint32_t virt_eth_bytes;           /* replied Ethernet bytes */
static uint32_t virt_eth_frames;

void wifi_virt_set_ap_ip(uint32_t ap_ip) { virt_ap_ip = ap_ip; }
uint32_t wifi_virt_ap_ip(void) { return virt_ap_ip; }
int wifi_virt_arp_replies(void) { return virt_arp_rx; }
int wifi_virt_icmp_replies(void) { return virt_icmp_rx; }
int wifi_virt_foreign_frames(void) { return virt_foreign; }

void wifi_virt_eth_reset(void) {
    virt_rxq_head = virt_rxq_tail = virt_rxq_count = 0;
    virt_arp_rx = virt_icmp_rx = virt_foreign = 0;
    virt_eth_bytes = virt_eth_frames = 0;
}

int wifi_virt_eth_pop(void *buf, uint32_t bufsize) {
    if (!buf || virt_rxq_count == 0) return 0;
    uint16_t n = virt_rxq_len[virt_rxq_tail];
    if (n > bufsize) n = (uint16_t)bufsize;
    memcpy(buf, virt_rxq[virt_rxq_tail], n);
    virt_rxq_tail = (virt_rxq_tail + 1) % VIRT_RXQ_DEPTH;
    virt_rxq_count--;
    return (int)n;
}

static void virt_eth_queue(const uint8_t *frame, uint16_t len) {
    if (len > VIRT_ETH_MAX) return;
    if (virt_rxq_count == VIRT_RXQ_DEPTH) {
        /* Full: drop the OLDEST.  A ping's reply is the newest frame and the
         * one the stack is waiting for, so the queue must not prefer stale
         * traffic -- dropping the newest would strand every waiter. */
        virt_rxq_tail = (virt_rxq_tail + 1) % VIRT_RXQ_DEPTH;
        virt_rxq_count--;
        virt_foreign++;                 /* counted as a dropped frame */
    }
    memcpy(virt_rxq[virt_rxq_head], frame, len);
    virt_rxq_len[virt_rxq_head] = len;
    virt_rxq_head = (virt_rxq_head + 1) % VIRT_RXQ_DEPTH;
    virt_rxq_count++;
    virt_eth_frames++;
    virt_eth_bytes += len;
}

/* Internet checksum (RFC 1071): the one the IP and ICMP headers carry. */
static uint16_t virt_csum(const uint8_t *p, int len, uint32_t sum) {
    for (int i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build and queue an ARP reply to a request we just received. */
static void virt_arp_reply(const uint8_t *eth) {
    uint8_t out[42];
    const uint8_t *req = eth + 14;             /* ARP payload */
    memcpy(out, req + 8, 6);                   /* DA = requester's SHA */
    memcpy(out + 6, AP_BSSID, 6);              /* SA = the AP */
    out[12] = VIRT_ETH_ARP >> 8;
    out[13] = VIRT_ETH_ARP & 0xFF;
    uint8_t *r = out + 14;
    r[0] = 0x00; r[1] = 0x01;                  /* htype ethernet */
    r[2] = 0x08; r[3] = 0x00;                  /* ptype IPv4 */
    r[4] = 6; r[5] = 4;                        /* hlen/plen */
    r[6] = 0x00; r[7] = 0x02;                  /* oper = reply */
    memcpy(r + 8, AP_BSSID, 6);                /* SHA = the AP */
    r[14] = (uint8_t)(virt_ap_ip >> 24);       /* SPA = AP address */
    r[15] = (uint8_t)(virt_ap_ip >> 16);
    r[16] = (uint8_t)(virt_ap_ip >> 8);
    r[17] = (uint8_t)(virt_ap_ip);
    memcpy(r + 18, req + 8, 6);                /* THA = requester */
    memcpy(r + 24, req + 14, 4);               /* TPA = requester's IP */
    virt_arp_rx++;
    virt_eth_queue(out, (uint16_t)sizeof(out));
}

/* Build and queue an ICMP echo reply for an echo request addressed to us. */
static void virt_icmp_reply(const uint8_t *eth, int len) {
    if (len < 14 + 20 + 8) return;
    uint8_t out[VIRT_ETH_MAX];
    const uint8_t *ip = eth + 14;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 8) return;
    int total = (ip[2] << 8) | ip[3];
    if (total < ihl + 8 || 14 + total > len) total = len - 14;
    int icmp_len = total - ihl;
    if (icmp_len > VIRT_ETH_MAX - 14 - ihl) icmp_len = VIRT_ETH_MAX - 14 - ihl;

    memcpy(out, eth + 6, 6);                   /* DA = original source */
    memcpy(out + 6, AP_BSSID, 6);
    out[12] = VIRT_ETH_IPV4 >> 8;
    out[13] = VIRT_ETH_IPV4 & 0xFF;

    uint8_t *oip = out + 14;
    memcpy(oip, ip, (size_t)ihl);              /* header: addresses swapped */
    memcpy(oip + 12, ip + 16, 4);              /* src = our IP */
    memcpy(oip + 16, ip + 12, 4);              /* dst = the requester */
    oip[8] = 64;                               /* ttl */
    oip[10] = 0; oip[11] = 0;                  /* checksum recomputed */
    oip[2] = (uint8_t)((ihl + icmp_len) >> 8);
    oip[3] = (uint8_t)((ihl + icmp_len) & 0xFF);
    uint16_t ipc = virt_csum(oip, ihl, 0);
    oip[10] = (uint8_t)(ipc >> 8); oip[11] = (uint8_t)(ipc & 0xFF);

    uint8_t *oicmp = oip + ihl;
    memcpy(oicmp, ip + ihl, (size_t)icmp_len); /* payload copied verbatim */
    oicmp[0] = 0;                              /* type = echo reply */
    oicmp[1] = 0;
    oicmp[2] = 0; oicmp[3] = 0;
    uint16_t icc = virt_csum(oicmp, icmp_len, 0);
    oicmp[2] = (uint8_t)(icc >> 8); oicmp[3] = (uint8_t)(icc & 0xFF);

    virt_icmp_rx++;
    virt_eth_queue(out, (uint16_t)(14 + ihl + icmp_len));
}

/* Handle one Ethernet frame the MAC layer carried up from a ToDS data
 * frame: answer what is addressed to the AP, drop the rest. */
static void virt_eth_input(const uint8_t *eth, int len) {
    if (len < 14) return;
    uint16_t type = (uint16_t)((eth[12] << 8) | eth[13]);
    if (type == VIRT_ETH_ARP && len >= 14 + 28) {
        const uint8_t *r = eth + 14;
        if (((r[6] << 8) | r[7]) != 1) return;          /* requests only */
        uint32_t tpa = ((uint32_t)r[24] << 24) | ((uint32_t)r[25] << 16) |
                       ((uint32_t)r[26] << 8) | r[27];
        if (virt_ap_ip && tpa == virt_ap_ip) virt_arp_reply(eth);
        else virt_foreign++;
        return;
    }
    if (type == VIRT_ETH_IPV4 && len >= 14 + 20) {
        const uint8_t *ip = eth + 14;
        int ihl = (ip[0] & 0x0F) * 4;
        if (ihl < 20 || len < 14 + ihl) return;
        uint32_t dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
                       ((uint32_t)ip[18] << 8) | ip[19];
        if (ip[9] == 1 && dst == virt_ap_ip && len >= 14 + ihl + 8 &&
            ip[ihl] == 8) {                              /* ICMP echo req */
            virt_icmp_reply(eth, len);
            return;
        }
        virt_foreign++;
        return;
    }
    virt_foreign++;
}

/* Pull the Ethernet part out of a ToDS data frame (RA = AP, SA = us,
 * DA = the final destination) and hand it to the AP. */
static void virt_data_in(const struct wifi_mgmt_hdr *h, const uint8_t *frame,
                         uint32_t len) {
    const uint8_t *snap = frame + sizeof(*h);
    if (len < sizeof(*h) + 8) return;
    if (snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03) return;

    uint8_t eth[VIRT_ETH_MAX];
    memcpy(eth, h->addr3, 6);                       /* DA */
    memcpy(eth + 6, h->addr2, 6);                   /* SA */
    eth[12] = snap[6];
    eth[13] = snap[7];
    uint32_t payload = len - (uint32_t)sizeof(*h) - 8;
    if (payload > VIRT_ETH_MAX - 14) payload = VIRT_ETH_MAX - 14;
    memcpy(eth + 14, snap + 8, payload);
    virt_eth_input(eth, (int)(14 + payload));
}

uint32_t wifi_virt_data_bytes(void) { return virt_data_bytes; }
int wifi_virt_data_frames(void) { return virt_data_frames; }

static int virt_tx_raw(const void *frame, uint32_t len) {
    if (!frame || len < sizeof(struct wifi_mgmt_hdr)) return -1;
    const struct wifi_mgmt_hdr *h =
        (const struct wifi_mgmt_hdr *)frame;
    uint8_t resp[256];

    if (h->fc.type == 2) {                    /* Data frame */
        virt_data_frames++;
        virt_data_bytes += len;
        /* C6: a data frame carries an Ethernet frame to the AP; answer what
         * is addressed to it (ARP for its address, ICMP echo to its
         * address) and queue the reply for the netdev seam. */
        virt_data_in(h, (const uint8_t *)frame, len);
        return (int)len;
    }
    if (h->fc.type != 0) return 0;            /* only mgmt answered */

    if (h->fc.subtype == WIFI_MGMT_PROBE_REQ) {
        /* Answer only from our channel — an AP lives on ONE channel. */
        if (virt_channel != VIRT_CHANNEL) return (int)len;

        wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)resp,
                            WIFI_MGMT_PROBE_RESP, h->addr2, AP_BSSID,
                            AP_BSSID);
        struct wifi_beacon_fixed *fx =
            (struct wifi_beacon_fixed *)(resp + sizeof(*h));
        memset(fx, 0, sizeof(*fx));
        fx->beacon_interval = 100;
        fx->capability = WIFI_CAP_ESS;        /* open network */

        uint8_t *ie = resp + sizeof(*h) + sizeof(*fx);
        int pos = 0;
        ie[pos++] = WIFI_IE_SSID;
        ie[pos++] = VIRT_SSID_LEN;
        memcpy(ie + pos, VIRT_SSID, VIRT_SSID_LEN);
        pos += VIRT_SSID_LEN;
        ie[pos++] = WIFI_IE_RATES;
        ie[pos++] = 8;
        memcpy(ie + pos, wifi_proto_rates, 8);
        pos += 8;
        ie[pos++] = WIFI_IE_DS_PARAM;
        ie[pos++] = 1;
        ie[pos++] = VIRT_CHANNEL;

        return wifi_rx_frame(resp,
                (uint32_t)(sizeof(*h) + sizeof(*fx) + pos));
    }

    if (h->fc.subtype == WIFI_MGMT_AUTH) {
        wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)resp,
                            WIFI_MGMT_AUTH, h->addr2, AP_BSSID,
                            AP_BSSID);
        struct wifi_auth_body *a =
            (struct wifi_auth_body *)(resp + sizeof(*h));
        a->auth_alg = WIFI_AUTH_OPEN;
        a->auth_transaction = 2;              /* the response */
        a->status_code = WIFI_STATUS_SUCCESS;
        return wifi_rx_frame(resp,
                (uint32_t)(sizeof(*h) + sizeof(*a)));
    }

    if (h->fc.subtype == WIFI_MGMT_ASSOC_REQ) {
        wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)resp,
                            WIFI_MGMT_ASSOC_RESP, h->addr2, AP_BSSID,
                            AP_BSSID);
        struct wifi_assoc_resp_body *r =
            (struct wifi_assoc_resp_body *)(resp + sizeof(*h));
        r->capability = WIFI_CAP_ESS;
        r->status_code = WIFI_STATUS_SUCCESS;
        r->aid = VIRT_AID_TX;                 /* flags in the top bits */
        return wifi_rx_frame(resp,
                (uint32_t)(sizeof(*h) + sizeof(*r)));
    }

    return (int)len;
}

static int virt_set_channel(uint8_t channel) {
    virt_channel = channel;
    return 0;
}

static void virt_get_mac(uint8_t mac[6]) {
    memcpy(mac, VIRT_MAC, 6);
}

static const wifi_driver_t virt_driver = {
    .tx_raw      = virt_tx_raw,
    .set_channel = virt_set_channel,
    .get_mac     = virt_get_mac,
};

int wifi_virt_eth_frames(void) { return (int)virt_eth_frames; }
uint32_t wifi_virt_eth_bytes(void) { return virt_eth_bytes; }

void wifi_virt_register(void) {
    virt_data_bytes = 0;
    virt_data_frames = 0;
    wifi_virt_eth_reset();
    wifi_register_driver(&virt_driver);
}
