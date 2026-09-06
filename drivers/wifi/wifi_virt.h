#ifndef AURALITE_DRIVERS_WIFI_WIFI_VIRT_H
#define AURALITE_DRIVERS_WIFI_WIFI_VIRT_H

/*
 * wifi_virt — the RESIDUE2 T6 reference backend for the Wi-Fi MAC
 * layer: a deterministic virtual AP (no radio claimed).
 */

/* Register the virtual AP as the wifi_driver_t backend and reset its
 * counters.  Idempotent. */
void wifi_virt_register(void);

/* Receipt counters, for the boot self-test's PASS lines. */
uint32_t wifi_virt_data_bytes(void);
int wifi_virt_data_frames(void);

#endif
