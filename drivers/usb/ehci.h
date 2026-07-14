#ifndef AURALITE_DRIVERS_USB_EHCI_H
#define AURALITE_DRIVERS_USB_EHCI_H

#include <stdint.h>

#define EHCI_MAX_PORTS  15

int ehci_init(void);
int ehci_get_port_count(void);
int ehci_port_has_device(int port);
int ehci_reset_port_public(int port);
int ehci_suspend_port(int port);
int ehci_resume_port(int port);
int ehci_suspend(void);
int ehci_resume(void);

int ehci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int ehci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);
int ehci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io);
int ehci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint,
                              int low_speed, uint16_t max_packet,
                              void *data, uint32_t len, int is_in);
int ehci_split_transaction_setup(uint8_t hub_addr, uint8_t hub_port, uint8_t dev_addr, uint8_t endpoint);
void ehci_self_test(void);

#endif
