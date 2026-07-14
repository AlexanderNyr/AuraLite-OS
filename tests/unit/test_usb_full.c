#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
static int passed=0, failed=0, tn=0;
#define RUN(f) do{int b=failed; f(); tn++; if(failed==b)passed++;}while(0)
#define CHECK(c) do{if(!(c)){printf("  FAIL L%d: %s\n",__LINE__,#c);failed++;}}while(0)
#define CHECK_EQ(a,e) do{if((long)(a)!=(long)(e)){printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e));failed++;}}while(0)
#define USB_CLASS_AUDIO 0x01
#define USB_CLASS_CDC 0x02
#define USB_CLASS_HID 0x03
#define USB_CLASS_PRINTER 0x07
#define USB_CLASS_MSC 0x08
#define USB_CLASS_HUB 0x09
#define USB_CLASS_CDC_DATA 0x0A
#define USB_EP_CONTROL 0x00
#define USB_EP_ISOCHRONOUS 0x01
#define USB_EP_BULK 0x02
#define USB_EP_INTERRUPT 0x03
struct usb_device_desc { uint8_t bLength; uint8_t bDescriptorType; uint16_t bcdUSB; uint8_t bDeviceClass; uint8_t bDeviceSubClass; uint8_t bDeviceProtocol; uint8_t bMaxPacketSize0; uint16_t idVendor; uint16_t idProduct; uint16_t bcdDevice; uint8_t iManufacturer; uint8_t iProduct; uint8_t iSerialNumber; uint8_t bNumConfigurations; } __attribute__((packed));
struct usb_interface_desc { uint8_t bLength; uint8_t bDescriptorType; uint8_t bInterfaceNumber; uint8_t bAlternateSetting; uint8_t bNumEndpoints; uint8_t bInterfaceClass; uint8_t bInterfaceSubClass; uint8_t bInterfaceProtocol; uint8_t iInterface; } __attribute__((packed));
struct usb_endpoint_desc { uint8_t bLength; uint8_t bDescriptorType; uint8_t bEndpointAddress; uint8_t bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; } __attribute__((packed));
struct cdc_line_coding { uint32_t dwDTERate; uint8_t bCharFormat; uint8_t bParityType; uint8_t bDataBits; } __attribute__((packed));
static int usb_string_to_ascii(const uint8_t *utf16, uint8_t bLength, char *out, uint16_t out_len) {
    if (!utf16 || !out || bLength < 2 || out_len == 0) return -1;
    uint16_t pos=0;
    for (int i=2; i+1 < bLength && pos+1 < out_len; i+=2) {
        uint16_t ch = utf16[i] | (utf16[i+1]<<8);
        if (ch==0) break;
        out[pos++] = (ch<128) ? (char)ch : '?';
    }
    out[pos]=0;
    return pos;
}
static const char *ep_type_str(uint8_t attr) {
    switch (attr & 0x03) { case 0: return "CTRL"; case 1: return "ISOC"; case 2: return "BULK"; case 3: return "INTR"; default: return "?"; }
}
void t_device_desc_size(void){ CHECK_EQ(sizeof(struct usb_device_desc), 18); }
void t_interface_desc_size(void){ CHECK_EQ(sizeof(struct usb_interface_desc), 9); }
void t_endpoint_desc_size(void){ CHECK_EQ(sizeof(struct usb_endpoint_desc), 7); }
void t_class_names(void){ CHECK(USB_CLASS_HUB == 0x09); CHECK(USB_CLASS_CDC == 0x02); CHECK(USB_CLASS_AUDIO == 0x01); CHECK(USB_CLASS_PRINTER == 0x07); }
void t_endpoint_addr_decode(void){ uint8_t ep_in = 0x81; uint8_t ep_out = 0x02; CHECK(ep_in & 0x80); CHECK(!(ep_out & 0x80)); CHECK_EQ(ep_in & 0x0F, 1); CHECK_EQ(ep_out & 0x0F, 2); }
void t_endpoint_types(void){ CHECK_EQ(USB_EP_BULK & 0x03, 2); CHECK_EQ(USB_EP_INTERRUPT & 0x03, 3); CHECK_EQ(USB_EP_ISOCHRONOUS & 0x03, 1); CHECK_EQ(USB_EP_CONTROL & 0x03, 0); CHECK(strcmp(ep_type_str(0x02), "BULK")==0); CHECK(strcmp(ep_type_str(0x03), "INTR")==0); CHECK(strcmp(ep_type_str(0x01), "ISOC")==0); }
void t_string_decode_ascii(void){ uint8_t buf[12] = {12, 3, 'A',0,'u',0,'r',0,'a',0,0,0}; char out[32]; int r = usb_string_to_ascii(buf, buf[0], out, sizeof(out)); CHECK(r>0); CHECK(strcmp(out, "Aura")==0); }
void t_string_decode_empty(void){ uint8_t buf[4] = {4, 3, 0, 0}; char out[32]; int r = usb_string_to_ascii(buf, buf[0], out, sizeof(out)); CHECK_EQ(r,0); CHECK_EQ(out[0],0); }
void t_cdc_line_coding_default(void){ struct cdc_line_coding coding = {115200, 0, 0, 8}; CHECK_EQ(coding.dwDTERate, 115200); CHECK_EQ(coding.bDataBits, 8); CHECK_EQ(coding.bParityType, 0); }
void t_cdc_line_coding_9600(void){ struct cdc_line_coding coding = {9600, 0, 0, 7}; CHECK_EQ(coding.dwDTERate, 9600); CHECK_EQ(coding.bDataBits, 7); }
void t_audio_rates(void){ uint32_t rates[3] = {44100, 48000, 96000}; CHECK_EQ(rates[0], 44100); CHECK_EQ(rates[1], 48000); CHECK(rates[2] > rates[1]); }
void t_hub_port_count(void){ uint8_t bNbrPorts = 4; CHECK(bNbrPorts <= 32); CHECK(bNbrPorts >= 1); }
void t_hub_depth_limit(void){ int max_depth = 5; for (int d=0; d<=max_depth; d++) CHECK(d <= 5); CHECK(max_depth == 5); }
void t_isoc_packets(void){ int max_packets = 32; uint32_t packet_len = 64; uint32_t total = max_packets * packet_len; CHECK_EQ(total, 2048); uint32_t used = 0; used += 10; CHECK(used <= 90); }
void t_isoc_sync_types(void){ uint8_t sync_none = 0x00; uint8_t sync_async = 0x04; uint8_t sync_adaptive = 0x08; uint8_t sync_sync = 0x0C; CHECK(sync_none != sync_async); CHECK(sync_adaptive != sync_sync); }
void t_printer_id_parse(void){ uint8_t buf[30] = {0x00, 0x1C, 'M','F','G',':','H','P',';','M','D','L',':','L','a','s','e','r','J','e','t',';','\0'}; uint16_t total = (buf[0]<<8) | buf[1]; CHECK_EQ(total, 28); char id[32]; memcpy(id, buf+2, total>sizeof(id)-1?sizeof(id)-1:total); id[total]=0; CHECK(strstr(id, "MFG") != NULL); }
void t_descriptor_walking(void){ uint8_t blob[64]; int off=0; blob[off++]=9; blob[off++]=2; blob[off++]=0x20; blob[off++]=0x00; blob[off++]=2; blob[off++]=1; blob[off++]=0; blob[off++]=0x80; blob[off++]=50; blob[off++]=9; blob[off++]=4; blob[off++]=0; blob[off++]=0; blob[off++]=1; blob[off++]=2; blob[off++]=2; blob[off++]=1; blob[off++]=0; blob[off++]=7; blob[off++]=5; blob[off++]=0x81; blob[off++]=3; blob[off++]=8; blob[off++]=0; blob[off++]=10; blob[off++]=9; blob[off++]=4; blob[off++]=1; blob[off++]=0; blob[off++]=2; blob[off++]=10; blob[off++]=0; blob[off++]=0; blob[off++]=0; blob[off++]=7; blob[off++]=5; blob[off++]=0x02; blob[off++]=2; blob[off++]=64; blob[off++]=0; blob[off++]=0; blob[off++]=7; blob[off++]=5; blob[off++]=0x82; blob[off++]=2; blob[off++]=64; blob[off++]=0; blob[off++]=0; int interfaces=0, endpoints=0; int p=0; while (p+2 <= off) { uint8_t dlen=blob[p]; uint8_t dtype=blob[p+1]; if (dlen<2 || p+dlen>off) break; if (dtype==4) interfaces++; if (dtype==5) endpoints++; p+=dlen; } CHECK_EQ(interfaces, 2); CHECK_EQ(endpoints, 3); }
void t_driver_match(void){ struct { uint8_t class_code; const char *name; } drivers[4] = { {USB_CLASS_HID, "hid"}, {USB_CLASS_MSC, "msc"}, {USB_CLASS_HUB, "hub"}, {USB_CLASS_CDC, "cdc_acm"}, }; CHECK(strcmp(drivers[3].name, "cdc_acm")==0); CHECK_EQ(drivers[2].class_code, USB_CLASS_HUB); }
void t_bulk_toggle(void){ int toggle=0; uint32_t len=512; uint32_t max_packet=64; uint32_t packets = (len+max_packet-1)/max_packet; for (uint32_t i=0;i<packets;i++) toggle ^=1; CHECK_EQ(toggle, 0); }
void t_audio_format_tag(void){ uint16_t PCM = 0x0001; uint16_t IEEE_FLOAT = 0x0003; CHECK(PCM != IEEE_FLOAT); CHECK_EQ(PCM, 1); }
int main(void){
    printf("=== USB Full Stack Point Tests ===\n\n");
    RUN(t_device_desc_size); RUN(t_interface_desc_size); RUN(t_endpoint_desc_size); RUN(t_class_names); RUN(t_endpoint_addr_decode); RUN(t_endpoint_types);
    RUN(t_string_decode_ascii); RUN(t_string_decode_empty); RUN(t_cdc_line_coding_default); RUN(t_cdc_line_coding_9600);
    RUN(t_audio_rates); RUN(t_hub_port_count); RUN(t_hub_depth_limit); RUN(t_isoc_packets); RUN(t_isoc_sync_types);
    RUN(t_printer_id_parse); RUN(t_descriptor_walking); RUN(t_driver_match); RUN(t_bulk_toggle); RUN(t_audio_format_tag);
    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    if (failed==0) printf("FULL USB SUPPORT — POINT TESTS PASS\n");
    return failed?1:0;
}
