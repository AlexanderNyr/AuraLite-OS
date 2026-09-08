/* ota_manifest.c — OTA_PLAN O4: manifest parsing + hex helpers.
 * Deliberately dependency-free C (string.h only): the same translation
 * unit compiles into the guest `ota` app and the host unit test. */

#include <string.h>
#include "ota_manifest.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void ota_digest_hex(const unsigned char digest[32], char out[OTA_SHA256_HEX]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hexd[digest[i] >> 4];
        out[2 * i + 1] = hexd[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

int ota_hex_digest_ok(const unsigned char digest[32], const char *hex64) {
    if (!hex64) return 0;
    for (int i = 0; i < 32; i++) {
        int hi = hexval((unsigned char)hex64[2 * i]);
        int lo = hexval((unsigned char)hex64[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        if (digest[i] != (unsigned char)((hi << 4) | lo)) return 0;
    }
    return 1;
}

/* Copy at most cap-1 bytes; returns 0 on fit, -1 when the value is
 * too long (the manifest names something this build cannot hold). */
static int copy_value(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

int ota_manifest_parse(const char *text, size_t len, ota_manifest *out) {
    if (!text || !out) return OTA_MANIFEST_EARG;

    int have_version = 0, have_url = 0, have_size = 0, have_sha = 0;
    size_t i = 0;

    while (i < len) {
        /* One line at a time. */
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t line_len = i - start;
        if (i < len) i++;                       /* skip '\n' */
        while (line_len > 0 &&
               (text[start + line_len - 1] == '\r' ||
                text[start + line_len - 1] == ' ' ||
                text[start + line_len - 1] == '\t'))
            line_len--;                          /* trim CR/WS */
        if (line_len == 0) continue;             /* blank line */

        const char *line = text + start;
        const char *eq = (const char *)memchr(line, '=', line_len);
        if (!eq) continue;                       /* not key=value: ignore */
        size_t key_len = (size_t)(eq - line);
        const char *val = eq + 1;
        size_t val_len = line_len - key_len - 1;

        if (key_len == 7 && memcmp(line, "version", 7) == 0) {
            if (val_len == 0) return OTA_MANIFEST_EVERSION;
            if (copy_value(out->version, OTA_VERSION_MAX, val, val_len) != 0)
                return OTA_MANIFEST_ETOOLONG;
            have_version = 1;
        } else if (key_len == 3 && memcmp(line, "url", 3) == 0) {
            if (val_len == 0) return OTA_MANIFEST_EURL;
            if (copy_value(out->url, OTA_URL_MAX, val, val_len) != 0)
                return OTA_MANIFEST_EURL;
            have_url = 1;
        } else if (key_len == 4 && memcmp(line, "size", 4) == 0) {
            if (val_len == 0) return OTA_MANIFEST_ESIZE;
            unsigned long long n = 0;
            for (size_t k = 0; k < val_len; k++) {
                if (val[k] < '0' || val[k] > '9') return OTA_MANIFEST_ESIZEFMT;
                n = n * 10u + (unsigned long long)(val[k] - '0');
                if (n > (1ull << 40)) return OTA_MANIFEST_ESIZEFMT; /* >1 TiB */
            }
            out->size = n;
            have_size = 1;
        } else if (key_len == 6 && memcmp(line, "sha256", 6) == 0) {
            if (val_len != 64) return OTA_MANIFEST_ESHAFMT;
            for (size_t k = 0; k < 64; k++)
                if (hexval((unsigned char)val[k]) < 0) return OTA_MANIFEST_ESHAFMT;
            memcpy(out->sha256_hex, val, 64);
            out->sha256_hex[64] = '\0';
            have_sha = 1;
        }
        /* Unknown keys: ignored on purpose (forward compatibility). */
    }

    if (!have_version) return OTA_MANIFEST_EVERSION;
    if (!have_url)     return OTA_MANIFEST_EURL;
    if (!have_size)    return OTA_MANIFEST_ESIZE;
    if (!have_sha)     return OTA_MANIFEST_ESHA;
    return OTA_MANIFEST_OK;
}

const char *ota_manifest_strerror(int err) {
    switch (err) {
    case OTA_MANIFEST_OK:       return "ok";
    case OTA_MANIFEST_EARG:     return "internal: bad arguments";
    case OTA_MANIFEST_EVERSION: return "version= missing or empty";
    case OTA_MANIFEST_EURL:     return "url= missing, empty, or too long";
    case OTA_MANIFEST_ESIZE:    return "size= missing";
    case OTA_MANIFEST_ESIZEFMT: return "size= is not a plain decimal number";
    case OTA_MANIFEST_ESHA:     return "sha256= missing";
    case OTA_MANIFEST_ESHAFMT:  return "sha256= is not 64 hex characters";
    case OTA_MANIFEST_ETOOLONG: return "a value exceeded this build's buffers";
    default:                    return "unknown error";
    }
}
