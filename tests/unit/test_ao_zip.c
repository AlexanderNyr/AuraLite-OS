/* tests/unit/test_ao_zip.c — OF7 ZIP reader, deterministic writer, .aod. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "userspace/office/libaodoc/aozip.h"
#include "userspace/office/libaodoc/aopackage.h"
#include "userspace/office/libaodoc/aomodel.h"

static unsigned checks;
static unsigned failures;
#define CHECK(expr) do { checks++; if (expr) printf("PASS: %s\n", #expr); \
    else { failures++; printf("FAIL: %s (line %d)\n", #expr, __LINE__); } } while (0)

/* Created by Python zipfile: one stored file and one raw-DEFLATE file. */
static const uint8_t fixture[] = {
    0x50,0x4B,0x03,0x04,0x14,0x00,0x00,0x00,0x00,0x00,0x83,0x18,
    0x22,0x50,0xD0,0xCF,0x1C,0xFC,0x0E,0x00,0x00,0x00,0x0E,0x00,
    0x00,0x00,0x0A,0x00,0x00,0x00,0x73,0x74,0x6F,0x72,0x65,0x64,
    0x2E,0x74,0x78,0x74,0x53,0x54,0x4F,0x52,0x45,0x44,0x20,0x70,
    0x61,0x79,0x6C,0x6F,0x61,0x64,0x50,0x4B,0x03,0x04,0x14,0x00,
    0x00,0x00,0x08,0x00,0x83,0x18,0x22,0x50,0xB3,0xE7,0x77,0xFB,
    0x27,0x00,0x00,0x00,0x25,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,
    0x64,0x65,0x66,0x6C,0x61,0x74,0x65,0x64,0x2E,0x74,0x78,0x74,
    0x4B,0x49,0x4D,0xCB,0x49,0x2C,0x49,0x4D,0x51,0x28,0x48,0xAC,
    0xCC,0xC9,0x4F,0x4C,0xB1,0x52,0xC8,0x48,0xCD,0xC9,0xC9,0xD7,
    0x51,0x88,0xF2,0x0C,0x50,0x48,0xCC,0x4B,0x51,0x88,0xF0,0xF5,
    0x51,0x04,0x00,0x50,0x4B,0x01,0x02,0x14,0x03,0x14,0x00,0x00,
    0x00,0x00,0x00,0x83,0x18,0x22,0x50,0xD0,0xCF,0x1C,0xFC,0x0E,
    0x00,0x00,0x00,0x0E,0x00,0x00,0x00,0x0A,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x00,0x00,0x00,
    0x00,0x73,0x74,0x6F,0x72,0x65,0x64,0x2E,0x74,0x78,0x74,0x50,
    0x4B,0x01,0x02,0x14,0x03,0x14,0x00,0x00,0x00,0x08,0x00,0x83,
    0x18,0x22,0x50,0xB3,0xE7,0x77,0xFB,0x27,0x00,0x00,0x00,0x25,
    0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x80,0x01,0x36,0x00,0x00,0x00,0x64,0x65,0x66,
    0x6C,0x61,0x74,0x65,0x64,0x2E,0x74,0x78,0x74,0x50,0x4B,0x05,
    0x06,0x00,0x00,0x00,0x00,0x02,0x00,0x02,0x00,0x72,0x00,0x00,
    0x00,0x87,0x00,0x00,0x00,0x00,0x00
};

#define OUT_CAP 65536u
static uint8_t out_a[OUT_CAP], out_b[OUT_CAP], out_many[OUT_CAP];
static uint8_t xml_scratch[8192], import_scratch[8192];
static char model_text[2048], back_text[2048];
static ao_run_t model_runs[64], back_runs[64];
static ao_para_t model_paras[32], back_paras[32];

typedef struct { uint8_t *p; size_t cap, len; } buffer_t;
static int to_buffer(void *ctx, const void *bytes, size_t n) {
    buffer_t *b = (buffer_t *)ctx;
    if (n > b->cap - b->len) return -1;
    if (n) memcpy(b->p + b->len, bytes, n);
    b->len += n;
    return 0;
}

static void test_read_fixture(void) {
    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, fixture, sizeof(fixture)) == AO_ZIP_OK);
    CHECK(ao_zip_reader_count(&r) == 2);
    ao_zip_entry_t e;
    CHECK(ao_zip_reader_find(&r, "stored.txt", 10, &e) == AO_ZIP_OK &&
          e.method == AO_ZIP_STORE && e.uncompressed_size == 14);
    const uint8_t *view = NULL; size_t view_len = 0;
    CHECK(ao_zip_entry_view(&r, &e, &view, &view_len) == AO_ZIP_OK &&
          view_len == 14 && memcmp(view, "STORED payload", 14) == 0);

    CHECK(ao_zip_reader_find(&r, "deflated.txt", 12, &e) == AO_ZIP_OK &&
          e.method == AO_ZIP_DEFLATE && e.uncompressed_size == 37);
    CHECK(ao_zip_entry_view(&r, &e, &view, &view_len) == AO_ZIP_E_METHOD);
    char decoded[64]; size_t decoded_len = 0;
    CHECK(ao_zip_extract(&r, &e, decoded, sizeof(decoded), &decoded_len) == AO_ZIP_OK &&
          decoded_len == 37 && memcmp(decoded, "deflated payload: hello, ZIP and XML!", 37) == 0);
    CHECK(ao_zip_extract(&r, &e, decoded, 4, &decoded_len) == AO_ZIP_E_FULL);
    CHECK(ao_zip_crc32("123456789", 9) == 0xCBF43926u);
}

static void test_malformed(void) {
    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, fixture, sizeof(fixture) - 5u) == AO_ZIP_E_FORMAT);

    uint8_t bad[sizeof(fixture)];
    memcpy(bad, fixture, sizeof(bad));
    /* Lying uncompressed size: local + central directory agree, DEFLATE does not. */
    bad[76] = 0x26; /* local uncompressed size at offset 54+22 */
    bad[215] = 0x26; /* second central record uncompressed size at 191+24 */
    CHECK(ao_zip_reader_init(&r, bad, sizeof(bad)) == AO_ZIP_OK);
    ao_zip_entry_t e;
    CHECK(ao_zip_reader_find(&r, "deflated.txt", 12, &e) == AO_ZIP_OK);
    char decoded[64]; size_t n = 0;
    CHECK(ao_zip_extract(&r, &e, decoded, sizeof(decoded), &n) == AO_ZIP_E_SIZE);

    memcpy(bad, fixture, sizeof(bad));
    bad[14] ^= 0x01;    /* corrupt stored-entry CRC in local header */
    bad[151] ^= 0x01;   /* and in central directory, so header consistency holds */
    CHECK(ao_zip_reader_init(&r, bad, sizeof(bad)) == AO_ZIP_OK);
    CHECK(ao_zip_reader_find(&r, "stored.txt", 10, &e) == AO_ZIP_OK);
    const uint8_t *view = NULL; size_t view_len = 0;
    CHECK(ao_zip_entry_view(&r, &e, &view, &view_len) == AO_ZIP_E_CRC);

    uint8_t zip64_eocd[22] = {
        0x50,0x4B,0x05,0x06, 0,0, 0,0, 0xFF,0xFF, 0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0,0
    };
    CHECK(ao_zip_reader_init(&r, zip64_eocd, sizeof(zip64_eocd)) == AO_ZIP_E_ZIP64);
}

static int add_roundtrip_entries(ao_zip_writer_t *w) {
    int rc = ao_zip_writer_add(w, "one.txt", 7, "one", 3);
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_add(w, "two.txt", 7, "two\n", 4);
    return rc;
}

static void test_writer_determinism_and_many(void) {
    buffer_t a = { out_a, sizeof(out_a), 0 }, b = { out_b, sizeof(out_b), 0 };
    ao_zip_writer_t wa, wb;
    CHECK(ao_zip_writer_init(&wa, to_buffer, &a) == AO_ZIP_OK);
    CHECK(add_roundtrip_entries(&wa) == AO_ZIP_OK);
    size_t size_a = 0;
    CHECK(ao_zip_writer_finish(&wa, &size_a) == AO_ZIP_OK && size_a == a.len);
    CHECK(ao_zip_writer_init(&wb, to_buffer, &b) == AO_ZIP_OK);
    CHECK(add_roundtrip_entries(&wb) == AO_ZIP_OK);
    size_t size_b = 0;
    CHECK(ao_zip_writer_finish(&wb, &size_b) == AO_ZIP_OK && size_a == size_b &&
          memcmp(out_a, out_b, size_a) == 0);
    CHECK(ao_zip_writer_finish(&wb, NULL) == AO_ZIP_E_STATE);

    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, out_a, size_a) == AO_ZIP_OK &&
          ao_zip_reader_count(&r) == 2);
    ao_zip_entry_t e;
    CHECK(ao_zip_reader_find(&r, "two.txt", 7, &e) == AO_ZIP_OK);
    char text[8]; size_t n = 0;
    CHECK(ao_zip_extract(&r, &e, text, sizeof(text), &n) == AO_ZIP_OK &&
          n == 4 && memcmp(text, "two\n", 4) == 0);

    buffer_t many = { out_many, sizeof(out_many), 0 };
    ao_zip_writer_t wm;
    CHECK(ao_zip_writer_init(&wm, to_buffer, &many) == AO_ZIP_OK);
    for (unsigned i = 0; i < 50; i++) {
        char name[24], data[24];
        int nn = snprintf(name, sizeof(name), "files/item-%02u.txt", i);
        int dn = snprintf(data, sizeof(data), "payload-%u", i);
        if (nn <= 0 || dn <= 0 || ao_zip_writer_add(&wm, name, (size_t)nn,
                                                    data, (size_t)dn) != AO_ZIP_OK) {
            failures++; checks++; printf("FAIL: add 50-entry item %u\n", i); return;
        }
        checks++; printf("PASS: add 50-entry item %u\n", i);
    }
    size_t many_size = 0;
    CHECK(ao_zip_writer_finish(&wm, &many_size) == AO_ZIP_OK);
    CHECK(ao_zip_reader_init(&r, out_many, many_size) == AO_ZIP_OK &&
          ao_zip_reader_count(&r) == 50);
    for (unsigned i = 0; i < 50; i++) {
        char name[24], expected[24], actual[24]; size_t actual_len = 0;
        int nn = snprintf(name, sizeof(name), "files/item-%02u.txt", i);
        int dn = snprintf(expected, sizeof(expected), "payload-%u", i);
        if (ao_zip_reader_find(&r, name, (size_t)nn, &e) != AO_ZIP_OK ||
            ao_zip_extract(&r, &e, actual, sizeof(actual), &actual_len) != AO_ZIP_OK ||
            actual_len != (size_t)dn || memcmp(actual, expected, actual_len) != 0) {
            failures++; checks++; printf("FAIL: read 50-entry item %u\n", i); return;
        }
        checks++; printf("PASS: read 50-entry item %u\n", i);
    }

    b.len = 0;
    CHECK(ao_zip_writer_init(&wb, to_buffer, &b) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&wb, "../escape", 9, "x", 1) == AO_ZIP_E_FORMAT);
    CHECK(ao_zip_writer_add(&wb, "one.txt", 7, "1", 1) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&wb, "one.txt", 7, "2", 1) == AO_ZIP_E_DUP);
}

static int package_setup(ao_model_t *m, char *text, ao_run_t *runs,
                         ao_para_t *paras) {
    int rc = ao_model_init(m, text, 2048, runs, 64, paras, 32);
    if (rc != AO_OK) return rc;
    rc = ao_model_insert(m, 0, "Native ZIP & XML", 16);
    if (rc != AO_OK) return rc;
    rc = ao_model_set_style(m, 0, 6, AO_STYLE_BOLD, 1);
    if (rc != AO_OK) return rc;
    rc = ao_model_insert(m, ao_model_text_len(m), "\nsecond paragraph", 17);
    return rc;
}

static void test_package_and_legacy(void) {
    ao_model_t m, back;
    CHECK(package_setup(&m, model_text, model_runs, model_paras) == AO_OK);
    CHECK(ao_model_init(&back, back_text, sizeof(back_text), back_runs, 64,
                        back_paras, 32) == AO_OK);
    buffer_t packaged = { out_a, sizeof(out_a), 0 };
    size_t archive_size = 0, xml_size = 0;
    CHECK(ao_package_export(&m, to_buffer, &packaged,
                           &archive_size, &xml_size) == AO_OK &&
          archive_size == packaged.len && xml_size > 0);
    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, out_a, archive_size) == AO_ZIP_OK &&
          ao_zip_reader_count(&r) == 3);
    ao_zip_entry_t e;
    CHECK(ao_zip_reader_entry_at(&r, 0, &e) == AO_ZIP_OK &&
          e.name_len == 8 && memcmp(e.name, "mimetype", 8) == 0 &&
          e.method == AO_ZIP_STORE);
    CHECK(ao_zip_reader_entry_at(&r, 1, &e) == AO_ZIP_OK &&
          e.name_len == 11 && memcmp(e.name, "content.xml", 11) == 0);
    CHECK(ao_zip_reader_entry_at(&r, 2, &e) == AO_ZIP_OK &&
          e.name_len == 10 && memcmp(e.name, "styles.xml", 10) == 0);
    CHECK(ao_package_import(&back, out_a, archive_size, (char *)import_scratch,
                            sizeof(import_scratch)) == AO_OK);
    CHECK(ao_model_text_len(&back) == ao_model_text_len(&m) &&
          memcmp(back_text, model_text, ao_model_text_len(&m)) == 0 &&
          ao_model_style_at(&back, 0) == AO_STYLE_BOLD);

    /* Native packages are exactly three parts, with mimetype physically first. */
    {
        buffer_t extra = { out_b, sizeof(out_b), 0 };
        ao_zip_writer_t w;
        CHECK(ao_zip_writer_init(&w, to_buffer, &extra) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_MIMETYPE_PART, 8,
                                AO_PACKAGE_MIMETYPE,
                                sizeof(AO_PACKAGE_MIMETYPE) - 1u) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_CONTENT_PART, 11,
                                "<aodoc/>", 8) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_STYLES_PART, 10,
                                "<styles/>", 9) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, "extra.xml", 9, "<extra/>", 8) == AO_ZIP_OK);
        size_t extra_len = 0;
        CHECK(ao_zip_writer_finish(&w, &extra_len) == AO_ZIP_OK);
        CHECK(ao_package_import(&back, out_b, extra_len, (char *)import_scratch,
                                sizeof(import_scratch)) == AO_E_FORMAT);
    }
    {
        buffer_t reversed = { out_b, sizeof(out_b), 0 };
        ao_zip_writer_t w;
        CHECK(ao_zip_writer_init(&w, to_buffer, &reversed) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_CONTENT_PART, 11,
                                "<aodoc/>", 8) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_MIMETYPE_PART, 8,
                                AO_PACKAGE_MIMETYPE,
                                sizeof(AO_PACKAGE_MIMETYPE) - 1u) == AO_ZIP_OK);
        CHECK(ao_zip_writer_add(&w, AO_PACKAGE_STYLES_PART, 10,
                                "<styles/>", 9) == AO_ZIP_OK);
        size_t reversed_len = 0;
        CHECK(ao_zip_writer_finish(&w, &reversed_len) == AO_ZIP_OK);
        CHECK(ao_package_import(&back, out_b, reversed_len,
                                (char *)import_scratch, sizeof(import_scratch)) == AO_E_FORMAT);
    }

    size_t legacy_len = 0;
    CHECK(ao_io_export(&m, (char *)xml_scratch, sizeof(xml_scratch),
                       &legacy_len) == AO_OK);
    buffer_t streamed_xml = { (uint8_t *)import_scratch, sizeof(import_scratch), 0 };
    size_t streamed_len = 0;
    CHECK(ao_io_export_stream(&m, to_buffer, &streamed_xml, &streamed_len) == AO_OK &&
          streamed_len == legacy_len && streamed_xml.len == legacy_len &&
          memcmp(import_scratch, xml_scratch, legacy_len) == 0);
    ao_zip_entry_t content_part;
    const uint8_t *content_bytes = NULL; size_t content_len = 0;
    CHECK(ao_zip_reader_find(&r, AO_PACKAGE_CONTENT_PART,
                             sizeof(AO_PACKAGE_CONTENT_PART) - 1u,
                             &content_part) == AO_ZIP_OK);
    CHECK(ao_zip_entry_view(&r, &content_part, &content_bytes,
                            &content_len) == AO_ZIP_OK &&
          content_len == legacy_len &&
          memcmp(content_bytes, xml_scratch, legacy_len) == 0);
    CHECK(ao_package_import(&back, xml_scratch, legacy_len, (char *)import_scratch,
                            sizeof(import_scratch)) == AO_OK);
    CHECK(ao_model_text_len(&back) == ao_model_text_len(&m) &&
          memcmp(back_text, model_text, ao_model_text_len(&m)) == 0);

    /* This is the real package emitted by the same OF7 writer; Make's unit
     * gate opens it with Python's independent standard-library zipfile reader. */
    (void)mkdir("build", 0755);
    FILE *f = fopen("build/test_ao_zip_interop.aod", "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(out_a, 1, archive_size, f) == archive_size);
        CHECK(fclose(f) == 0);
    }
}

int main(void) {
    test_read_fixture();
    test_malformed();
    test_writer_determinism_and_many();
    test_package_and_legacy();
    printf("checks=%u fails=%u\n", checks, failures);
    return failures ? 1 : 0;
}
