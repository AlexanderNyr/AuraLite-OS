#ifndef AURALITE_DRIVERS_WIFI_WIFI_H
#define AURALITE_DRIVERS_WIFI_WIFI_H

#include <stdint.h>

#include "drivers/wifi/wifi_proto.h"

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

/* Connection states. */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_SCANNING,
    WIFI_STATE_AUTHENTICATING,
    WIFI_STATE_ASSOCIATING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR,
} wifi_state_t;

/* ---- Scan result ---- */
typedef struct {
    wifi_bss_desc_t bss;    /* parsed wire fields (wifi_proto.h) */
    int8_t   rssi;          /* signal strength (dBm, negative) */
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

/* Deliver a received 802.11 frame (beacons, probe responses, auth/assoc
 * responses) into the MAC layer.  Called by the NIC driver's RX path. */
int wifi_rx_frame(const void *frame, uint32_t len);

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
