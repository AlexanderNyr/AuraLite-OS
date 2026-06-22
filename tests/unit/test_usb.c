/*
 * test_usb.c — unit tests for USB data structures and protocol encoding.
 *
#include <stddef.h>
 * Tests: CBW construction, CSW validation, setup packet layout,
 * TD token/control field encoding, USB descriptor field offsets.
 * 30+ test cases.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int passed=0, failed=0, tn=0;
#define RUN(f) do{int b=failed; f(); tn++; if(failed==b)passed++;}while(0)
#define CHECK(c) do{if(!(c)){printf("  FAIL L%d: %s\n",__LINE__,#c);failed++;}}while(0)
#define CHECK_EQ(a,e) do{if((long)(a)!=(long)(e)){printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e));failed++;}}while(0)

/* ---- USB setup packet (8 bytes) ---- */
struct usb_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* ---- CBW (31 bytes) ---- */
#define CBW_SIG 0x43425355
struct msc_cbw {
    uint32_t sig; uint32_t tag; uint32_t len;
    uint8_t flags; uint8_t lun; uint8_t cmd_len; uint8_t cbwcb[16];
} __attribute__((packed));

/* ---- CSW (13 bytes) ---- */
#define CSW_SIG 0x53425355
struct msc_csw {
    uint32_t sig; uint32_t tag; uint32_t residue; uint8_t status;
} __attribute__((packed));

/* ---- UHCI TD token field builder ---- */
#define TD_PID_SHIFT 0
#define TD_DEV_SHIFT 8
#define TD_EP_SHIFT  15
#define TD_DT_SHIFT  19
#define TD_LEN_SHIFT 21

static uint32_t make_token(uint8_t pid, uint8_t addr, uint8_t ep, int dt, uint32_t len) {
    uint32_t t=0;
    t |= (uint32_t)pid << TD_PID_SHIFT;
    t |= (uint32_t)(addr&0x7F) << TD_DEV_SHIFT;
    t |= (uint32_t)(ep&0xF) << TD_EP_SHIFT;
    t |= (uint32_t)(dt&1) << TD_DT_SHIFT;
    if (len==0) t |= 0x7FFu << TD_LEN_SHIFT;
    else t |= ((len-1)&0x7FF) << TD_LEN_SHIFT;
    return t;
}

/* ---- SCSI helpers ---- */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int advance_toggle(int start, uint32_t len, uint32_t max_packet) {
    uint32_t packets = (len + max_packet - 1) / max_packet;
    return (start ^ (int)(packets & 1));
}

/* ---- SCSI READ(10) builder ---- */
static void scsi_read10(uint8_t *c, uint32_t lba, uint16_t cnt) {
    memset(c,0,10);
    c[0]=0x28;
    c[2]=(lba>>24)&0xFF; c[3]=(lba>>16)&0xFF;
    c[4]=(lba>>8)&0xFF;  c[5]=lba&0xFF;
    c[7]=(cnt>>8)&0xFF;  c[8]=cnt&0xFF;
}

/* ---- Tests ---- */

/* Setup packet */
void t_setup_size(void){CHECK_EQ(sizeof(struct usb_setup),8);}
void t_setup_get_desc(void){
    struct usb_setup s={0x80,6,0x0100,0,18};
    CHECK_EQ(s.bmRequestType,0x80);
    CHECK_EQ(s.bRequest,6);
    CHECK_EQ(s.wValue,0x0100);
    CHECK_EQ(s.wLength,18);
}
void t_setup_set_addr(void){
    struct usb_setup s={0x00,5,5,0,0};
    CHECK_EQ(s.bRequest,5);
    CHECK_EQ(s.wValue,5);
}
void t_setup_set_config(void){
    struct usb_setup s={0x00,9,1,0,0};
    CHECK_EQ(s.bRequest,9);
    CHECK_EQ(s.wValue,1);
}
void t_setup_dir_in(void){
    struct usb_setup s={0x80,0,0,0,0};
    CHECK(s.bmRequestType & 0x80);
}
void t_setup_dir_out(void){
    struct usb_setup s={0x00,0,0,0,0};
    CHECK(!(s.bmRequestType & 0x80));
}

/* CBW */
void t_cbw_size(void){CHECK_EQ(sizeof(struct msc_cbw),31);}
void t_cbw_sig(void){
    struct msc_cbw cbw;
    memset(&cbw,0,sizeof(cbw));
    cbw.sig=CBW_SIG;
    CHECK_EQ(cbw.sig,0x43425355);
}
void t_cbw_flags_in(void){
    struct msc_cbw cbw={CBW_SIG,1,512,0x80,0,10,{0}};
    CHECK(cbw.flags & 0x80);
}
void t_cbw_flags_out(void){
    struct msc_cbw cbw={CBW_SIG,1,512,0x00,0,10,{0}};
    CHECK(!(cbw.flags & 0x80));
}
void t_cbw_cmd(void){
    struct msc_cbw cbw={CBW_SIG,1,512,0x80,0,10,{0}};
    cbw.cbwcb[0]=0x28;
    CHECK_EQ(cbw.cbwcb[0],0x28);
}

/* CSW */
void t_csw_size(void){CHECK_EQ(sizeof(struct msc_csw),13);}
void t_csw_sig(void){
    struct msc_csw csw;
    memset(&csw,0,sizeof(csw));
    csw.sig=CSW_SIG;
    CHECK_EQ(csw.sig,0x53425355);
}
void t_csw_status_ok(void){
    struct msc_csw csw={CSW_SIG,1,0,0};
    CHECK_EQ(csw.status,0);
}
void t_csw_status_fail(void){
    struct msc_csw csw={CSW_SIG,1,0,1};
    CHECK_EQ(csw.status,1);
}

/* TD token encoding */
void t_token_pid(void){
    uint32_t t=make_token(0x2D,0,0,0,8);
    CHECK_EQ(t & 0xFF, 0x2D);
}
void t_token_addr(void){
    uint32_t t=make_token(0x69,5,0,0,8);
    CHECK_EQ((t>>8)&0x7F, 5);
}
void t_token_ep(void){
    uint32_t t=make_token(0x69,1,2,0,64);
    CHECK_EQ((t>>15)&0xF, 2);
}
void t_token_toggle0(void){
    uint32_t t=make_token(0x2D,0,0,0,8);
    CHECK(!((t>>19)&1));
}
void t_token_toggle1(void){
    uint32_t t=make_token(0x69,0,0,1,8);
    CHECK((t>>19)&1);
}
void t_token_len8(void){
    uint32_t t=make_token(0x2D,0,0,0,8);
    CHECK_EQ((t>>21)&0x7FF, 7);  /* len-1 encoding */
}
void t_token_len64(void){
    uint32_t t=make_token(0x69,0,0,0,64);
    CHECK_EQ((t>>21)&0x7FF, 63);
}
void t_token_len0(void){
    uint32_t t=make_token(0x69,0,0,0,0);
    CHECK_EQ((t>>21)&0x7FF, 0x7FF);
}

/* SCSI commands */
void t_scsi_read10_lba(void){
    uint8_t c[10]; scsi_read10(c,0x12345678,1);
    CHECK_EQ(c[0],0x28);
    CHECK_EQ(c[2],0x12);
    CHECK_EQ(c[3],0x34);
    CHECK_EQ(c[4],0x56);
    CHECK_EQ(c[5],0x78);
}
void t_scsi_read10_count(void){
    uint8_t c[10]; scsi_read10(c,0,256);
    CHECK_EQ(c[7],1);
    CHECK_EQ(c[8],0);
}
void t_scsi_read10_lba0(void){
    uint8_t c[10]; scsi_read10(c,0,1);
    for(int i=2;i<6;i++) CHECK_EQ(c[i],0);
}
void t_read_capacity_parse(void){
    uint8_t cap[8]={0x00,0x00,0x7F,0xFF, 0x00,0x00,0x02,0x00};
    CHECK_EQ(be32(cap), 32767);
    CHECK_EQ(be32(cap+4), 512);
}
void t_bulk_toggle_31(void){
    CHECK_EQ(advance_toggle(0,31,64), 1); /* CBW: one OUT packet */
}
void t_bulk_toggle_512(void){
    CHECK_EQ(advance_toggle(0,512,64), 0); /* sector: eight packets */
}

/* Device descriptor field offsets */
void t_dev_desc_offsets(void){
    /* Standard USB device descriptor is 18 bytes */
    struct __attribute__((packed)) usb_dev_desc {
        uint8_t bLength,bDescriptorType; uint16_t bcdUSB;
        uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0;
        uint16_t idVendor,idProduct,bcdDevice;
        uint8_t iManufacturer,iProduct,iSerialNumber,bNumConfigurations;
    };
    CHECK_EQ(sizeof(struct usb_dev_desc), 18);
    CHECK_EQ(offsetof(struct usb_dev_desc, bMaxPacketSize0), 7);
}
void t_endpoint_addr_in(void){
    uint8_t addr=0x82; /* EP 2 IN */
    CHECK(addr & 0x80);
    CHECK_EQ(addr & 0x0F, 2);
}
void t_endpoint_addr_out(void){
    uint8_t addr=0x01; /* EP 1 OUT */
    CHECK(!(addr & 0x80));
    CHECK_EQ(addr & 0x0F, 1);
}

int main(void){
    printf("=== USB Protocol Tests ===\n\n");
    printf("--- setup packets ---\n");
    RUN(t_setup_size);RUN(t_setup_get_desc);RUN(t_setup_set_addr);
    RUN(t_setup_set_config);RUN(t_setup_dir_in);RUN(t_setup_dir_out);

    printf("--- CBW ---\n");
    RUN(t_cbw_size);RUN(t_cbw_sig);RUN(t_cbw_flags_in);
    RUN(t_cbw_flags_out);RUN(t_cbw_cmd);

    printf("--- CSW ---\n");
    RUN(t_csw_size);RUN(t_csw_sig);RUN(t_csw_status_ok);RUN(t_csw_status_fail);

    printf("--- TD token ---\n");
    RUN(t_token_pid);RUN(t_token_addr);RUN(t_token_ep);
    RUN(t_token_toggle0);RUN(t_token_toggle1);
    RUN(t_token_len8);RUN(t_token_len64);RUN(t_token_len0);

    printf("--- SCSI ---\n");
    RUN(t_scsi_read10_lba);RUN(t_scsi_read10_count);RUN(t_scsi_read10_lba0);
    RUN(t_read_capacity_parse);

    printf("--- bulk toggles ---\n");
    RUN(t_bulk_toggle_31);RUN(t_bulk_toggle_512);

    printf("--- descriptors ---\n");
    RUN(t_dev_desc_offsets);RUN(t_endpoint_addr_in);RUN(t_endpoint_addr_out);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",passed,tn,failed);
    return failed?1:0;
}
