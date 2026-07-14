#ifndef AURALITE_DRIVERS_USB_UHCI_H
#define AURALITE_DRIVERS_USB_UHCI_H

#include <stdint.h>

/*
 * UHCI (Universal Host Controller Interface) USB 1.1 driver — FULL SUPPORT
 *
 * - Control, bulk, interrupt, isochronous via TD/QH frame list
 * - Memory leak fixes (all DMA buffers freed)
 * - Bandwidth tracking for isoc, suspend/resume, periodic scheduling
 * - Persistent toggle, SPD handling, NAK handling
 */

#define UHCI_MAX_PORTS   2

int uhci_init(void);
int uhci_get_port_count(void);
int uhci_port_has_device(int port);
int uhci_port_is_low_speed(int port);
int uhci_reset_port(int port);

int uhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data, uint16_t data_len);
int uhci_control_transfer_ex(uint8_t dev_addr, int low_speed,
                             const void *setup, void *data, uint16_t data_len,
                             uint8_t max_packet0);
int uhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len);
int uhci_bulk_transfer_ex(uint8_t dev_addr, uint8_t endpoint,
                          void *data, uint32_t len, int *toggle_io);
int uhci_interrupt_transfer_ex(uint8_t dev_addr, uint8_t endpoint,
                               int low_speed, uint16_t max_packet,
                               void *data, uint16_t len, int *toggle_io);
int uhci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint, int low_speed,
                              uint16_t max_packet, void *data, uint32_t len, int is_in);
int uhci_suspend(void);
int uhci_resume(void);
void uhci_self_test(void);

#endif
