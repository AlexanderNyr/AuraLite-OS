#ifndef AURALITE_DRIVERS_WIFI_WIFI_H
#define AURALITE_DRIVERS_WIFI_WIFI_H

#include <stdint.h>

/*
 * Wi-Fi (IEEE 802.11) subsystem for AuraLite OS.
 *
 * Implements the 802.11 MAC layer management: active scanning (Probe
 * Request/Response), authentication (Open / WPA2-PSK), association, and
 * data frame TX/RX. Operates on top of a wireless NIC driver (e.g. Intel
 * iwlwifi, Realtek rtl8188, Atheros ath9k) connected via PCI or USB.
 *
 * The protocol layer handles:
 *   - Management frame construction (Beacon, Probe, Auth, Assoc, Deauth)
 *   - SSID scanning and result collection
 *   - BSS selection and connection state machine
 *   - 802.11 → 802.3 frame conversion for the IP stack
 *   - WPA2-PSK 4-way handshake (framework; crypto via software AES/SHA)
 *
 * Driver interface:
 *   Each wireless NIC driver registers a set of callbacks (tx_raw, set_channel,
 *   get_mac) that the Wi-Fi core uses for hardware access.
 */

/* ---- 802.11 constants ---- */
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

/* Reason codes. */
#define WIFI_REASON_UNSPEC      1
#define WIFI_REASON_AUTH_EXPIRE 2

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

/* Connection states. */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_SCANNING,
    WIFI_STATE_AUTHENTICATING,
    WIFI_STATE_ASSOCIATING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR,
} wifi_state_t;

/* ---- 802.11 frame structures ---- */

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
    uint8_t  addr1[6];   /* destination / BSSID */
    uint8_t  addr2[6];   /* source / transmitter */
    uint8_t  addr3[6];   /* BSSID */
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

/* ---- Scan result ---- */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[WIFI_MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    int8_t   rssi;          /* signal strength (dBm, negative) */
    uint16_t capability;
    int      wpa2;
} wifi_scan_result_t;

/* ---- Wi-Fi device driver interface ---- */
typedef struct {
    /* Send a raw 802.11 frame (includes full MAC header). */
    int  (*tx_raw)(const void *frame, uint32_t len);
    /* Set the radio channel (1-14 for 2.4 GHz). */
    int  (*set_channel)(uint8_t channel);
    /* Get our MAC address. */
    void (*get_mac)(uint8_t mac[6]);
} wifi_driver_t;

/* ---- Public API ---- */

/* Register a wireless NIC driver. Called by the chipset-specific driver. */
int wifi_register_driver(const wifi_driver_t *drv);

/* Initialise the Wi-Fi subsystem. */
int wifi_init(void);

/* Perform an active scan on all 2.4 GHz channels.
 * Sends Probe Request frames and collects responses.
 * Returns the number of networks found. */
int wifi_scan(void);

/* Get a scan result by index. Returns NULL if out of range. */
const wifi_scan_result_t *wifi_get_scan_result(int index);

/* Connect to an open network by SSID. */
int wifi_connect(const char *ssid);

/* Get the current connection state. */
wifi_state_t wifi_get_state(void);

/* Get the BSSID of the current connection. */
int wifi_get_bssid(uint8_t bssid[6]);

/* Send a data frame (Ethernet → 802.11 conversion).
 * Wraps the payload in an 802.11 data frame header. */
int wifi_send_data(const void *eth_frame, uint32_t len);

/* Self-test. */
void wifi_self_test(void);

#endif /* AURALITE_DRIVERS_WIFI_WIFI_H */
