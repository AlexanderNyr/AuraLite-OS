/* userspace/office/libaodoc/aozip.c — bounded classic ZIP reader/writer. */
#include "aozip.h"

#include <string.h>

#include "aodoc.h"
#include "aoinflate.h"

#define ZIP_SIG_LOCAL   0x04034B50u
#define ZIP_SIG_CENTRAL 0x02014B50u
#define ZIP_SIG_EOCD    0x06054B50u
#define ZIP_SIG_DD      0x08074B50u
#define ZIP_SIG64_LOC   0x07064B50u
#define ZIP_EXTRA64     0x0001u
#define ZIP_UTF8_FLAG   0x0800u
#define ZIP_DESC_FLAG   0x0008u
#define ZIP_DEFLATE_OPTS 0x0006u
#define ZIP_ALLOWED_FLAGS (ZIP_UTF8_FLAG | ZIP_DESC_FLAG | ZIP_DEFLATE_OPTS)

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint32_t ao_zip_crc32_update(uint32_t crc, const void *bytes, size_t length) {
    const uint8_t *p = (const uint8_t *)bytes;
    if (!p && length) return crc;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t ao_zip_crc32_finish(uint32_t state) {
    return state ^ 0xFFFFFFFFu;
}

uint32_t ao_zip_crc32(const void *bytes, size_t length) {
    if (!bytes && length) return 0;
    return ao_zip_crc32_finish(ao_zip_crc32_update(AO_ZIP_CRC32_INITIAL,
                                                   bytes, length));
}

const char *ao_zip_error_string(int error) {
    switch (error) {
        case AO_ZIP_OK:          return "success";
        case AO_ZIP_E_ARG:       return "invalid ZIP argument";
        case AO_ZIP_E_FORMAT:    return "malformed or unsupported ZIP structure";
        case AO_ZIP_E_LIMIT:     return "ZIP archive exceeds a configured limit";
        case AO_ZIP_E_METHOD:    return "ZIP compression method is not supported";
        case AO_ZIP_E_CRC:       return "ZIP CRC-32 check failed";
        case AO_ZIP_E_SIZE:      return "ZIP entry size does not match its data";
        case AO_ZIP_E_ZIP64:     return "ZIP64 is not supported (classic ZIP only)";
        case AO_ZIP_E_FULL:      return "output buffer is too small";
        case AO_ZIP_E_NOT_FOUND: return "ZIP entry not found";
        case AO_ZIP_E_DUP:       return "ZIP contains duplicate entry names";
        case AO_ZIP_E_IO:        return "ZIP output callback failed";
        case AO_ZIP_E_STATE:     return "ZIP writer is already finished or failed";
        default:                 return "unknown ZIP error";
    }
}

/* Reject unsafe names even though this reader never extracts paths to disk:
 * it keeps package-part lookup unambiguous and safe for later callers. */
static int name_valid(const char *name, size_t len, int utf8) {
    if (!name || len == 0 || len > AO_ZIP_MAX_NAME || name[0] == '/') return 0;
    if (utf8 && !ao_utf8_valid(name, len)) return 0;
    size_t segment = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && name[i] != '/' && name[i] != '\\' && name[i] != '\0')
            continue;
        if (i < len && name[i] != '/') return 0; /* backslash and NUL */
        size_t n = i - segment;
        if ((n == 1 && name[segment] == '.') ||
            (n == 2 && name[segment] == '.' && name[segment + 1] == '.'))
            return 0;
        segment = i + 1;
    }
    return 1;
}

/* Parse extra fields and reject ZIP64, malformed lengths, and trailing junk. */
static int extras_valid(const uint8_t *p, size_t len) {
    size_t at = 0;
    while (at < len) {
        if (len - at < 4) return AO_ZIP_E_FORMAT;
        uint16_t tag = rd16(p + at), n = rd16(p + at + 2);
        at += 4;
        if ((size_t)n > len - at) return AO_ZIP_E_FORMAT;
        if (tag == ZIP_EXTRA64) return AO_ZIP_E_ZIP64;
        at += n;
    }
    return at == len ? AO_ZIP_OK : AO_ZIP_E_FORMAT;
}

static int central_parse_at(const ao_zip_reader_t *r, size_t at,
                            ao_zip_entry_t *e, size_t *next) {
    if (!r || !e || !next || at > r->central_offset + r->central_size ||
        r->central_offset + r->central_size - at < 46u)
        return AO_ZIP_E_FORMAT;
    const uint8_t *p = r->bytes + at;
    if (rd32(p) != ZIP_SIG_CENTRAL) return AO_ZIP_E_FORMAT;
    uint16_t flags = rd16(p + 8), method = rd16(p + 10);
    uint32_t crc = rd32(p + 16), csize = rd32(p + 20), usize = rd32(p + 24);
    uint16_t nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
    uint16_t disk = rd16(p + 34);
    uint32_t local = rd32(p + 42);
    size_t record_len = 46u + (size_t)nlen + (size_t)elen + (size_t)clen;
    size_t cd_end = r->central_offset + r->central_size;
    if (record_len > cd_end - at) return AO_ZIP_E_FORMAT;
    if (!nlen || nlen > AO_ZIP_MAX_NAME) return AO_ZIP_E_LIMIT;
    if (disk != 0) return AO_ZIP_E_FORMAT; /* multi-disk archives */
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu ||
        local == 0xFFFFFFFFu) return AO_ZIP_E_ZIP64;
    if ((flags & (uint16_t)~ZIP_ALLOWED_FLAGS) != 0 ||
        ((flags & ZIP_DEFLATE_OPTS) && method != AO_ZIP_DEFLATE))
        return AO_ZIP_E_FORMAT;
    if (method != AO_ZIP_STORE && method != AO_ZIP_DEFLATE)
        return AO_ZIP_E_METHOD;
    if (method == AO_ZIP_STORE && csize != usize) return AO_ZIP_E_SIZE;
    if (!name_valid((const char *)(p + 46), nlen, (flags & ZIP_UTF8_FLAG) != 0))
        return AO_ZIP_E_FORMAT;
    int rc = extras_valid(p + 46u + nlen, elen);
    if (rc != AO_ZIP_OK) return rc;

    e->name = (const char *)(p + 46);
    e->name_len = nlen;
    e->flags = flags; e->method = method;
    e->crc32 = crc; e->compressed_size = csize;
    e->uncompressed_size = usize; e->local_offset = local;
    e->data_offset = 0;
    *next = at + record_len;
    return AO_ZIP_OK;
}

static int descriptor_matches(const uint8_t *p, size_t available,
                              const ao_zip_entry_t *e) {
    if (available < 12u) return AO_ZIP_E_FORMAT;
    if (available >= 16u && rd32(p) == ZIP_SIG_DD &&
        rd32(p + 4) == e->crc32 &&
        rd32(p + 8) == e->compressed_size &&
        rd32(p + 12) == e->uncompressed_size)
        return AO_ZIP_OK;
    if (rd32(p) == e->crc32 &&
        rd32(p + 4) == e->compressed_size &&
        rd32(p + 8) == e->uncompressed_size)
        return AO_ZIP_OK;
    return AO_ZIP_E_SIZE;
}

static int local_validate(const ao_zip_reader_t *r, ao_zip_entry_t *e) {
    if (e->local_offset > r->central_offset ||
        r->central_offset - e->local_offset < 30u)
        return AO_ZIP_E_FORMAT;
    const uint8_t *p = r->bytes + e->local_offset;
    if (rd32(p) != ZIP_SIG_LOCAL) return AO_ZIP_E_FORMAT;
    uint16_t flags = rd16(p + 6), method = rd16(p + 8);
    uint32_t crc = rd32(p + 14), csize = rd32(p + 18), usize = rd32(p + 22);
    uint16_t nlen = rd16(p + 26), elen = rd16(p + 28);
    if (flags != e->flags || method != e->method || nlen != e->name_len)
        return AO_ZIP_E_FORMAT;
    if (e->local_offset + 30u > r->central_offset ||
        (size_t)nlen + (size_t)elen > r->central_offset - e->local_offset - 30u)
        return AO_ZIP_E_FORMAT;
    if (memcmp(p + 30, e->name, nlen) != 0) return AO_ZIP_E_FORMAT;
    int rc = extras_valid(p + 30u + nlen, elen);
    if (rc != AO_ZIP_OK) return rc;
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu) return AO_ZIP_E_ZIP64;
    if (flags & ZIP_DESC_FLAG) {
        if ((crc && crc != e->crc32) || (csize && csize != e->compressed_size) ||
            (usize && usize != e->uncompressed_size)) return AO_ZIP_E_SIZE;
    } else if (crc != e->crc32 || csize != e->compressed_size ||
               usize != e->uncompressed_size) {
        return AO_ZIP_E_SIZE;
    }
    size_t data = (size_t)e->local_offset + 30u + nlen + elen;
    if (data > r->central_offset ||
        e->compressed_size > r->central_offset - data)
        return AO_ZIP_E_FORMAT;
    size_t after = data + e->compressed_size;
    if (flags & ZIP_DESC_FLAG) {
        rc = descriptor_matches(r->bytes + after, r->central_offset - after, e);
        if (rc != AO_ZIP_OK) return rc;
    }
    e->data_offset = data;
    return AO_ZIP_OK;
}

int ao_zip_reader_init(ao_zip_reader_t *r, const void *bytes, size_t length) {
    if (!r || !bytes) return AO_ZIP_E_ARG;
    memset(r, 0, sizeof(*r));
    if (length > AO_ZIP_MAX_ARCHIVE_BYTES) return AO_ZIP_E_LIMIT;
    if (length < 22u) return AO_ZIP_E_FORMAT;
    const uint8_t *src = (const uint8_t *)bytes;
    size_t min = length > (22u + 65535u) ? length - (22u + 65535u) : 0;
    size_t pos = length - 22u;
    size_t eocd = (size_t)-1;
    for (;;) {
        if (rd32(src + pos) == ZIP_SIG_EOCD) {
            uint16_t comment = rd16(src + pos + 20);
            if ((size_t)comment == length - pos - 22u) { eocd = pos; break; }
        }
        if (pos == min) break;
        pos--;
    }
    if (eocd == (size_t)-1) {
        if (length >= 20u && rd32(src + length - 20u) == ZIP_SIG64_LOC)
            return AO_ZIP_E_ZIP64;
        return AO_ZIP_E_FORMAT;
    }
    if (eocd >= 20u && rd32(src + eocd - 20u) == ZIP_SIG64_LOC)
        return AO_ZIP_E_ZIP64;
    const uint8_t *end = src + eocd;
    uint16_t disk = rd16(end + 4), cd_disk = rd16(end + 6);
    uint16_t disk_count = rd16(end + 8), count = rd16(end + 10);
    uint32_t cd_size = rd32(end + 12), cd_offset = rd32(end + 16);
    if (disk_count == 0xFFFFu || count == 0xFFFFu ||
        cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu)
        return AO_ZIP_E_ZIP64;
    if (disk || cd_disk || disk_count != count) return AO_ZIP_E_FORMAT;
    if (count > AO_ZIP_MAX_ENTRIES) return AO_ZIP_E_LIMIT;
    if ((size_t)cd_offset > eocd || (size_t)cd_size != eocd - (size_t)cd_offset)
        return AO_ZIP_E_FORMAT;

    r->bytes = src; r->length = length;
    r->central_offset = cd_offset; r->central_size = cd_size;
    r->eocd_offset = eocd; r->entries = count;
    size_t at = r->central_offset;
    size_t cd_end = r->central_offset + r->central_size;
    for (size_t i = 0; i < count; i++) {
        ao_zip_entry_t e;
        size_t next = 0;
        int rc = central_parse_at(r, at, &e, &next);
        if (rc != AO_ZIP_OK) { memset(r, 0, sizeof(*r)); return rc; }
        rc = local_validate(r, &e);
        if (rc != AO_ZIP_OK) { memset(r, 0, sizeof(*r)); return rc; }
        at = next;
    }
    if (at != cd_end) { memset(r, 0, sizeof(*r)); return AO_ZIP_E_FORMAT; }
    return AO_ZIP_OK;
}

size_t ao_zip_reader_count(const ao_zip_reader_t *r) {
    return r && r->bytes ? r->entries : 0;
}

int ao_zip_reader_entry_at(const ao_zip_reader_t *r, size_t index,
                           ao_zip_entry_t *entry) {
    if (!r || !r->bytes || !entry) return AO_ZIP_E_ARG;
    if (index >= r->entries) return AO_ZIP_E_NOT_FOUND;
    size_t at = r->central_offset;
    for (size_t i = 0; i <= index; i++) {
        size_t next = 0;
        int rc = central_parse_at(r, at, entry, &next);
        if (rc != AO_ZIP_OK) return rc;
        if (i == index) return local_validate(r, entry);
        at = next;
    }
    return AO_ZIP_E_NOT_FOUND;
}

int ao_zip_reader_find(const ao_zip_reader_t *r, const char *name,
                       size_t name_len, ao_zip_entry_t *entry) {
    if (!r || !r->bytes || !name || !entry) return AO_ZIP_E_ARG;
    int found = 0;
    ao_zip_entry_t candidate;
    for (size_t i = 0; i < r->entries; i++) {
        ao_zip_entry_t e;
        int rc = ao_zip_reader_entry_at(r, i, &e);
        if (rc != AO_ZIP_OK) return rc;
        if (e.name_len == name_len && memcmp(e.name, name, name_len) == 0) {
            if (found) return AO_ZIP_E_DUP;
            candidate = e; found = 1;
        }
    }
    if (!found) return AO_ZIP_E_NOT_FOUND;
    *entry = candidate;
    return AO_ZIP_OK;
}

static int entry_range_valid(const ao_zip_reader_t *r,
                             const ao_zip_entry_t *e) {
    if (!r || !r->bytes || !e || e->data_offset > r->central_offset ||
        e->compressed_size > r->central_offset - e->data_offset)
        return 0;
    return 1;
}

int ao_zip_entry_view(const ao_zip_reader_t *r, const ao_zip_entry_t *e,
                      const uint8_t **bytes, size_t *length) {
    if (!r || !e || !bytes || !length || !entry_range_valid(r, e))
        return AO_ZIP_E_ARG;
    if (e->method != AO_ZIP_STORE) return AO_ZIP_E_METHOD;
    if (e->compressed_size != e->uncompressed_size) return AO_ZIP_E_SIZE;
    const uint8_t *p = r->bytes + e->data_offset;
    if (ao_zip_crc32(p, e->uncompressed_size) != e->crc32) return AO_ZIP_E_CRC;
    *bytes = p; *length = e->uncompressed_size;
    return AO_ZIP_OK;
}

int ao_zip_extract(const ao_zip_reader_t *r, const ao_zip_entry_t *e,
                   void *out, size_t out_cap, size_t *out_len) {
    if (!r || !e || !out_len || (!out && e->uncompressed_size) ||
        !entry_range_valid(r, e)) return AO_ZIP_E_ARG;
    *out_len = 0;
    if (out_cap < e->uncompressed_size) return AO_ZIP_E_FULL;
    uint8_t *dst = (uint8_t *)out;
    const uint8_t *src = r->bytes + e->data_offset;
    if (e->method == AO_ZIP_STORE) {
        if (e->compressed_size != e->uncompressed_size) return AO_ZIP_E_SIZE;
        if (e->uncompressed_size) memcpy(dst, src, e->uncompressed_size);
    } else if (e->method == AO_ZIP_DEFLATE) {
        size_t got = 0, consumed = 0;
        int irc = ao_inflate_raw(src, e->compressed_size, dst,
                                 e->uncompressed_size, &got, &consumed);
        if (irc == AO_INFLATE_E_FULL) return AO_ZIP_E_SIZE;
        if (irc != AO_INFLATE_OK) return AO_ZIP_E_FORMAT;
        if (consumed != e->compressed_size) return AO_ZIP_E_FORMAT;
        if (got != e->uncompressed_size) return AO_ZIP_E_SIZE;
    } else {
        return AO_ZIP_E_METHOD;
    }
    if (ao_zip_crc32(dst, e->uncompressed_size) != e->crc32)
        return AO_ZIP_E_CRC;
    *out_len = e->uncompressed_size;
    return AO_ZIP_OK;
}

static int writer_emit(ao_zip_writer_t *w, const void *bytes, size_t length) {
    if (w->error) return w->error;
    const size_t max_archive = (size_t)AO_ZIP_MAX_ARCHIVE_BYTES;
    if (length > max_archive || w->bytes_out > max_archive - length ||
        length > 0xFFFFFFFFu || w->bytes_out > 0xFFFFFFFFu - length) {
        w->error = AO_ZIP_E_LIMIT; return w->error;
    }
    if (length && w->write(w->context, bytes, length) != 0) {
        w->error = AO_ZIP_E_IO; return w->error;
    }
    w->bytes_out += length;
    return AO_ZIP_OK;
}

int ao_zip_writer_init(ao_zip_writer_t *w, ao_zip_write_fn write,
                       void *context) {
    if (!w || !write) return AO_ZIP_E_ARG;
    memset(w, 0, sizeof(*w));
    w->write = write; w->context = context;
    return AO_ZIP_OK;
}

int ao_zip_writer_begin_entry(ao_zip_writer_t *w, const char *name,
                              size_t name_len, size_t length, uint32_t crc32) {
    if (!w || !w->write || !name) return AO_ZIP_E_ARG;
    if (w->error) return w->error;
    if (w->finished || w->entry_open) return AO_ZIP_E_STATE;
    if (w->entries >= AO_ZIP_MAX_WRITE_ENTRIES) return AO_ZIP_E_LIMIT;
    if (!name_valid(name, name_len, 1)) return AO_ZIP_E_FORMAT;
    if (length > 0xFFFFFFFFu || w->bytes_out > 0xFFFFFFFFu)
        return AO_ZIP_E_LIMIT;
    for (uint16_t i = 0; i < w->entries; i++) {
        const ao_zip_written_entry_t *old = &w->central[i];
        if (old->name_len == name_len && memcmp(old->name, name, name_len) == 0)
            return AO_ZIP_E_DUP;
    }
    const size_t max_archive = (size_t)AO_ZIP_MAX_ARCHIVE_BYTES;
    size_t fixed = sizeof(uint8_t) * 30u + name_len;
    if (fixed > max_archive || w->bytes_out > max_archive - fixed ||
        length > max_archive - fixed - w->bytes_out)
        return AO_ZIP_E_LIMIT;

    memset(&w->current, 0, sizeof(w->current));
    memcpy(w->current.name, name, name_len);
    w->current.name[name_len] = '\0';
    w->current.name_len = (uint16_t)name_len;
    w->current.crc32 = crc32;
    w->current.size = (uint32_t)length;
    w->current.local_offset = (uint32_t)w->bytes_out;
    w->entry_bytes = 0;
    w->entry_crc_state = AO_ZIP_CRC32_INITIAL;
    w->entry_open = 1;

    uint8_t header[30];
    memset(header, 0, sizeof(header));
    wr32(header, ZIP_SIG_LOCAL);
    wr16(header + 4, 20);                  /* version needed: ZIP 2.0 */
    wr16(header + 6, ZIP_UTF8_FLAG);
    wr16(header + 8, AO_ZIP_STORE);
    wr16(header + 12, 0x0021u);            /* 1980-01-01, fixed time 00:00 */
    wr32(header + 14, crc32);
    wr32(header + 18, (uint32_t)length);
    wr32(header + 22, (uint32_t)length);
    wr16(header + 26, w->current.name_len);

    int rc = writer_emit(w, header, sizeof(header));
    if (rc == AO_ZIP_OK) rc = writer_emit(w, name, name_len);
    return rc;
}

int ao_zip_writer_write_entry(ao_zip_writer_t *w, const void *bytes,
                              size_t length) {
    if (!w || !w->write || (!bytes && length)) return AO_ZIP_E_ARG;
    if (w->error) return w->error;
    if (w->finished || !w->entry_open) return AO_ZIP_E_STATE;
    if (length > (size_t)w->current.size - w->entry_bytes) {
        w->error = AO_ZIP_E_SIZE; return w->error;
    }
    int rc = writer_emit(w, bytes, length);
    if (rc != AO_ZIP_OK) return rc;
    w->entry_crc_state = ao_zip_crc32_update(w->entry_crc_state, bytes, length);
    w->entry_bytes += length;
    return AO_ZIP_OK;
}

int ao_zip_writer_end_entry(ao_zip_writer_t *w) {
    if (!w || !w->write) return AO_ZIP_E_ARG;
    if (w->error) return w->error;
    if (w->finished || !w->entry_open) return AO_ZIP_E_STATE;
    if (w->entry_bytes != w->current.size) {
        w->error = AO_ZIP_E_SIZE; return w->error;
    }
    if (ao_zip_crc32_finish(w->entry_crc_state) != w->current.crc32) {
        w->error = AO_ZIP_E_CRC; return w->error;
    }
    w->central[w->entries++] = w->current;
    memset(&w->current, 0, sizeof(w->current));
    w->entry_open = 0;
    return AO_ZIP_OK;
}

int ao_zip_writer_add(ao_zip_writer_t *w, const char *name, size_t name_len,
                      const void *bytes, size_t length) {
    if (!w || !w->write || (!bytes && length)) return AO_ZIP_E_ARG;
    int rc = ao_zip_writer_begin_entry(w, name, name_len, length,
                                       ao_zip_crc32(bytes, length));
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_write_entry(w, bytes, length);
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_end_entry(w);
    return rc;
}

int ao_zip_writer_finish(ao_zip_writer_t *w, size_t *archive_size) {
    if (!w || !w->write) return AO_ZIP_E_ARG;
    if (w->error) return w->error;
    if (w->finished || w->entry_open) return AO_ZIP_E_STATE;
    if (w->bytes_out > 0xFFFFFFFFu) return AO_ZIP_E_LIMIT;
    uint32_t cd_offset = (uint32_t)w->bytes_out;

    for (uint16_t i = 0; i < w->entries; i++) {
        const ao_zip_written_entry_t *e = &w->central[i];
        uint8_t header[46];
        memset(header, 0, sizeof(header));
        wr32(header, ZIP_SIG_CENTRAL);
        wr16(header + 4, 20);                /* made by DOS, ZIP 2.0 */
        wr16(header + 6, 20);
        wr16(header + 8, ZIP_UTF8_FLAG);
        wr16(header + 10, AO_ZIP_STORE);
        wr16(header + 14, 0x0021u);
        wr32(header + 16, e->crc32);
        wr32(header + 20, e->size);
        wr32(header + 24, e->size);
        wr16(header + 28, e->name_len);
        wr32(header + 42, e->local_offset);
        int rc = writer_emit(w, header, sizeof(header));
        if (rc == AO_ZIP_OK) rc = writer_emit(w, e->name, e->name_len);
        if (rc != AO_ZIP_OK) return rc;
    }
    if (w->bytes_out < cd_offset || w->bytes_out - cd_offset > 0xFFFFFFFFu)
        return AO_ZIP_E_LIMIT;
    uint32_t cd_size = (uint32_t)(w->bytes_out - cd_offset);
    if (w->bytes_out > 0xFFFFFFFFu - 22u) return AO_ZIP_E_LIMIT;
    uint8_t end[22];
    memset(end, 0, sizeof(end));
    wr32(end, ZIP_SIG_EOCD);
    wr16(end + 8, w->entries);
    wr16(end + 10, w->entries);
    wr32(end + 12, cd_size);
    wr32(end + 16, cd_offset);
    int rc = writer_emit(w, end, sizeof(end));
    if (rc != AO_ZIP_OK) return rc;
    w->finished = 1;
    if (archive_size) *archive_size = w->bytes_out;
    return AO_ZIP_OK;
}
