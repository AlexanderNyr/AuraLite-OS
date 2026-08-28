#ifndef AURALITE_DRIVERS_USB_CDC_ACM_H
#define AURALITE_DRIVERS_USB_CDC_ACM_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define CDC_ACM_MAX_DEVICES 4
#define CDC_ACM_BUFFER_SIZE 512

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct cdc_line_coding {
    uint32_t dwDTERate;
    uint8_t bCharFormat;
    uint8_t bParityType;
    uint8_t bDataBits;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

#define CDC_CTRL_DTR (1<<0)
#define CDC_CTRL_RTS (1<<1)

#define CDC_NOTIF_NETWORK_CONNECTION 0x00
#define CDC_NOTIF_RESPONSE_AVAILABLE 0x01
#define CDC_NOTIF_SERIAL_STATE       0x20

typedef struct {
    int in_use;
    usb_device_t *dev;
    uint8_t comm_if;
    uint8_t data_if;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t interrupt_in;
    struct cdc_line_coding line_coding;
    uint8_t dtr, rts;
    int bulk_in_toggle;
    int bulk_out_toggle;
    int interrupt_toggle;
    uint32_t rx_count;
    uint32_t tx_count;
    char port_name[16];
} cdc_acm_device_t;

int cdc_acm_init(void);
int cdc_acm_attach_device(usb_device_t *dev);
void cdc_acm_detach_device(usb_device_t *dev);
int cdc_acm_set_line_coding(cdc_acm_device_t *acm, struct cdc_line_coding *coding);
int cdc_acm_get_line_coding(cdc_acm_device_t *acm, struct cdc_line_coding *coding);
int cdc_acm_set_control_line_state(cdc_acm_device_t *acm, uint8_t dtr, uint8_t rts);
int cdc_acm_send(cdc_acm_device_t *acm, const void *data, uint32_t len);
int cdc_acm_recv(cdc_acm_device_t *acm, void *data, uint32_t len);
int cdc_acm_send_break(cdc_acm_device_t *acm, uint16_t duration_ms);
cdc_acm_device_t *cdc_acm_get_device(int idx);
int cdc_acm_device_count(void);
void cdc_acm_self_test(void);

extern cdc_acm_device_t cdc_acm_devices[CDC_ACM_MAX_DEVICES];

#endif
