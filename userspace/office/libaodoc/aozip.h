/* userspace/office/libaodoc/aozip.h — bounded classic ZIP reader/writer. */
#ifndef AURALITE_OFFICE_AOZIP_H
#define AURALITE_OFFICE_AOZIP_H

#include <stddef.h>
#include <stdint.h>

#define AO_ZIP_MAX_ENTRIES        256u
#define AO_ZIP_MAX_WRITE_ENTRIES   64u
#define AO_ZIP_MAX_NAME           255u
#define AO_ZIP_MAX_ARCHIVE_BYTES  (64u * 1024u * 1024u)

#define AO_ZIP_OK                0
#define AO_ZIP_E_ARG            -1
#define AO_ZIP_E_FORMAT         -2
#define AO_ZIP_E_LIMIT          -3
#define AO_ZIP_E_METHOD         -4
#define AO_ZIP_E_CRC            -5
#define AO_ZIP_E_SIZE           -6
#define AO_ZIP_E_ZIP64           -7
#define AO_ZIP_E_FULL           -8
#define AO_ZIP_E_NOT_FOUND      -9
#define AO_ZIP_E_DUP            -10
#define AO_ZIP_E_IO             -11
#define AO_ZIP_E_STATE          -12

#define AO_ZIP_STORE              0u
#define AO_ZIP_DEFLATE            8u

typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t central_offset;
    size_t central_size;
    size_t eocd_offset;
    uint16_t entries;
    uint16_t _pad;
} ao_zip_reader_t;

typedef struct {
    const char *name;          /* slice in the input archive */
    size_t name_len;
    uint16_t flags;
    uint16_t method;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
    size_t data_offset;        /* verified local-header-derived payload offset */
} ao_zip_entry_t;

int ao_zip_reader_init(ao_zip_reader_t *r, const void *bytes, size_t length);
size_t ao_zip_reader_count(const ao_zip_reader_t *r);
int ao_zip_reader_entry_at(const ao_zip_reader_t *r, size_t index,
                           ao_zip_entry_t *entry);
int ao_zip_reader_find(const ao_zip_reader_t *r, const char *name,
                       size_t name_len, ao_zip_entry_t *entry);

/* A stored entry can be returned as a zero-copy view (CRC-checked). */
int ao_zip_entry_view(const ao_zip_reader_t *r, const ao_zip_entry_t *entry,
                      const uint8_t **bytes, size_t *length);
/* Read stored or raw-DEFLATE entry bytes; verifies exact size and CRC. */
int ao_zip_extract(const ao_zip_reader_t *r, const ao_zip_entry_t *entry,
                   void *out, size_t out_cap, size_t *out_len);

#define AO_ZIP_CRC32_INITIAL 0xFFFFFFFFu
uint32_t ao_zip_crc32_update(uint32_t state, const void *bytes, size_t length);
uint32_t ao_zip_crc32_finish(uint32_t state);
uint32_t ao_zip_crc32(const void *bytes, size_t length);
const char *ao_zip_error_string(int error);

/* Streaming writer. Entries are emitted immediately; only the bounded central
 * directory is retained until finish. Output ordering is caller-defined and
 * timestamps are pinned to 1980-01-01 00:00:00 for reproducible saves. */
typedef int (*ao_zip_write_fn)(void *context, const void *bytes, size_t length);

typedef struct {
    char name[AO_ZIP_MAX_NAME + 1u];
    uint16_t name_len;
    uint16_t _pad;
    uint32_t crc32;
    uint32_t size;
    uint32_t local_offset;
} ao_zip_written_entry_t;

typedef struct {
    ao_zip_write_fn write;
    void *context;
    size_t bytes_out;
    size_t entry_bytes;
    uint32_t entry_crc_state;
    uint16_t entries;
    uint8_t finished;
    uint8_t entry_open;
    int error;
    ao_zip_written_entry_t current;
    ao_zip_written_entry_t central[AO_ZIP_MAX_WRITE_ENTRIES];
} ao_zip_writer_t;

int ao_zip_writer_init(ao_zip_writer_t *w, ao_zip_write_fn write,
                       void *context);
/* Begin a stored entry when size and CRC are known; write_entry can then feed
 * it in chunks without staging its full payload in memory. */
int ao_zip_writer_begin_entry(ao_zip_writer_t *w, const char *name,
                              size_t name_len, size_t length, uint32_t crc32);
int ao_zip_writer_write_entry(ao_zip_writer_t *w, const void *bytes,
                              size_t length);
int ao_zip_writer_end_entry(ao_zip_writer_t *w);
int ao_zip_writer_add(ao_zip_writer_t *w, const char *name, size_t name_len,
                      const void *bytes, size_t length);
int ao_zip_writer_finish(ao_zip_writer_t *w, size_t *archive_size);

#endif /* AURALITE_OFFICE_AOZIP_H */
