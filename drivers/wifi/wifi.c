/* wifi.c — IEEE 802.11 Wi-Fi MAC layer management.
 *
 * Implements the 802.11 management protocol: active scanning (Probe
 * Request/Response), authentication (Open System), association, and data
 * frame TX/RX. Operates through a registered wireless NIC driver.
 *
 * Protocol flow for connecting to an open network:
 *   1. wifi_scan() — send Probe Requests on each channel, collect beacons
 *   2. wifi_connect(ssid) — find the target in scan results
 *   3. Send Authentication frame (Open System)
 *   4. Wait for Authentication Response
 *   5. Send Association Request
 *   6. Wait for Association Response (contains AID)
 *   7. State = CONNECTED — data frames can now be sent
 *
 * Data path: Ethernet frame → 802.11 data frame → driver → radio
 *
 * The 802.11 frame construction and Information Element parsing are fully
 * implemented. Hardware access is abstracted through the wifi_driver_t
 * callback interface, so any wireless chipset can be supported by writing
 * a thin register-level driver that implements tx_raw/set_channel/get_mac.
 */

#include <stdint.h>
#include "drivers/wifi/wifi.h"
#include "drivers/wifi/wifi_virt.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/kheap.h"

/* ---- Driver state ---- */
static wifi_driver_t driver;
static int driver_registered = 0;
static uint8_t our_mac[6];
static wifi_state_t conn_state = WIFI_STATE_DISCONNECTED;
static uint8_t connected_bssid[6];
static uint16_t connected_aid = 0;
static int auth_ok = 0;         /* RESIDUE2 T6: set by a PARSED auth response */

/* Scan results. */
static wifi_scan_result_t scan_results[WIFI_MAX_SCAN_RESULTS];
static int scan_count = 0;


/* ---- Public API ---- */

int wifi_register_driver(const wifi_driver_t *drv) {
    if (drv == NULL || drv->tx_raw == NULL) {
        return -1;
    }
    driver = *drv;
    driver_registered = 1;
    driver.get_mac(our_mac);
    kprintf("[wifi] driver registered, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            our_mac[0], our_mac[1], our_mac[2],
            our_mac[3], our_mac[4], our_mac[5]);
    return 0;
}

int wifi_init(void) {
    if (!driver_registered) {
        kprintf("[wifi] no wireless driver registered\n");
        return -1;
    }
    conn_state = WIFI_STATE_DISCONNECTED;
    scan_count = 0;
    kprintf("[wifi] subsystem initialised\n");
    return 0;
}

int wifi_scan(void) {
    if (!driver_registered) return -1;

    scan_count = 0;
    auth_ok = 0;
    conn_state = WIFI_STATE_SCANNING;

    kprintf("[wifi] starting active scan...\n");

    /* Scan channels 1-11 (2.4 GHz): the Probe Request IEs are rebuilt
     * per channel so the DS Parameter Set always matches. */
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (int ch = 1; ch <= 11; ch++) {
        driver.set_channel((uint8_t)ch);

        uint8_t probe[128];
        wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)probe,
                            WIFI_MGMT_PROBE_REQ, broadcast, our_mac,
                            broadcast);
        int ie_len = wifi_proto_probe_ies(
            probe + sizeof(struct wifi_mgmt_hdr), (uint8_t)ch);
        driver.tx_raw(probe,
                      (uint32_t)(sizeof(struct wifi_mgmt_hdr) + ie_len));

        /* Beacons/Probe Responses arrive via wifi_rx_frame() — the
         * virtual AP answers synchronously inside tx_raw; a real NIC
         * delivers them from its RX path. */
    }

    conn_state = WIFI_STATE_DISCONNECTED;
    kprintf("[wifi] scan complete: %d networks found\n", scan_count);
    return scan_count;
}

/* ---- RX ingestion (RESIDUE2 T6) ----------------------------------------
 * The driver calls this for every received 802.11 frame.  Becons and
 * Probe Responses feed scan results (parsed by wifi_proto.h); auth and
 * assoc responses advance the connection state machine with values
 * taken from the wire. */
int wifi_rx_frame(const void *frame, uint32_t len) {
    if (!frame || len < sizeof(struct wifi_mgmt_hdr)) return -1;
    const struct wifi_mgmt_hdr *h =
        (const struct wifi_mgmt_hdr *)frame;

    if (h->fc.type != 0) return 0;   /* only management handled here */

    if (h->fc.subtype == WIFI_MGMT_BEACON ||
        h->fc.subtype == WIFI_MGMT_PROBE_RESP) {
        if (conn_state != WIFI_STATE_SCANNING) return 0;
        if (scan_count >= WIFI_MAX_SCAN_RESULTS) return 0;
        wifi_bss_desc_t bss;
        if (wifi_proto_parse_beacon((const uint8_t *)frame, (int)len,
                                    NULL, &bss) != 0)
            return -1;
        /* de-dup by BSSID: refresh, don't grow */
        for (int i = 0; i < scan_count; i++) {
            if (memcmp(scan_results[i].bss.bssid, bss.bssid, 6) == 0) {
                scan_results[i].bss = bss;
                return 0;
            }
        }
        scan_results[scan_count].bss = bss;
        scan_results[scan_count].rssi = -40;   /* driver would report */
        scan_count++;
        return 0;
    }

    if (h->fc.subtype == WIFI_MGMT_AUTH) {
        int st = wifi_proto_parse_auth_resp((const uint8_t *)frame,
                                            (int)len);
        auth_ok = (st == WIFI_STATUS_SUCCESS);
        return (st < 0) ? -1 : 0;
    }

    if (h->fc.subtype == WIFI_MGMT_ASSOC_RESP ||
        h->fc.subtype == WIFI_MGMT_REASSOC_RESP) {
        uint16_t aid = 0;
        int st = wifi_proto_parse_assoc_resp((const uint8_t *)frame,
                                             (int)len, &aid);
        if (st == WIFI_STATUS_SUCCESS) {
            memcpy(connected_bssid, h->addr2, 6);
            connected_aid = aid;      /* from the wire, not a constant */
            conn_state = WIFI_STATE_CONNECTED;
            return 0;
        }
        conn_state = WIFI_STATE_ERROR;
        return (st < 0) ? -1 : 0;
    }
    return 0;
}

const wifi_scan_result_t *wifi_get_scan_result(int index) {
    if (index < 0 || index >= scan_count) return NULL;
    return &scan_results[index];
}

int wifi_connect(const char *ssid) {
    if (!driver_registered) return -1;

    /* Find the SSID in scan results. */
    int target = -1;
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_results[i].bss.ssid, ssid) == 0) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        kprintf("[wifi] SSID '%s' not found in scan results\n", ssid);
        return -1;
    }

    const wifi_scan_result_t *ap = &scan_results[target];
    kprintf("[wifi] connecting to '%s' (BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%d)\n",
            ap->bss.ssid, ap->bss.bssid[0], ap->bss.bssid[1],
            ap->bss.bssid[2], ap->bss.bssid[3], ap->bss.bssid[4],
            ap->bss.bssid[5], ap->bss.channel);

    driver.set_channel(ap->bss.channel);

    /* Step 1: Authentication (Open System). */
    conn_state = WIFI_STATE_AUTHENTICATING;
    auth_ok = 0;
    uint8_t auth_frame[64];
    wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)auth_frame,
                        WIFI_MGMT_AUTH, ap->bss.bssid, our_mac,
                        ap->bss.bssid);
    struct wifi_auth_body *abody =
        (struct wifi_auth_body *)(auth_frame + sizeof(struct wifi_mgmt_hdr));
    abody->auth_alg = WIFI_AUTH_OPEN;
    abody->auth_transaction = 1;
    abody->status_code = 0;

    int ret = driver.tx_raw(auth_frame,
        (uint32_t)(sizeof(struct wifi_mgmt_hdr) + sizeof(struct wifi_auth_body)));
    if (ret < 0) {
        kprintf("[wifi] failed to send Authentication frame\n");
        conn_state = WIFI_STATE_ERROR;
        return -1;
    }
    if (!auth_ok) {
        kprintf("[wifi] no successful Authentication Response\n");
        conn_state = WIFI_STATE_ERROR;
        return -1;
    }

    /* Step 2: Association Request. */
    conn_state = WIFI_STATE_ASSOCIATING;
    uint8_t assoc_frame[128];
    wifi_proto_mgmt_hdr((struct wifi_mgmt_hdr *)assoc_frame,
                        WIFI_MGMT_ASSOC_REQ, ap->bss.bssid, our_mac,
                        ap->bss.bssid);
    struct wifi_assoc_req_body *arbody =
        (struct wifi_assoc_req_body *)(assoc_frame + sizeof(struct wifi_mgmt_hdr));
    arbody->capability = 0x0431;  /* ESS + Privacy + Short Preamble */
    arbody->listen_interval = 100;
    int ies_len = wifi_proto_assoc_ies(
        assoc_frame + sizeof(struct wifi_mgmt_hdr) + sizeof(*arbody),
        ap->bss.ssid, ap->bss.ssid_len);
    int assoc_frame_len = (int)sizeof(struct wifi_mgmt_hdr) +
                          (int)sizeof(*arbody) + ies_len;

    ret = driver.tx_raw(assoc_frame, (uint32_t)assoc_frame_len);
    if (ret < 0) {
        kprintf("[wifi] failed to send Association Request\n");
        conn_state = WIFI_STATE_ERROR;
        return -1;
    }

    /* The Association Response arrives via wifi_rx_frame(): it set
     * conn_state=CONNECTED with the AID from the wire.  A driver with
     * asynchronous RX would complete this later — here we insist. */
    if (conn_state != WIFI_STATE_CONNECTED) {
        kprintf("[wifi] no Association Response\n");
        conn_state = WIFI_STATE_ERROR;
        return -1;
    }

    kprintf("[wifi] CONNECTED to '%s' (AID=%d)\n", ap->bss.ssid,
            connected_aid);
    return 0;
}

wifi_state_t wifi_get_state(void) {
    return conn_state;
}

int wifi_get_bssid(uint8_t bssid[6]) {
    if (conn_state != WIFI_STATE_CONNECTED) return -1;
    memcpy(bssid, connected_bssid, 6);
    return 0;
}

int wifi_send_data(const void *eth_frame, uint32_t len) {
    if (conn_state != WIFI_STATE_CONNECTED || !driver_registered) return -1;

    /* Ethernet → 802.11 Data frame with LLC/SNAP (wifi_proto.h). */
    uint8_t wifi_frame[1518];
    int wifi_len = wifi_proto_encap_eth(wifi_frame,
                                        (const uint8_t *)eth_frame, len,
                                        connected_bssid, our_mac);
    if (wifi_len < 0) return -1;
    return driver.tx_raw(wifi_frame, (uint32_t)wifi_len);
}

void wifi_self_test(void) {
    kprintf("[wifi] self-test:\n");
    kprintf("[wifi]   IEEE 802.11 management: Beacon, Probe, Auth, Assoc\n");
    kprintf("[wifi]   Protocol layer: wifi_proto.h (host-pinned by "
            "test_wifi_proto)\n");
    kprintf("[wifi]   RX ingestion: wifi_rx_frame (scan/auth/aid from "
            "parsed frames)\n");

    if (!driver_registered) {
        /* No real silicon in the tree: exercise the whole MAC flow over
         * the deterministic reference backend (a virtual AP — RESIDUE2
         * T6; real chipsets stay a loud D2 skip). */
        wifi_virt_register();
        wifi_init();

        int n = wifi_scan();
        const wifi_scan_result_t *ap = wifi_get_scan_result(0);
        if (n >= 1 && ap && strcmp(ap->bss.ssid, "AuraVirt") == 0 &&
                ap->bss.channel == 6) {
            kprintf("[wifi] PASS: virtual AP: scan found '%s' ch%u "
                    "(parsed from the probe response)\n",
                    ap->bss.ssid, ap->bss.channel);
        } else {
            kprintf("[wifi] FAIL: virtual AP scan (n=%d)\n", n);
            return;
        }

        if (wifi_connect("AuraVirt") == 0 && connected_aid ==
                (uint16_t)(0xC005 & 0x3FFF)) {
            kprintf("[wifi] PASS: virtual AP: open-auth + assoc, AID=%u "
                    "from the wire\n", connected_aid);
        } else {
            kprintf("[wifi] FAIL: virtual AP connect\n");
            return;
        }

        /* One Ethernet frame through the LLC/SNAP data path. */
        uint8_t eth[60] = {0};
        eth[12] = 0x08; eth[13] = 0x00;       /* ethertype IPv4 */
        for (int i = 14; i < 60; i++) eth[i] = (uint8_t)i;
        if (wifi_send_data(eth, sizeof(eth)) > 0 &&
                wifi_virt_data_frames() == 1 &&
                wifi_virt_data_bytes() == 24 + 8 + 46) {
            kprintf("[wifi] PASS: virtual AP: %u data bytes through the "
                    "LLC/SNAP path\n", wifi_virt_data_bytes());
        } else {
            kprintf("[wifi] FAIL: virtual AP data path\n");
            return;
        }
        return;
    }

    if (driver_registered) {
        kprintf("[wifi] PASS: wireless driver registered\n");
    } else {
        kprintf("[wifi] PASS: protocol layer ready (no wireless NIC)\n");
    }
}
