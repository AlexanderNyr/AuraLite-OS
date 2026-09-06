/* test_wifi_proto.c — host gate for the IEEE 802.11 wire protocol
 * (RESIDUE2 T6, ledger RES-39; drivers/wifi/wifi_proto.h).
 *
 * D2, tcp_cc/bt_hci style: QEMU has no 802.11 radio, so the byte
 * layouts and parser verdicts are pinned HERE deterministically; the
 * kernel's wifi_virt.c reference backend then drives the same
 * builders/parsers end to end (receipt lines in the boot self-test).
 *
 * What is pinned:
 *   - Probe Request IEs (wildcard SSID, 8-rate set, DS channel) and
 *     their per-channel rebuild;
 *   - Association Request IEs (SSID + rates);
 *   - Beacon/Probe Response parsing: BSSID source, SSID copy with
 *     termination, DS channel, capability bits, RSN → wpa2, truncated
 *     IE handling;
 *   - Authentication response verdicts (transaction 2 + status);
 *   - Association response verdicts and the AID mask (top two bits are
 *     flags — the virtual AP transmits 0xC005, the MAC layer must see
 *     0x4005);
 *   - Ethernet ↔ 802.11 Data translation both ways, byte-exact, and
 *     the malformed-input refusals (short frames, non-mgmt, wrong
 *     direction bit, missing LLC/SNAP).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/wifi/wifi_proto.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

/* Build a mgmt frame header for parser inputs. */
static void mkhdr(uint8_t *buf, uint8_t subtype) {
    struct wifi_mgmt_hdr *h = (struct wifi_mgmt_hdr *)buf;
    memset(h, 0, sizeof(*h));
    h->fc.type = 0;
    h->fc.subtype = subtype;
    memset(h->addr3, 0xAB, 6);      /* BSSID */
}

int main(void) {
    uint8_t buf[256];

    /* ---- Probe Request IEs ----------------------------------------- */
    {
        int n = wifi_proto_probe_ies(buf, 6);
        CHECK(n == 15, "probe IE block length %d", n);
        CHECK(buf[0] == WIFI_IE_SSID && buf[1] == 0, "wildcard SSID IE");
        CHECK(buf[2] == WIFI_IE_RATES && buf[3] == 8, "rates IE header");
        CHECK(buf[4] == 0x82 && buf[11] == 0x60, "rates content");
        CHECK(buf[12] == WIFI_IE_DS_PARAM && buf[13] == 1 &&
              buf[14] == 6, "DS channel 6");
        int n1 = wifi_proto_probe_ies(buf, 11);
        CHECK(buf[14] == 11 && n1 == 15, "per-channel rebuild");
    }

    /* ---- Association Request IEs ------------------------------------ */
    {
        int n = wifi_proto_assoc_ies(buf, "AuraVirt", 8);
        CHECK(n == 2 + 8 + 10, "assoc IE length %d", n);
        CHECK(buf[0] == WIFI_IE_SSID && buf[1] == 8 &&
              memcmp(buf + 2, "AuraVirt", 8) == 0, "assoc SSID IE");
        CHECK(buf[10] == WIFI_IE_RATES && buf[11] == 8, "assoc rates IE");
    }

    /* ---- Beacon / Probe Response parsing ----------------------------- */
    {
        mkhdr(buf, WIFI_MGMT_PROBE_RESP);
        struct wifi_beacon_fixed *fx =
            (struct wifi_beacon_fixed *)(buf + sizeof(struct wifi_mgmt_hdr));
        memset(fx, 0, sizeof(*fx));
        fx->beacon_interval = 100;
        fx->capability = WIFI_CAP_ESS;
        uint8_t *ie = buf + sizeof(struct wifi_mgmt_hdr) + sizeof(*fx);
        int pos = 0;
        ie[pos++] = WIFI_IE_SSID; ie[pos++] = 8;
        memcpy(ie + pos, "AuraVirt", 8); pos += 8;
        ie[pos++] = WIFI_IE_DS_PARAM; ie[pos++] = 1; ie[pos++] = 6;
        ie[pos++] = WIFI_IE_RSN; ie[pos++] = 2; ie[pos++] = 1; ie[pos++] = 0;
        int len = (int)sizeof(struct wifi_mgmt_hdr) + (int)sizeof(*fx) + pos;

        wifi_bss_desc_t bss;
        CHECK(wifi_proto_parse_beacon(buf, len, NULL, &bss) == 0,
              "probe response parsed");
        CHECK(memcmp(bss.bssid,
                     ((struct wifi_mgmt_hdr *)buf)->addr3, 6) == 0,
              "BSSID from addr3");
        CHECK(bss.ssid_len == 8 && strcmp(bss.ssid, "AuraVirt") == 0,
              "SSID extracted and terminated");
        CHECK(bss.channel == 6, "channel from DS param");
        CHECK(bss.capability == WIFI_CAP_ESS, "capability kept");
        CHECK(bss.wpa2 == 1, "RSN IE sets wpa2");

        /* truncation: cut inside the last IE — parse stops, keeps the
         * fields it already saw (SSID, channel) and does not overrun */
        CHECK(wifi_proto_parse_beacon(buf, len - 2, NULL, &bss) == 0,
              "truncated IE tolerated");
        CHECK(bss.ssid_len == 8 && bss.channel == 6,
              "fields before the cut intact");

        CHECK(wifi_proto_parse_beacon(buf, 10, NULL, &bss) == -1,
              "short frame refused");
        mkhdr(buf + 0, WIFI_MGMT_AUTH);
        CHECK(wifi_proto_parse_beacon(buf, len, NULL, &bss) == -1,
              "non-beacon subtype refused");
    }

    /* ---- Authentication responses ------------------------------------- */
    {
        mkhdr(buf, WIFI_MGMT_AUTH);
        struct wifi_auth_body *a =
            (struct wifi_auth_body *)(buf + sizeof(struct wifi_mgmt_hdr));
        int len = (int)sizeof(struct wifi_mgmt_hdr) + (int)sizeof(*a);
        a->auth_alg = WIFI_AUTH_OPEN;
        a->auth_transaction = 2;
        a->status_code = 0;
        CHECK(wifi_proto_parse_auth_resp(buf, len) == 0, "auth success");
        a->status_code = 1;
        CHECK(wifi_proto_parse_auth_resp(buf, len) == 1, "auth failure code");
        a->auth_transaction = 1; a->status_code = 0;
        CHECK(wifi_proto_parse_auth_resp(buf, len) == -1,
              "request (seq 1) is not a response");
        CHECK(wifi_proto_parse_auth_resp(buf, len - 2) == -1,
              "short auth refused");
    }

    /* ---- Association responses: AID masking ---------------------------- */
    {
        mkhdr(buf, WIFI_MGMT_ASSOC_RESP);
        struct wifi_assoc_resp_body *r =
            (struct wifi_assoc_resp_body *)(buf + sizeof(struct wifi_mgmt_hdr));
        int len = (int)sizeof(struct wifi_mgmt_hdr) + (int)sizeof(*r);
        r->capability = WIFI_CAP_ESS;
        r->status_code = 0;
        r->aid = 0xC005;                    /* flags in the top bits */
        uint16_t aid = 0;
        CHECK(wifi_proto_parse_assoc_resp(buf, len, &aid) == 0,
              "assoc success");
        CHECK(aid == 0x0005, "AID masked to 14 bits (0x0005), got 0x%x", aid);
        r->status_code = 1;
        CHECK(wifi_proto_parse_assoc_resp(buf, len, &aid) == 1,
              "assoc failure code");
        r->status_code = 0;
        CHECK(wifi_proto_parse_assoc_resp(buf, len - 3, &aid) == -1,
              "short assoc refused");
    }

    /* ---- Ethernet ↔ 802.11 translation ---------------------------------- */
    {
        uint8_t eth[60];
        for (int i = 0; i < 6; i++) { eth[i] = 0x10 + i; eth[6 + i] = 0x20 + i; }
        eth[12] = 0x08; eth[13] = 0x00;     /* IPv4 */
        for (int i = 14; i < 60; i++) eth[i] = (uint8_t)i;

        uint8_t bssid[6], mac[6];
        memset(bssid, 0xAB, 6);
        memset(mac, 0xCD, 6);
        int wlen = wifi_proto_encap_eth(buf, eth, sizeof(eth), bssid, mac);
        CHECK(wlen == 24 + 8 + 46, "encap length %d", wlen);
        struct wifi_mgmt_hdr *h = (struct wifi_mgmt_hdr *)buf;
        CHECK(h->fc.type == 2 && h->fc.to_ds == 1 && h->fc.from_ds == 0,
              "data frame ToDS");
        CHECK(memcmp(h->addr1, bssid, 6) == 0 && memcmp(h->addr2, mac, 6) == 0
              && memcmp(h->addr3, eth, 6) == 0, "address mapping");
        CHECK(buf[24] == 0xAA && buf[25] == 0xAA && buf[26] == 0x03,
              "LLC/SNAP signature");
        CHECK(buf[30] == 0x08 && buf[31] == 0x00, "ethertype carried");

        /* The AP forwards: a FromDS frame carries the SAME payload with
         * addr1 = recipient (DA), addr2 = the AP, addr3 = original SA. */
        uint8_t fwd[256];
        struct wifi_mgmt_hdr *fh = (struct wifi_mgmt_hdr *)fwd;
        memset(fh, 0, sizeof(*fh));
        fh->fc.type = 2;
        fh->fc.from_ds = 1;
        memcpy(fh->addr1, eth, 6);            /* DA */
        memcpy(fh->addr2, bssid, 6);          /* the AP */
        memcpy(fh->addr3, eth + 6, 6);        /* original SA */
        memcpy(fwd + sizeof(*fh), buf + sizeof(*h), (size_t)(wlen - 24));

        uint8_t eth2[60];
        int elen = wifi_proto_decap_data(fwd, wlen, eth2);
        CHECK(elen == 60, "decap length %d", elen);
        CHECK(memcmp(eth, eth2, 60) == 0, "round-trip byte-exact");

        elen = wifi_proto_decap_data(buf, wlen, eth2);
        CHECK(elen == -1 && ((struct wifi_mgmt_hdr *)buf)->fc.to_ds == 1,
              "ToDS-only frame is not from the DS");

        fwd[24] = 0x00;                        /* not LLC/SNAP */
        CHECK(wifi_proto_decap_data(fwd, wlen, eth2) == -1,
              "missing LLC/SNAP refused");
        CHECK(wifi_proto_encap_eth(buf, eth, 13, bssid, mac) == -1,
              "short Ethernet frame refused");
    }

    printf("test_wifi_proto: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
