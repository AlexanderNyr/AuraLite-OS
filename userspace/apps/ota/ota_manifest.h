#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

/* ota_manifest.h — OTA_PLAN O4: the update manifest, parsed in exactly
 * one place so the guest tool and the host unit test run the same bytes
 * (the test_sha256sum no-second-copy discipline).
 *
 * Format: line-based ASCII, `key=value`, one per line.  Required keys:
 *   version=<short string>          e.g. 0.0.2-ota
 *   url=<payload URL>               e.g. http://10.0.2.2:18081/kernel.elf
 *   size=<payload bytes, decimal>
 *   sha256=<64 lowercase/uppercase hex chars>
 * Unknown keys and blank lines are ignored (forward compatibility); a
 * missing or malformed required key is a hard error — an update whose
 * description cannot be verified is not an update. */

#include <stddef.h>

#define OTA_VERSION_MAX 32
#define OTA_URL_MAX     512
#define OTA_SHA256_HEX  65   /* 64 chars + NUL */

typedef struct {
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
    unsigned long long size;
    char sha256_hex[OTA_SHA256_HEX];
} ota_manifest;

/* Parse errors (ota_manifest_strerror names them for humans). */
#define OTA_MANIFEST_OK          0
#define OTA_MANIFEST_EARG       (-1)   /* NULL text/out */
#define OTA_MANIFEST_EVERSION   (-2)   /* version= missing or empty */
#define OTA_MANIFEST_EURL       (-3)   /* url= missing/empty/too long */
#define OTA_MANIFEST_ESIZE      (-4)   /* size= missing */
#define OTA_MANIFEST_ESIZEFMT   (-5)   /* size= not a plain decimal number */
#define OTA_MANIFEST_ESHA       (-6)   /* sha256= missing */
#define OTA_MANIFEST_ESHAFMT    (-7)   /* sha256= not 64 hex chars */
#define OTA_MANIFEST_ETOOLONG   (-8)   /* a value exceeded its buffer */

int ota_manifest_parse(const char *text, size_t len, ota_manifest *out);
const char *ota_manifest_strerror(int err);

/* Hex helpers shared by the digest checks (also unit-tested).
 * ota_hex_digest compares a 32-byte digest against a 64-char hex string
 * case-insensitively; returns 1 on match. */
void ota_digest_hex(const unsigned char digest[32], char out[OTA_SHA256_HEX]);
int  ota_hex_digest_ok(const unsigned char digest[32], const char *hex64);

#endif /* OTA_MANIFEST_H */
