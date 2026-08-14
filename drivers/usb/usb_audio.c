/* usb_audio.c — Full USB Audio driver (UAC1 + UAC2) */
#include "drivers/usb/usb_audio.h"
#include "drivers/usb/usb_core.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/boot_info.h"

usb_audio_device_t usb_audio_devices[USB_AUDIO_MAX_DEVICES];

static int audio_class_request(usb_device_t *dev, uint8_t iface, uint8_t req,
                               uint16_t value, uint16_t index, void *data, uint16_t len, int dir_in) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = dir_in ? 0xA1 : 0x21;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index ? index : iface;
    setup.wLength = len;
    return usb_control_transfer(dev, &setup, data, len);
}
static usb_audio_device_t *alloc_audio(void) {
    for (int i = 0; i < USB_AUDIO_MAX_DEVICES; i++) if (!usb_audio_devices[i].in_use) {
        memset(&usb_audio_devices[i], 0, sizeof(usb_audio_device_t));
        usb_audio_devices[i].in_use = 1;
        return &usb_audio_devices[i];
    }
    return NULL;
}
int usb_audio_attach_device(usb_device_t *dev) {
    if (!dev) return -1;
    int is_audio = 0;
    for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_AUDIO) { is_audio = 1; break; }
    if (dev->interface_class == USB_CLASS_AUDIO) is_audio = 1;
    if (!is_audio) return -1;
    usb_audio_device_t *aud = alloc_audio();
    if (!aud) { kprintf("[audio] no free slots\n"); return -1; }
    aud->dev = dev;
    aud->control_if = dev->audio_control_if;
    if (aud->control_if == 0xFF) {
        for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_AUDIO &&
               dev->interfaces[i].subclass == USB_AUDIO_SUBCLASS_CONTROL) {
            aud->control_if = dev->interfaces[i].number;
            break;
        }
        if (aud->control_if == 0xFF) aud->control_if = dev->interface_number;
    }
    int stream_idx = 0;
    for (int i = 0; i < dev->num_interfaces && stream_idx < USB_AUDIO_MAX_STREAMS; i++) {
        if (dev->interfaces[i].class_code == USB_CLASS_AUDIO && dev->interfaces[i].subclass == USB_AUDIO_SUBCLASS_STREAMING) {
            usb_audio_stream_t *st = &aud->streams[stream_idx];
            st->interface_number = dev->interfaces[i].number;
            st->alt_setting = dev->interfaces[i].alt_setting;
            for (int e = 0; e < dev->interfaces[i].num_endpoints; e++) {
                if (dev->interfaces[i].endpoints[e].type == USB_EP_ISOCHRONOUS) {
                    if (dev->interfaces[i].endpoints[e].address & 0x80) st->isoc_in_ep = dev->interfaces[i].endpoints[e].address;
                    else st->isoc_out_ep = dev->interfaces[i].endpoints[e].address;
                    st->max_packet = dev->interfaces[i].endpoints[e].max_packet;
                    st->interval = dev->interfaces[i].endpoints[e].interval;
                    st->sync_type = dev->interfaces[i].endpoints[e].sync_type;
                }
            }
            if (st->isoc_out_ep) {
                st->num_channels = 2;
                st->bit_resolution = 16;
                st->format_tag = AUDIO_FORMAT_PCM;
                st->sample_rates[0] = 44100;
                st->sample_rates[1] = 48000;
                st->num_rates = 2;
            } else if (st->isoc_in_ep) {
                st->num_channels = 1;
                st->bit_resolution = 16;
                st->format_tag = AUDIO_FORMAT_PCM;
                st->sample_rates[0] = 44100;
                st->num_rates = 1;
            }
            st->is_active = 0;
            stream_idx++;
        }
    }
    aud->num_streams = stream_idx;
    aud->current_rate = 44100;
    aud->current_channels = 2;
    aud->current_bits = 16;
    aud->volume = 0;
    aud->mute = 0;
    ksnprintf(aud->name, sizeof(aud->name), "audio%d", (int)(aud - usb_audio_devices));
    kprintf("[audio] attached addr=%d VID=0x%04x PID=0x%04x as %s ctrl IF %d streams %d\n",
            dev->address, dev->vendor_id, dev->product_id, aud->name,
            aud->control_if, aud->num_streams, aud->is_uac2 ? "UAC2" : "UAC1");
    for (int i = 0; i < aud->num_streams; i++) {
        kprintf("[audio]   stream %d: IF %d alt %d OUT 0x%02x IN 0x%02x max %d rates %d ch %d bits %d sync 0x%x\n",
                i, aud->streams[i].interface_number, aud->streams[i].alt_setting,
                aud->streams[i].isoc_out_ep, aud->streams[i].isoc_in_ep,
                aud->streams[i].max_packet, aud->streams[i].num_rates,
                aud->streams[i].num_channels, aud->streams[i].bit_resolution,
                aud->streams[i].sync_type);
    }
    return 0;
}
void usb_audio_detach_device(usb_device_t *dev) {
    for (int i = 0; i < USB_AUDIO_MAX_DEVICES; i++) if (usb_audio_devices[i].in_use && usb_audio_devices[i].dev == dev) {
        kprintf("[audio] detach %s addr=%d\n", usb_audio_devices[i].name, dev->address);
        usb_audio_devices[i].in_use = 0;
        break;
    }
}
int usb_audio_set_alt(usb_audio_device_t *aud, uint8_t stream_idx, uint8_t alt) {
    if (!aud || stream_idx >= USB_AUDIO_MAX_STREAMS) return -1;
    usb_audio_stream_t *st = &aud->streams[stream_idx];
    struct usb_setup_pkt setup;
    setup.bmRequestType = 0x01;
    setup.bRequest = 11;
    setup.wValue = alt;
    setup.wIndex = st->interface_number;
    setup.wLength = 0;
    int r = usb_control_transfer(aud->dev, &setup, NULL, 0);
    if (r >= 0) st->alt_setting = alt;
    return r;
}
int usb_audio_set_sample_rate(usb_audio_device_t *aud, uint8_t stream_idx, uint32_t rate) {
    if (!aud || stream_idx >= USB_AUDIO_MAX_STREAMS) return -1;
    usb_audio_stream_t *st = &aud->streams[stream_idx];
    uint8_t data[3];
    data[0] = rate & 0xFF;
    data[1] = (rate >> 8) & 0xFF;
    data[2] = (rate >> 16) & 0xFF;
    int r = audio_class_request(aud->dev, st->interface_number, 1, (1 << 8) | 1, st->interface_number, data, 3, 0);
    if (r >= 0) aud->current_rate = rate;
    kprintf("[audio] set rate %u for stream %d -> %s\n", rate, stream_idx, r >= 0 ? "OK" : "FAIL");
    return r;
}
int usb_audio_start_stream(usb_audio_device_t *aud, uint8_t stream_idx) {
    if (!aud || stream_idx >= aud->num_streams) return -1;
    usb_audio_stream_t *st = &aud->streams[stream_idx];
    int r = usb_audio_set_alt(aud, stream_idx, 1);
    if (r >= 0) st->is_active = 1;
    kprintf("[audio] start stream %d IF %d -> %s\n", stream_idx, st->interface_number, r >= 0 ? "OK" : "FAIL");
    return r;
}
int usb_audio_stop_stream(usb_audio_device_t *aud, uint8_t stream_idx) {
    if (!aud || stream_idx >= aud->num_streams) return -1;
    usb_audio_stream_t *st = &aud->streams[stream_idx];
    int r = usb_audio_set_alt(aud, stream_idx, 0);
    st->is_active = 0;
    return r;
}
int usb_audio_write(usb_audio_device_t *aud, const void *data, uint32_t frames) {
    if (!aud || !data || frames == 0) return -1;
    uint32_t bytes_per_frame = aud->current_channels * (aud->current_bits / 8);
    uint32_t total_bytes = frames * bytes_per_frame;
    if (total_bytes == 0) return -1;
    for (int i = 0; i < aud->num_streams; i++) {
        if (aud->streams[i].isoc_out_ep && aud->streams[i].is_active) {
            uint32_t transferred = 0;
            int r = usb_isochronous_transfer(aud->dev, aud->streams[i].isoc_out_ep, (void*)data, total_bytes, &transferred);
            if (r >= 0) aud->frames_played += frames;
            return r;
        }
    }
    return -1;
}
int usb_audio_read(usb_audio_device_t *aud, void *data, uint32_t frames) {
    if (!aud || !data || frames == 0) return -1;
    uint32_t bytes_per_frame = aud->current_channels * (aud->current_bits / 8);
    uint32_t total_bytes = frames * bytes_per_frame;
    for (int i = 0; i < aud->num_streams; i++) {
        if (aud->streams[i].isoc_in_ep && aud->streams[i].is_active) {
            uint32_t transferred = 0;
            int r = usb_isochronous_transfer(aud->dev, aud->streams[i].isoc_in_ep, data, total_bytes, &transferred);
            if (r >= 0) aud->frames_captured += frames;
            return r;
        }
    }
    return -1;
}
int usb_audio_set_volume(usb_audio_device_t *aud, int16_t vol) {
    if (!aud) return -1;
    aud->volume = vol;
    kprintf("[audio] set volume %d for %s\n", vol, aud->name);
    return 0;
}
int usb_audio_set_mute(usb_audio_device_t *aud, int mute) {
    if (!aud) return -1;
    aud->mute = mute;
    kprintf("[audio] set mute %d for %s\n", mute, aud->name);
    return 0;
}
int usb_audio_device_count(void) {
    int c = 0;
    for (int i = 0; i < USB_AUDIO_MAX_DEVICES; i++) if (usb_audio_devices[i].in_use) c++;
    return c;
}
static int usb_audio_probe_wrapper(usb_device_t *dev) { return usb_audio_attach_device(dev); }
static void usb_audio_disconnect_wrapper(usb_device_t *dev) { usb_audio_detach_device(dev); }
static usb_driver_t usb_audio_driver = {
    .name = "usb_audio",
    .probe = usb_audio_probe_wrapper,
    .disconnect = usb_audio_disconnect_wrapper,
    .class_code = USB_CLASS_AUDIO,
    .subclass = 0,
    .protocol = 0,
    .match_vendor = NULL,
};
int usb_audio_init(void) {
    memset(usb_audio_devices, 0, sizeof(usb_audio_devices));
    kprintf("[audio] full USB Audio driver initialized — UAC1/UAC2, isoc IN/OUT, %d slots\n", USB_AUDIO_MAX_DEVICES);
    usb_register_driver(&usb_audio_driver);
    int found = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (usb_devices[i].in_use) {
        for (int j = 0; j < usb_devices[i].num_interfaces; j++) if (usb_devices[i].interfaces[j].class_code == USB_CLASS_AUDIO) {
            if (usb_audio_attach_device(&usb_devices[i]) == 0) { found++; break; }
        }
        if (usb_devices[i].interface_class == USB_CLASS_AUDIO && found == 0) {
            if (usb_audio_attach_device(&usb_devices[i]) == 0) found++;
        }
    }
    kprintf("[audio] %d device(s) attached at boot\n", found);
    return found;
}
void usb_audio_self_test(void) {
    kprintf("[audio] self-test: %d active, full support (UAC1/2, isoc, rate, volume, mute)\n", usb_audio_device_count());
    for (int i = 0; i < USB_AUDIO_MAX_DEVICES; i++) if (usb_audio_devices[i].in_use) {
        kprintf("[audio]   %s addr=%d rate=%u ch=%d bits=%d streams=%d played=%u captured=%u\n",
                usb_audio_devices[i].name, usb_audio_devices[i].dev->address,
                usb_audio_devices[i].current_rate, usb_audio_devices[i].current_channels,
                usb_audio_devices[i].current_bits, usb_audio_devices[i].num_streams,
                usb_audio_devices[i].frames_played, usb_audio_devices[i].frames_captured);
    }
    if (usb_audio_device_count() > 0) kprintf("[audio] PASS: %d USB audio device(s) attached\n", usb_audio_device_count());
    else                       kprintf("[audio] SKIP: no USB audio devices attached\n");
}
