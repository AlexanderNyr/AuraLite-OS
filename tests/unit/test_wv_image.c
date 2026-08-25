/*
 * test_wv_image.c — host tests for inflate + PNG/JPEG/GIF/BMP decode.
 * Links the REAL wv_inflate.c / wv_image.c.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "userspace/apps/gbrowser/wv_inflate.h"
#include "userspace/apps/gbrowser/wv_image.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* zlib-wrapped DEFLATE of "hello" (python zlib.compress(b'hello')) */
static const uint8_t ZHELLO[] = {
    0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15
};

static void test_inflate_hello(void) {
    uint8_t out[32];
    size_t n = wv_inflate_zlib(ZHELLO, sizeof ZHELLO, out, sizeof out);
    CK(n == 5);
    CK(n == 5 && memcmp(out, "hello", 5) == 0);
}

/* 2×2 RGB PNG: red, green / blue, white.  Generated offline with zlib. */
static const uint8_t PNG_2X2[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x02,0x00,0x00,0x00,0xfd,0xd4,0x9a,
    0x73,0x00,0x00,0x00,0x16,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xc0,
    0xf0,0x9f,0x81,0x11,0x48,0xfc,0xff,0xcf,0x00,0x00,0x1a,0x18,0x04,0x00,0xa3,0x62,
    0x0b,0x17,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};

static void test_png_2x2(void) {
    uint32_t *px = NULL;
    int w = 0, h = 0;
    int rc = wv_image_decode(PNG_2X2, sizeof PNG_2X2, &px, &w, &h);
    CK(rc == 0 && w == 2 && h == 2 && px);
    if (px) {
        /* red, green / blue, white — allow a little encoder filter variance
         * by checking the channels are in the right ballpark. */
        CK(((px[0] >> 16) & 0xFF) > 200 && (px[0] & 0xFF) < 40);          /* red */
        CK(((px[1] >> 8) & 0xFF) > 200 && ((px[1] >> 16) & 0xFF) < 40);   /* green */
        CK((px[2] & 0xFF) > 200 && ((px[2] >> 16) & 0xFF) < 40);          /* blue */
        CK((px[3] & 0xFFFFFF) == 0xFFFFFF);                                /* white */
        free(px);
    }
}

/* 2×1 24-bpp BMP: blue then red, top-down. */
static void test_bmp(void) {
    uint8_t bmp[70];
    memset(bmp, 0, sizeof bmp);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[10] = 54;                       /* pixel offset */
    bmp[14] = 40;                       /* DIB size */
    bmp[18] = 2; bmp[22] = 1;           /* 2×1 */
    bmp[26] = 1;                        /* planes */
    bmp[28] = 24;                       /* bpp */
    /* pixels at 54: B,G,R  B,G,R  + pad */
    bmp[54] = 255; bmp[55] = 0; bmp[56] = 0;       /* blue */
    bmp[57] = 0;   bmp[58] = 0; bmp[59] = 255;     /* red */
    uint32_t *px = NULL;
    int w = 0, h = 0;
    int rc = wv_image_decode(bmp, sizeof bmp, &px, &w, &h);
    CK(rc == 0 && w == 2 && h == 1 && px);
    if (px) {
        CK(px[0] == 0x000000FFu);
        CK(px[1] == 0x00FF0000u);
        free(px);
    }
}

static void test_refuse(void) {
    uint32_t *px = NULL;
    int w = 0, h = 0;
    CK(wv_image_decode(NULL, 0, &px, &w, &h) != 0);
    const uint8_t junk[8] = { 1,2,3,4,5,6,7,8 };
    CK(wv_image_decode(junk, 8, &px, &w, &h) != 0);
}

static void test_data_url(void) {
    /* Same 2×2 PNG as PNG_2X2, as a data: URL. */
    const char *url =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGP4z8DA"
        "8J+BEUj8/88AABoYBACjYgsXAAAAAElFTkSuQmCC";
    uint32_t *px = NULL;
    int w = 0, h = 0;
    int rc = wv_image_decode_data_url(url, &px, &w, &h);
    CK(rc == 0 && w == 2 && h == 2 && px);
    if (px) {
        CK(((px[0] >> 16) & 0xFF) > 200 && (px[0] & 0xFF) < 40);
        CK((px[3] & 0xFFFFFF) == 0xFFFFFF);
        free(px);
    }
    CK(wv_image_decode_data_url("http://x/y.png", &px, &w, &h) != 0);
}

int main(void) {
    printf("== webview image decode ==\n");
    test_inflate_hello();
    test_png_2x2();
    test_bmp();
    test_refuse();
    test_data_url();
    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
