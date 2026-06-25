#ifndef AURALITE_DRIVERS_USB_USB_CORE_H
#define AURALITE_DRIVERS_USB_USB_CORE_H

#include <stdint.h>

/*
 * USB Core: device enumeration and protocol layer.
 *
 * Sits above the host controller drivers (UHCI/OHCI/EHCI/xHCI) and implements
 * the standard USB enumeration sequence:
 *
 *   1. Detect device on a port (already done by controller drivers)
 *   2. GET_DESCRIPTOR(DEVICE) at address 0 to get max packet size
 *   3. SET_ADDRESS to assign a unique device address
 *   4. GET_DESCRIPTOR(DEVICE) at the new address for full descriptor
 *   5. GET_DESCRIPTOR(CONFIGURATION) for interface/endpoint info
 *   6. SET_CONFIGURATION to activate the device
 *
 * Then class drivers (HID keyboard/mouse, MSC mass storage) communicate via
 * control, interrupt, and bulk transfers.
 */

#define USB_MAX_DEVICES  16

/* ---- USB standard requests (bmRequestType values) ---- */
#define USB_REQ_DIR_OUT   0x00
#define USB_REQ_DIR_IN    0x80
#define USB_REQ_TYPE_STD  0x00
#define USB_REQ_TYPE_CLASS 0x20
#define USB_REQ_TYPE_VENDOR 0x40
#define USB_REQ_RCPT_DEV  0x00
#define USB_REQ_RCPT_IF   0x01
#define USB_REQ_RCPT_EP   0x02
#define USB_REQ_RCPT_OTHER 0x03

/* Standard request codes */
#define USB_GET_STATUS        0
#define USB_CLEAR_FEATURE     1
#define USB_SET_FEATURE       3
#define USB_SET_ADDRESS       5
#define USB_GET_DESCRIPTOR    6
#define USB_SET_DESCRIPTOR    7
#define USB_GET_CONFIGURATION 8
#define USB_SET_CONFIGURATION 9
#define USB_GET_INTERFACE     10
#define USB_SET_INTERFACE     11
#define USB_SYNCH_FRAME       12

/* Descriptor types */
#define USB_DESC_DEVICE       1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_STRING       3
#define USB_DESC_INTERFACE    4
#define USB_DESC_ENDPOINT     5
#define USB_DESC_HID          0x21
#define USB_DESC_REPORT       0x22
#define USB_DESC_HUB          0x29

/* Device class codes */
#define USB_CLASS_USE_DEVICE   0x00   /* use interface class */
#define USB_CLASS_AUDIO        0x01
#define USB_CLASS_CDC          0x02
#define USB_CLASS_HID          0x03
#define USB_CLASS_PHYSICAL     0x05
#define USB_CLASS_IMAGE        0x06
#define USB_CLASS_PRINTER      0x07
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09
#define USB_CLASS_CDC_DATA     0x0A
#define USB_CLASS_VENDOR       0xFF

/* Endpoint types */
#define USB_EP_CONTROL         0x00
#define USB_EP_ISOCHRONOUS     0x01
#define USB_EP_BULK            0x02
#define USB_EP_INTERRUPT       0x03

/* ---- USB data structures ---- */

/* Device descriptor (18 bytes). */
struct usb_device_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* Configuration descriptor (9 bytes, followed by interface + endpoint descs). */
struct usb_config_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* Interface descriptor (9 bytes). */
struct usb_interface_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* Endpoint descriptor (7 bytes). */
struct usb_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;    /* bit 7: direction (0=OUT, 1=IN) */
    uint8_t  bmAttributes;        /* bits 0-1: transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;          /* polling interval in ms (for interrupt) */
} __attribute__((packed));

/* Setup packet (8 bytes). */
struct usb_setup_pkt {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* ---- USB device record ---- */
typedef enum {
    USB_SPEED_LOW = 1,
    USB_SPEED_FULL = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_SUPER = 4,
} usb_speed_t;

typedef enum {
    USB_CTRL_UHCI,
    USB_CTRL_OHCI,
    USB_CTRL_EHCI,
    USB_CTRL_XHCI,
} usb_ctrl_type_t;

typedef struct {
    int             in_use;
    usb_ctrl_type_t controller;
    int             port;
    usb_speed_t     speed;
    uint8_t         address;          /* USB device address (1-127) */
    uint8_t         config_value;     /* active configuration */
    uint8_t         interface_class;  /* active/primary interface class */
    uint8_t         interface_subclass;
    uint8_t         interface_protocol;
    uint8_t         interface_number;
    uint8_t         max_packet_size0; /* endpoint 0 max packet size */
    uint16_t        vendor_id;
    uint16_t        product_id;
    /* Bulk endpoints for MSC. */
    uint8_t         bulk_in_ep;
    uint8_t         bulk_out_ep;
    uint16_t        bulk_max_packet;
    /* Interrupt endpoints for HID input. */
    uint8_t         interrupt_in_ep;
    uint8_t         interrupt_out_ep;
    uint16_t        interrupt_max_packet;
    uint8_t         interrupt_interval;
    uint16_t        hid_report_desc_len;
    /* Hub bookkeeping. */
    uint8_t         hub_scanned;
    uint8_t         parent_hub_addr;
    uint8_t         parent_hub_port;
} usb_device_t;

/* Global device table. */
extern usb_device_t usb_devices[USB_MAX_DEVICES];

/* ---- Control transfer abstraction ---- */

/*
 * Send a USB control transfer (SETUP → optional DATA → STATUS).
 * Uses endpoint 0 of the specified device.
 *
 * @param dev       device record (with address + speed set)
 * @param setup     8-byte setup packet
 * @param data      data buffer (NULL for no-data phase)
 * @param data_len  data length (0 for no-data)
 * @return bytes transferred, or -1 on error
 */
int usb_control_transfer(usb_device_t *dev, const struct usb_setup_pkt *setup,
                         void *data, uint16_t data_len);

/* Poll an interrupt endpoint (used by HID boot keyboard/mouse). `endpoint`
 * includes the direction bit. Returns bytes copied into `data`, 0 for no data
 * (e.g. NAK/no-change), or -1 for a hard error. */
int usb_interrupt_transfer(usb_device_t *dev, uint8_t endpoint,
                           void *data, uint16_t data_len, int *toggle_io);

/* ---- Enumeration API ---- */

/* Scan all detected ports across all controllers and enumerate devices.
 * For each port with a device:
 *   1. Read device descriptor at addr 0 (max packet size)
 *   2. SET_ADDRESS
 *   3. Read full device descriptor
 *   4. Read configuration descriptor
 *   5. SET_CONFIGURATION
 *   6. Record device class for class driver dispatch
 */
int usb_enumerate_all(void);

/* Print the device table. */
void usb_dump_devices(void);

/* Count enumerated devices. */
int usb_device_count(void);

/* Find the first device matching a class code. Returns NULL if none. */
usb_device_t *usb_find_device_by_class(uint8_t class_code);

/* Hotplug/re-enumeration support.  The monitor polls root ports and known hubs
 * for connect/disconnect changes and attaches supported class drivers for new
 * devices. */
void usb_hotplug_poll(void);
int  usb_hotplug_start(void);

/* Full USB subsystem self-test. */
void usb_core_self_test(void);

#endif /* AURALITE_DRIVERS_USB_USB_CORE_H */
