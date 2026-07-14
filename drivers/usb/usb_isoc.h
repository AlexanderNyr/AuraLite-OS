#ifndef AURALITE_DRIVERS_USB_ISOC_H
#define AURALITE_DRIVERS_USB_ISOC_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define USB_ISOC_MAX_PACKETS 32
#define USB_ISOC_MAX_TRANSFERS 8

typedef struct {
    uint32_t length;
    uint32_t actual_length;
    uint32_t status;
} usb_isoc_packet_desc_t;

typedef struct {
    usb_device_t *dev;
    uint8_t endpoint;
    uint8_t num_packets;
    uint32_t total_length;
    uint32_t transferred;
    usb_isoc_packet_desc_t packets[USB_ISOC_MAX_PACKETS];
    void *buffer;
    uint64_t buffer_phys;
    int is_in;
    int active;
    uint64_t start_frame;
    uint32_t interval;
} usb_isoc_transfer_t;

typedef void (*usb_isoc_callback_t)(usb_isoc_transfer_t *transfer);

int usb_isoc_init(void);
usb_isoc_transfer_t *usb_isoc_alloc_transfer(usb_device_t *dev, uint8_t ep, uint8_t num_packets, uint32_t packet_len);
void usb_isoc_free_transfer(usb_isoc_transfer_t *transfer);
int usb_isoc_submit(usb_isoc_transfer_t *transfer, usb_isoc_callback_t cb);
int usb_isoc_cancel(usb_isoc_transfer_t *transfer);
int usb_isoc_transfer_simple(usb_device_t *dev, uint8_t ep, void *data, uint32_t len, uint32_t *transferred);
uint32_t usb_isoc_bandwidth_used(void);
uint32_t usb_isoc_max_bandwidth(void);
void usb_isoc_self_test(void);

#endif
