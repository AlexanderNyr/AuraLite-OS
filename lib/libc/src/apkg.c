/* apkg.c — the .apkg package format parser (SDK_PLAN phase S4).
 *
 * Shared by `apm` (which reads packages) and tools/mkapkg (which writes them),
 * so the writer and the reader cannot disagree about the format.
 *
 * EVERY BYTE THIS SEES IS ATTACKER-CONTROLLED. The file came from wherever
 * the user got it. The parser therefore:
 *
 *   - never trusts a declared length against the actual one;
 *   - bounds every scan, so a file with no newline in it terminates;
 *   - copies into fixed buffers with explicit truncation checks;
 *   - reports distinct causes, because "not a package" and "corrupt package"
 *     need different things from the person holding it.
 *
 * It is deliberately free of allocation and of any dependency beyond the
 * freestanding subset, so the host tool and the in-OS installer compile the
 * same source.
 */

#include "apkg.h"

/* ---- CRC-32 (IEEE 802.3) ---------------------------------------------- */

/* Computed on the fly rather than stored: a 1 KiB table in every program that
 * links this is a poor trade for a checksum run once per install. */
uint32_t apkg_crc32(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

const char *apkg_strerror(int result) {
    switch (result) {
        case APKG_OK:            return "ok";
        case APKG_ERR_MAGIC:     return "not an AuraLite package";
        case APKG_ERR_TRUNCATED: return "package header is truncated";
        case APKG_ERR_FIELD:     return "malformed header field";
        case APKG_ERR_MISSING:   return "required header field missing";
        case APKG_ERR_SIZE:      return "declared payload size is wrong";
        case APKG_ERR_CRC:       return "payload checksum mismatch";
        default:                 return "unknown error";
    }
}

/* ---- small helpers, all bounded ---------------------------------------- */

static int str_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

/* Copy at most cap-1 bytes; returns 0 if the source did not fit.  A silently
 * truncated name is a different package from the one on disk. */
static int copy_field(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) return 0;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    return 1;
}

/* Parse a decimal or 0x-prefixed hex integer.  Returns 0 on any malformed
 * input, including an empty field or trailing rubbish. */
static int parse_u64(const char *s, size_t len, uint64_t *out) {
    if (len == 0) return 0;
    uint64_t v = 0;
    size_t i = 0;
    int base = 10;

    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
        if (i >= len) return 0;
    }

    for (; i < len; i++) {
        char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9')                  d = (uint64_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else return 0;                              /* trailing rubbish */

        if (d >= (uint64_t)base) return 0;
        /* Overflow check before it happens: a declared size of 2^64-1 must
         * not wrap into something plausible. */
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / (uint64_t)base) return 0;
        v = v * (uint64_t)base + d;
    }
    *out = v;
    return 1;
}

/* ---- the parser --------------------------------------------------------- */

int apkg_parse(const void *buf, size_t len, struct apkg_header *out) {
    const char *p = (const char *)buf;

    if (!buf || !out) return APKG_ERR_MAGIC;
    if (len < APKG_MAGIC_LEN + 1) return APKG_ERR_MAGIC;
    if (!str_eq_n(p, APKG_MAGIC, APKG_MAGIC_LEN)) return APKG_ERR_MAGIC;
    if (p[APKG_MAGIC_LEN] != '\n') return APKG_ERR_MAGIC;

    /* Start clean: a field left over from a previous parse would be worse
     * than an empty one. */
    for (size_t i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;

    int have_name = 0, have_version = 0, have_size = 0, have_crc = 0;
    size_t pos = APKG_MAGIC_LEN + 1;

    for (;;) {
        if (pos >= len) return APKG_ERR_TRUNCATED;
        if (pos > APKG_HEADER_MAX) return APKG_ERR_TRUNCATED;

        /* A blank line ends the header. */
        if (p[pos] == '\n') { pos++; break; }

        /* Find the end of this line, bounded by both the file and the header
         * cap, so a file with no newline at all cannot run away. */
        size_t eol = pos;
        while (eol < len && eol <= APKG_HEADER_MAX && p[eol] != '\n') eol++;
        if (eol >= len || p[eol] != '\n') return APKG_ERR_TRUNCATED;

        /* Split at ": ". */
        size_t colon = pos;
        while (colon < eol && p[colon] != ':') colon++;
        if (colon >= eol) return APKG_ERR_FIELD;      /* no separator */
        size_t vstart = colon + 1;
        if (vstart < eol && p[vstart] == ' ') vstart++;
        if (vstart > eol) return APKG_ERR_FIELD;

        size_t klen = colon - pos;
        size_t vlen = eol - vstart;

        if (klen == 4 && str_eq_n(p + pos, "name", 4)) {
            if (!copy_field(out->name, APKG_NAME_MAX, p + vstart, vlen))
                return APKG_ERR_FIELD;
            if (vlen == 0) return APKG_ERR_FIELD;
            have_name = 1;
        } else if (klen == 7 && str_eq_n(p + pos, "version", 7)) {
            if (!copy_field(out->version, APKG_VERSION_MAX, p + vstart, vlen))
                return APKG_ERR_FIELD;
            have_version = 1;
        } else if (klen == 11 && str_eq_n(p + pos, "description", 11)) {
            if (!copy_field(out->desc, APKG_DESC_MAX, p + vstart, vlen))
                return APKG_ERR_FIELD;
        } else if (klen == 4 && str_eq_n(p + pos, "size", 4)) {
            uint64_t v;
            if (!parse_u64(p + vstart, vlen, &v)) return APKG_ERR_FIELD;
            out->size = v;
            have_size = 1;
        } else if (klen == 5 && str_eq_n(p + pos, "crc32", 5)) {
            uint64_t v;
            if (!parse_u64(p + vstart, vlen, &v)) return APKG_ERR_FIELD;
            if (v > 0xFFFFFFFFull) return APKG_ERR_FIELD;
            out->crc32 = (uint32_t)v;
            have_crc = 1;
        } else {
            /* An unknown field is rejected rather than ignored.  A format
             * that silently accepts what it does not understand cannot be
             * extended later without ambiguity about what old readers did. */
            return APKG_ERR_FIELD;
        }

        pos = eol + 1;
    }

    if (!have_name || !have_version || !have_size || !have_crc)
        return APKG_ERR_MISSING;

    if (out->size > APKG_PAYLOAD_MAX) return APKG_ERR_SIZE;

    out->payload_off = pos;

    /* The declared size must agree with what is actually here.  This is the
     * check that catches a truncated transfer, which is the failure this
     * format exists to detect. */
    if (pos > len) return APKG_ERR_TRUNCATED;
    if (out->size > (uint64_t)(len - pos)) return APKG_ERR_SIZE;

    return APKG_OK;
}

int apkg_verify(const void *buf, size_t len, const struct apkg_header *h) {
    if (!buf || !h) return APKG_ERR_SIZE;
    if (h->payload_off > len) return APKG_ERR_SIZE;
    if (h->size > (uint64_t)(len - h->payload_off)) return APKG_ERR_SIZE;

    const unsigned char *payload = (const unsigned char *)buf + h->payload_off;
    if (apkg_crc32(payload, (size_t)h->size) != h->crc32) return APKG_ERR_CRC;
    return APKG_OK;
}
