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

void wifi_virt_register(void) {
    virt_data_bytes = 0;
    virt_data_frames = 0;
    wifi_register_driver(&virt_driver);
}
