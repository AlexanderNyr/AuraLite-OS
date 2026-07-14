#ifndef AURALITE_DRIVERS_USB_HUB_H
#define AURALITE_DRIVERS_USB_HUB_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define USB_HUB_MAX_PORTS 32
#define USB_HUB_MAX_DEPTH 5
#define USB_HUB_MAX_HUBS  8

struct usb_hub_descriptor_full {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  DeviceRemovable[4];
    uint8_t  PortPwrCtrlMask[4];
} __attribute__((packed));

struct usb_ss_hub_descriptor {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  bHubHdrDecLat;
    uint16_t wHubDelay;
    uint16_t DeviceRemovable;
} __attribute__((packed));

typedef struct {
    int in_use;
    usb_device_t *dev;
    uint8_t num_ports;
    uint8_t power_good_ms;
    uint16_t characteristics;
    uint8_t depth;
    uint8_t parent_port;
    uint8_t tt_think_time;
    uint8_t port_power_mask;
    uint32_t port_status[USB_HUB_MAX_PORTS];
    uint32_t port_change[USB_HUB_MAX_PORTS];
    int has_interrupt_ep;
    uint8_t interrupt_ep;
    uint16_t max_packet;
} usb_hub_t;

int usb_hub_init(void);
int usb_hub_attach_device(usb_device_t *dev);
void usb_hub_detach_device(usb_device_t *dev);
int usb_hub_scan_all(void);
int usb_hub_get_port_count(usb_device_t *hub);
int usb_hub_reset_port(usb_device_t *hub, uint8_t port);
int usb_hub_power_on_port(usb_device_t *hub, uint8_t port);
int usb_hub_port_has_device(usb_device_t *hub, uint8_t port);
void usb_hub_self_test(void);

extern usb_hub_t usb_hubs[USB_HUB_MAX_HUBS];

#endif
