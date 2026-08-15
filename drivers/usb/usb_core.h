#ifndef AURALITE_DRIVERS_USB_USB_CORE_H
#define AURALITE_DRIVERS_USB_USB_CORE_H

#include <stdint.h>

/*
 * USB Core: device enumeration and protocol layer — full support version.
 *
 * Sits above the host controller drivers (UHCI/OHCI/EHCI/xHCI) and implements
 * the standard USB enumeration sequence:
 *
 *   1. Detect device on a port (already done by controller drivers)
 *   2. GET_DESCRIPTOR(DEVICE) at address 0 to get max packet size
 *   3. SET_ADDRESS to assign a unique device address
 *   4. GET_DESCRIPTOR(DEVICE) at the new address for full descriptor
 *   5. GET_DESCRIPTOR(CONFIGURATION) for interface/endpoint info
 *   6. GET_DESCRIPTOR(STRING) for manufacturer/product/serial
 *   7. Parse all alternate interfaces, endpoint companions, class-specific
 *   8. SET_CONFIGURATION to activate the device
 *
 * Supports all transfer types: CONTROL, BULK, INTERRUPT, ISOCHRONOUS
 * Supports all speed grades: LOW, FULL, HIGH, SUPER
 * Supports driver model with registration and probing
 * Supports CDC ACM, Audio, Printer, Hub, HID, MSC class drivers
 */

#define USB_MAX_DEVICES  32
#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS  16
#define USB_MAX_STRING_LEN 128

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
#define USB_SET_SEL           48
#define USB_SET_ISOCH_DELAY   49

/* Descriptor types */
#define USB_DESC_DEVICE       1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_STRING       3
#define USB_DESC_INTERFACE    4
#define USB_DESC_ENDPOINT     5
#define USB_DESC_DEVICE_QUAL  6
#define USB_DESC_OTHER_SPEED  7
#define USB_DESC_IF_POWER     8
#define USB_DESC_OTG          9
#define USB_DESC_DEBUG        10
#define USB_DESC_IF_ASSOC     11
#define USB_DESC_BOS          15
#define USB_DESC_CAPS         16
#define USB_DESC_HID          0x21
#define USB_DESC_REPORT       0x22
#define USB_DESC_PHYSICAL     0x23
#define USB_DESC_HUB          0x29
#define USB_DESC_SS_EP_COMP   0x30
#define USB_DESC_SS_ISOCH_COMP 0x31

/* Device class codes */
#define USB_CLASS_USE_DEVICE   0x00
#define USB_CLASS_AUDIO        0x01
#define USB_CLASS_CDC          0x02
#define USB_CLASS_HID          0x03
#define USB_CLASS_PHYSICAL     0x05
#define USB_CLASS_IMAGE        0x06
#define USB_CLASS_PRINTER      0x07
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09
#define USB_CLASS_CDC_DATA     0x0A
#define USB_CLASS_SMARTCARD    0x0B
#define USB_CLASS_SECURITY     0x0D
#define USB_CLASS_VIDEO        0x0E
#define USB_CLASS_HEALTHCARE   0x0F
#define USB_CLASS_AUDIO_VIDEO  0x10
#define USB_CLASS_BILLBOARD    0x11
#define USB_CLASS_CDC_BRIDGE   0x12
#define USB_CLASS_DIAGNOSTIC   0xDC
#define USB_CLASS_WIRELESS     0xE0
#define USB_CLASS_MISC         0xEF
#define USB_CLASS_APP_SPEC     0xFE
#define USB_CLASS_VENDOR       0xFF

/* Audio subclasses */
#define USB_AUDIO_SUBCLASS_UNDEFINED 0x00
#define USB_AUDIO_SUBCLASS_CONTROL   0x01
#define USB_AUDIO_SUBCLASS_STREAMING 0x02
#define USB_AUDIO_SUBCLASS_MIDI      0x03

/* CDC subclasses */
#define USB_CDC_SUBCLASS_DLCM  0x01
#define USB_CDC_SUBCLASS_ACM   0x02
#define USB_CDC_SUBCLASS_TCM   0x03
#define USB_CDC_SUBCLASS_MCCM  0x04
#define USB_CDC_SUBCLASS_CCM   0x05
#define USB_CDC_SUBCLASS_EEM   0x0C
#define USB_CDC_SUBCLASS_NCM   0x0D

/* CDC protocols */
#define USB_CDC_PROTO_NONE     0x00
#define USB_CDC_PROTO_AT       0x01
#define USB_CDC_PROTO_VENDOR   0xFF

/* CDC ACM requests */
#define CDC_SEND_ENCAPSULATED_COMMAND 0x00
#define CDC_GET_ENCAPSULATED_RESPONSE 0x01
#define CDC_SET_COMM_FEATURE          0x02
#define CDC_GET_COMM_FEATURE          0x03
#define CDC_CLEAR_COMM_FEATURE        0x04
#define CDC_SET_LINE_CODING           0x20
#define CDC_GET_LINE_CODING           0x21
#define CDC_SET_CONTROL_LINE_STATE    0x22
#define CDC_SEND_BREAK                0x23

/* Printer class */
#define USB_PRINTER_SUBCLASS_PRINTER  0x01
#define USB_PRINTER_PROTO_UNI         0x01
#define USB_PRINTER_PROTO_BIDI        0x02
#define USB_PRINTER_PROTO_1284_4      0x03

/* Endpoint types */
#define USB_EP_CONTROL         0x00
#define USB_EP_ISOCHRONOUS     0x01
#define USB_EP_BULK            0x02
#define USB_EP_INTERRUPT       0x03

/* Endpoint direction */
#define USB_EP_DIR_OUT         0x00
#define USB_EP_DIR_IN          0x80

/* Endpoint sync and usage for isoc */
#define USB_EP_SYNC_MASK       0x0C
#define USB_EP_SYNC_NONE       0x00
#define USB_EP_SYNC_ASYNC      0x04
#define USB_EP_SYNC_ADAPTIVE   0x08
#define USB_EP_SYNC_SYNC       0x0C
#define USB_EP_USAGE_MASK      0x30
#define USB_EP_USAGE_DATA      0x00
#define USB_EP_USAGE_FEEDBACK  0x10
#define USB_EP_USAGE_IMPLICIT  0x20

/* Feature selectors */
#define USB_FEAT_ENDPOINT_HALT 0
#define USB_FEAT_DEVICE_REMOTE_WAKEUP 1
#define USB_FEAT_TEST_MODE 2

/* BOS descriptor for USB 2.1+ */
struct usb_bos_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumDeviceCaps;
} __attribute__((packed));

/* BOS Device Capability */
struct usb_cap_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bDevCapabilityType;
    uint8_t  data[];
} __attribute__((packed));

/* SS Endpoint Companion */
struct usb_ss_ep_comp_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bMaxBurst;
    uint8_t  bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

/* Interface Association Descriptor */
struct usb_iad_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bFirstInterface;
    uint8_t  bInterfaceCount;
    uint8_t  bFunctionClass;
    uint8_t  bFunctionSubClass;
    uint8_t  bFunctionProtocol;
    uint8_t  iFunction;
} __attribute__((packed));

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

/* Endpoint info for full stack */
/* USB_PLAN U9: device "location" encoding.
 *
 * A location is the root port plus the chain of downstream hub ports taken
 * to reach the device.  The old encoding packed all of it into one byte --
 * root port in the high nibble, one hub port in the low nibble -- which
 * has no room for a second level: a hub at location 0x51 computed 0x51 for
 * its own port 1, colliding with itself, so usb_find_by_location() matched
 * the hub and no device two hubs deep was ever enumerated.
 *
 * Layout now: bits 0-7 root port (1-based), then up to five 4-bit hub port
 * numbers in bits 8-11, 12-15, 16-19, 20-23, 24-27.  That is USB's own
 * depth limit of 5 and matches the xHCI route string directly. */
#define USB_LOC_ROOT(rp)          ((int)((rp) & 0xFF))
#define USB_LOC_DEPTH_MAX         5
static inline int usb_loc_depth(int loc) {
    int d = 0;
    for (int i = 0; i < USB_LOC_DEPTH_MAX; i++)
        if (((loc >> (8 + 4 * i)) & 0xF) != 0) d = i + 1;
    return d;
}
static inline int usb_loc_child(int loc, unsigned hub_port) {
    int d = usb_loc_depth(loc);
    if (d >= USB_LOC_DEPTH_MAX) return -1;          /* deeper than USB allows */
    return loc | (int)((hub_port & 0xF) << (8 + 4 * d));
}
/* Downstream hub-port chain as an xHCI route string (4 bits per tier). */
static inline unsigned usb_loc_route(int loc) { return (unsigned)((loc >> 8) & 0xFFFFF); }

typedef struct {
    uint8_t  address;
    uint8_t  attributes;
    uint16_t max_packet;
    uint8_t  interval;
    uint8_t  type;           /* CONTROL/BULK/INTERRUPT/ISOC */
    uint8_t  sync_type;
    uint8_t  usage_type;
    /* companion for SS */
    uint8_t  max_burst;
    uint16_t bytes_per_interval;
    int      toggle;
} usb_endpoint_info_t;

typedef struct {
    uint8_t  number;
    uint8_t  alt_setting;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  iInterface;
    uint8_t  num_endpoints;
    usb_endpoint_info_t endpoints[USB_MAX_ENDPOINTS];
    /* class-specific lengths */
    uint16_t hid_report_len;
    uint16_t extra_len;
    uint8_t  extra[128];
} usb_interface_info_t;

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
    uint16_t        bcd_device;
    uint8_t         bcd_usb_lo;
    uint8_t         num_configs;
    /* String indices */
    uint8_t         iManufacturer;
    uint8_t         iProduct;
    uint8_t         iSerial;
    char            manufacturer_str[USB_MAX_STRING_LEN];
    char            product_str[USB_MAX_STRING_LEN];
    char            serial_str[USB_MAX_STRING_LEN];
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
    /* Isochronous endpoints */
    uint8_t         isoc_in_ep;
    uint8_t         isoc_out_ep;
    uint16_t        isoc_max_packet;
    uint8_t         isoc_interval;
    uint8_t         isoc_sync_type;
    /* Hub bookkeeping. */
    uint8_t         hub_scanned;
    uint8_t         parent_hub_addr;
    uint8_t         parent_hub_port;
    uint8_t         hub_num_ports;
    uint8_t         hub_tt_think_time;
    /* Full interface list */
    uint8_t         num_interfaces;
    usb_interface_info_t interfaces[USB_MAX_INTERFACES];
    /* CDC ACM specific */
    uint8_t         cdc_comm_if;
    uint8_t         cdc_data_if;
    uint8_t         cdc_bulk_in;
    uint8_t         cdc_bulk_out;
    uint8_t         cdc_interrupt_in;
    /* Audio */
    uint8_t         audio_control_if;
    uint8_t         audio_streaming_if;
    uint8_t         audio_isoc_in;
    uint8_t         audio_isoc_out;
    /* Driver binding */
    void            *driver_data;
    int             driver_id;
    uint32_t        last_error;
    uint8_t         authorized;
    uint8_t         speed_reported;
} usb_device_t;

/* Global device table. */
extern usb_device_t usb_devices[USB_MAX_DEVICES];

/* ---- Driver model ---- */
#define USB_DRIVER_MAX 16

typedef struct usb_driver {
    const char *name;
    int (*probe)(usb_device_t *dev);
    void (*disconnect)(usb_device_t *dev);
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    int (*match_vendor)(uint16_t vid, uint16_t pid);
} usb_driver_t;

int usb_register_driver(usb_driver_t *drv);
int usb_unregister_driver(usb_driver_t *drv);
usb_driver_t *usb_find_driver(usb_device_t *dev);

/* ---- Transfer abstractions ---- */
int usb_control_transfer(usb_device_t *dev, const struct usb_setup_pkt *setup,
                         void *data, uint16_t data_len);
int usb_bulk_transfer(usb_device_t *dev, uint8_t endpoint,
                      void *data, uint32_t len);
int usb_bulk_transfer_ex(usb_device_t *dev, uint8_t endpoint,
                         void *data, uint32_t len, int *toggle_io);
int usb_interrupt_transfer(usb_device_t *dev, uint8_t endpoint,
                           void *data, uint16_t data_len, int *toggle_io);
int usb_isochronous_transfer(usb_device_t *dev, uint8_t endpoint,
                             void *data, uint32_t len, uint32_t *transferred);

/* Sync frame */
int usb_get_sync_frame(usb_device_t *dev, uint8_t endpoint, uint16_t *frame);

/* ---- Descriptor helpers ---- */
int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                       uint16_t lang, void *buf, uint16_t len);
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index,
                              uint16_t lang, void *buf, uint16_t len);
int usb_get_string_ascii(usb_device_t *dev, uint8_t index, char *out, uint16_t out_len);
int usb_clear_feature(usb_device_t *dev, uint8_t rcpt, uint16_t feature,
                      uint16_t index);
int usb_set_feature(usb_device_t *dev, uint8_t rcpt, uint16_t feature,
                    uint16_t index);
int usb_get_status(usb_device_t *dev, uint8_t rcpt, uint16_t index, uint16_t *status);

/* ---- Enumeration API ---- */
int usb_enumerate_all(void);
int usb_enumerate_device_at(usb_ctrl_type_t ctrl, int port, usb_speed_t speed);
int usb_reset_device(usb_device_t *dev);
int usb_authorize_device(usb_device_t *dev);

/* Diagnostics */
void usb_dump_devices(void);
void usb_dump_device_detail(usb_device_t *dev);
int usb_device_count(void);
usb_device_t *usb_find_device_by_class(uint8_t class_code);
usb_device_t *usb_find_device_by_vid_pid(uint16_t vid, uint16_t pid);
usb_device_t *usb_find_device_by_address(uint8_t addr);

/* Hotplug */
void usb_hotplug_poll(void);
int  usb_hotplug_start(void);

/* Self-test */
void usb_core_self_test(void);

/* UT helpers for tests (host side may include) */
uint16_t usb_le16_to_cpu(uint16_t v);
uint32_t usb_le32_to_cpu(uint32_t v);

#endif /* AURALITE_DRIVERS_USB_USB_CORE_H */
