#ifndef AURALITE_DRIVERS_USB_XHCI_H
#define AURALITE_DRIVERS_USB_XHCI_H

#include <stdint.h>

#define XHCI_MAX_PORTS  32
#define XHCI_MAX_SLOTS  32

int xhci_init(void);
int xhci_get_port_count(void);
int xhci_port_has_device(int port);
int xhci_port_speed(int port);
int xhci_reset_port(int port);
int xhci_warm_reset_port(int port);
int xhci_suspend_port(int port);
int xhci_resume_port(int port);
int xhci_suspend(void);
int xhci_resume(void);

int xhci_address_device(uint8_t usb_addr, int port, int speed, uint8_t max_packet0);
/* USB_PLAN U1: No Op Command round-trip -- proves the command ring and
 * event ring are correctly wired.  Returns 0 on success. */
int xhci_test_command_ring(void);
int xhci_configure_endpoint(uint8_t usb_addr, uint8_t endpoint, uint16_t max_packet, int ep_type);
int xhci_disable_slot(uint8_t slot_id);
/* U3: release a device's slot + contexts + rings (detach path). */
int xhci_free_device(uint8_t usb_addr);
int xhci_active_slot_count(void);
/* U4: re-program EP0 max packet once the real bMaxPacketSize0 is
 * known (full-speed devices only; no-op otherwise). */
int xhci_update_max_packet0(uint8_t dev_addr, uint16_t mps0);
int xhci_stop_endpoint(uint8_t slot_id, uint8_t ep_id);

int xhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int xhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);
int xhci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io);
int xhci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint,
                              int low_speed, uint16_t max_packet,
                              void *data, uint32_t len, int is_in);
int xhci_isochronous_transfer_ex(uint8_t dev_addr, uint8_t endpoint,
                                 uint16_t max_packet, void *data, uint32_t len,
                                 uint32_t num_tds, uint32_t *transferred);
int xhci_poll_event(void *event_trb_out);
int xhci_handle_events(void);
void xhci_self_test(void);

#endif
