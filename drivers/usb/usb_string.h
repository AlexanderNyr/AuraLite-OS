#ifndef AURALITE_DRIVERS_USB_STRING_H
#define AURALITE_DRIVERS_USB_STRING_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define USB_LANG_EN_US 0x0409
#define USB_LANG_MAX 32

typedef struct {
    uint16_t lang_ids[USB_LANG_MAX];
    uint8_t count;
} usb_lang_table_t;

int usb_string_init(void);
int usb_string_get_lang_table(usb_device_t *dev, usb_lang_table_t *table);
int usb_string_read(usb_device_t *dev, uint8_t index, uint16_t lang, char *out, uint16_t out_len);
int usb_string_read_all(usb_device_t *dev);
int usb_string_to_utf8(const uint8_t *utf16_le, uint8_t bLength, char *out, uint16_t out_len);
void usb_string_self_test(void);

#endif
