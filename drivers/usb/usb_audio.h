#ifndef AURALITE_DRIVERS_USB_AUDIO_H
#define AURALITE_DRIVERS_USB_AUDIO_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define USB_AUDIO_MAX_DEVICES 4
#define USB_AUDIO_MAX_STREAMS 2
#define USB_AUDIO_MAX_RATES 8

#define AUDIO_FORMAT_PCM 0x0001
#define AUDIO_FORMAT_PCM8 0x0001
#define AUDIO_FORMAT_IEEE_FLOAT 0x0003
#define AUDIO_FORMAT_ALAW 0x0008
#define AUDIO_FORMAT_MULAW 0x0010

typedef enum {
    USB_AUDIO_STREAM_NONE = 0,
    USB_AUDIO_STREAM_PLAYBACK,
    USB_AUDIO_STREAM_CAPTURE,
} usb_audio_stream_dir_t;

typedef struct {
    uint32_t sample_rates[USB_AUDIO_MAX_RATES];
    uint8_t num_rates;
    uint8_t num_channels;
    uint8_t bit_resolution;
    uint16_t format_tag;
    uint8_t interface_number;
    uint8_t alt_setting;
    uint8_t isoc_out_ep;
    uint8_t isoc_in_ep;
    uint16_t max_packet;
    uint8_t interval;
    uint8_t sync_type;
    int is_active;
} usb_audio_stream_t;

typedef struct {
    int in_use;
    usb_device_t *dev;
    uint8_t control_if;
    uint8_t num_streams;
    usb_audio_stream_t streams[USB_AUDIO_MAX_STREAMS];
    uint8_t feature_unit_id;
    uint8_t clock_source_id;
    int is_uac2;
    uint32_t current_rate;
    uint8_t current_channels;
    uint8_t current_bits;
    int mute;
    int16_t volume;
    uint32_t frames_played;
    uint32_t frames_captured;
    char name[32];
} usb_audio_device_t;

int usb_audio_init(void);
int usb_audio_attach_device(usb_device_t *dev);
void usb_audio_detach_device(usb_device_t *dev);
int usb_audio_set_alt(usb_audio_device_t *aud, uint8_t stream_idx, uint8_t alt);
int usb_audio_set_sample_rate(usb_audio_device_t *aud, uint8_t stream_idx, uint32_t rate);
int usb_audio_start_stream(usb_audio_device_t *aud, uint8_t stream_idx);
int usb_audio_stop_stream(usb_audio_device_t *aud, uint8_t stream_idx);
int usb_audio_write(usb_audio_device_t *aud, const void *data, uint32_t frames);
int usb_audio_read(usb_audio_device_t *aud, void *data, uint32_t frames);
int usb_audio_set_volume(usb_audio_device_t *aud, int16_t vol);
int usb_audio_set_mute(usb_audio_device_t *aud, int mute);
int usb_audio_device_count(void);
void usb_audio_self_test(void);

extern usb_audio_device_t usb_audio_devices[USB_AUDIO_MAX_DEVICES];

#endif
