#ifndef AURALITE_DRIVERS_USB_OHCI_H
#define AURALITE_DRIVERS_USB_OHCI_H

#include <stdint.h>

#define OHCI_MAX_PORTS  15

int ohci_init(void);
int ohci_get_port_count(void);
int ohci_port_has_device(int port);
int ohci_port_is_low_speed(int port);
int ohci_reset_port(int port);
int ohci_suspend_port(int port);
int ohci_resume_port(int port);
int ohci_suspend(void);
int ohci_resume(void);

int ohci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int ohci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);
int ohci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io);
int ohci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint,
                              int low_speed, uint16_t max_packet,
                              void *data, uint16_t len, int is_in);
void ohci_self_test(void);

#endif
