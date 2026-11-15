/* userspace/office/libaodoc/aopackage.c — native .aod package and migration. */
#include "aopackage.h"

#include <string.h>

#include "aoxml.h"

static const char styles_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<styles version=\"1\"/>\n";

static int xml_error_to_io(int rc) {
    if (rc == AO_XML_E_UTF8) return AO_E_UTF8;
    if (rc == AO_XML_E_FULL || rc == AO_XML_E_LIMIT) return AO_E_FULL;
    return AO_E_FORMAT;
}

typedef struct {
    size_t length;
    uint32_t crc_state;
} xml_measure_t;

static int measure_xml_write(void *context, const void *bytes, size_t length) {
    xml_measure_t *measure = (xml_measure_t *)context;
    size_t max = (size_t)AO_ZIP_MAX_ARCHIVE_BYTES;
    if (!measure || (!bytes && length) || length > max - measure->length)
        return AO_ZIP_E_LIMIT;
    measure->crc_state = ao_zip_crc32_update(measure->crc_state, bytes, length);
    measure->length += length;
    return 0;
}

typedef struct {
    ao_zip_writer_t *writer;
} zip_stream_t;

static int stream_xml_to_zip(void *context, const void *bytes, size_t length) {
    zip_stream_t *stream = (zip_stream_t *)context;
    if (!stream || !stream->writer) return AO_ZIP_E_ARG;
    return ao_zip_writer_write_entry(stream->writer, bytes, length);
}

int ao_package_export(const ao_model_t *model,
                      ao_zip_write_fn write, void *context,
                      size_t *archive_size, size_t *content_xml_size) {
    if (!model || !write) return AO_E_ARG;

    xml_measure_t measure = { 0, AO_ZIP_CRC32_INITIAL };
    size_t xml_len = 0;
    int rc = ao_io_export_stream(model, measure_xml_write, &measure, &xml_len);
    if (rc != AO_OK) return rc;
    if (xml_len != measure.length) return AO_E_FORMAT;
    uint32_t xml_crc = ao_zip_crc32_finish(measure.crc_state);

    ao_zip_writer_t writer;
    rc = ao_zip_writer_init(&writer, write, context);
    if (rc != AO_ZIP_OK) return rc;
    static const char mimetype[] = AO_PACKAGE_MIMETYPE;
    rc = ao_zip_writer_add(&writer, AO_PACKAGE_MIMETYPE_PART,
                           sizeof(AO_PACKAGE_MIMETYPE_PART) - 1u,
                           mimetype, sizeof(mimetype) - 1u);
    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_begin_entry(&writer, AO_PACKAGE_CONTENT_PART,
                                       sizeof(AO_PACKAGE_CONTENT_PART) - 1u,
                                       xml_len, xml_crc);
    size_t streamed_len = 0;
    if (rc == AO_ZIP_OK) {
        zip_stream_t stream = { &writer };
        rc = ao_io_export_stream(model, stream_xml_to_zip, &stream, &streamed_len);
        if (rc != AO_OK && writer.error) rc = writer.error;
    }
    if (rc == AO_OK && streamed_len != xml_len) rc = AO_ZIP_E_SIZE;
    if (rc == AO_OK) rc = ao_zip_writer_end_entry(&writer);
    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_add(&writer, AO_PACKAGE_STYLES_PART,
                               sizeof(AO_PACKAGE_STYLES_PART) - 1u,
                               styles_xml, sizeof(styles_xml) - 1u);
    size_t total = 0;
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_finish(&writer, &total);
    if (rc != AO_ZIP_OK) return rc;
    if (archive_size) *archive_size = total;
    if (content_xml_size) *content_xml_size = xml_len;
    return AO_OK;
}

static int is_zip_magic(const uint8_t *p, size_t n) {
    return n >= 2u && p[0] == 'P' && p[1] == 'K';
}

static int part_named(const ao_zip_entry_t *e, const char *s) {
    size_t n = strlen(s);
    return e->name_len == n && memcmp(e->name, s, n) == 0;
}

static int read_part(const ao_zip_reader_t *r, const ao_zip_entry_t *e,
                     const uint8_t **data, size_t *len,
                     char *scratch, size_t scratch_cap) {
    if (e->method == AO_ZIP_STORE)
        return ao_zip_entry_view(r, e, data, len);
    if (!scratch || scratch_cap < e->uncompressed_size) return AO_ZIP_E_FULL;
    size_t got = 0;
    int rc = ao_zip_extract(r, e, scratch, scratch_cap, &got);
    if (rc != AO_ZIP_OK) return rc;
    *data = (const uint8_t *)scratch; *len = got;
    return AO_ZIP_OK;
}

int ao_package_import(ao_model_t *model, const void *bytes, size_t length,
                      char *scratch, size_t scratch_cap) {
    if (!model || !bytes) return AO_E_ARG;
    ao_model_clear(model);
    if (length == 0) return AO_E_FORMAT;
    const uint8_t *src = (const uint8_t *)bytes;
    if (!is_zip_magic(src, length)) return ao_io_import(model, (const char *)bytes, length);

    ao_zip_reader_t reader;
    int rc = ao_zip_reader_init(&reader, bytes, length);
    if (rc != AO_ZIP_OK) { ao_model_clear(model); return rc; }
    if (ao_zip_reader_count(&reader) != 3u) {
        ao_model_clear(model); return AO_E_FORMAT;
    }

    ao_zip_entry_t first, content, styles;
    rc = ao_zip_reader_entry_at(&reader, 0, &first);
    if (rc == AO_ZIP_OK) rc = ao_zip_reader_entry_at(&reader, 1, &content);
    if (rc == AO_ZIP_OK) rc = ao_zip_reader_entry_at(&reader, 2, &styles);
    if (rc != AO_ZIP_OK) { ao_model_clear(model); return rc; }
    /* OF7's native package has an exact, reproducible part list. The first
     * local file record starts at byte zero and is the uncompressed mimetype;
     * the two XML parts follow it in the same order as the central directory. */
    if (!part_named(&first, AO_PACKAGE_MIMETYPE_PART) ||
        !part_named(&content, AO_PACKAGE_CONTENT_PART) ||
        !part_named(&styles, AO_PACKAGE_STYLES_PART) ||
        first.method != AO_ZIP_STORE || first.local_offset != 0u ||
        content.local_offset <= first.local_offset ||
        styles.local_offset <= content.local_offset) {
        ao_model_clear(model); return AO_E_FORMAT;
    }
    const uint8_t *part = NULL;
    size_t part_len = 0;
    rc = ao_zip_entry_view(&reader, &first, &part, &part_len);
    if (rc != AO_ZIP_OK) { ao_model_clear(model); return rc; }
    static const char mimetype[] = AO_PACKAGE_MIMETYPE;
    if (part_len != sizeof(mimetype) - 1u ||
        memcmp(part, mimetype, sizeof(mimetype) - 1u) != 0) {
        ao_model_clear(model); return AO_E_FORMAT;
    }

    /* Validate styles even though OF7's native model has no named-style table
     * yet. This prevents a malformed required package part being ignored. */
    rc = read_part(&reader, &styles, &part, &part_len, scratch, scratch_cap);
    if (rc != AO_ZIP_OK) { ao_model_clear(model); return rc; }
    int xrc = ao_xml_validate_native((const char *)part, part_len);
    if (xrc != AO_XML_OK) {
        ao_model_clear(model); return xml_error_to_io(xrc);
    }

    rc = read_part(&reader, &content, &part, &part_len, scratch, scratch_cap);
    if (rc != AO_ZIP_OK) { ao_model_clear(model); return rc; }
    rc = ao_io_import(model, (const char *)part, part_len);
    if (rc != AO_OK) ao_model_clear(model);
    return rc;
}
