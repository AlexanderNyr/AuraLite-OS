#ifndef AURALITE_APKG_H
#define AURALITE_APKG_H

#include <stdint.h>
#include <stddef.h>

/*
 * .apkg — the AuraLite package format (SDK_PLAN phase S4).
 *
 * A package file is a short textual header followed by the raw ELF payload:
 *
 *     AURAPKG1\n
 *     name: hello-app\n
 *     version: 1.0.0\n
 *     description: an example application\n
 *     size: 96768\n
 *     crc32: 0x1a2b3c4d\n
 *     \n
 *     <payload bytes>
 *
 * WHY THIS SHAPE
 *
 * Before S4 a "package" was `cp foo.elf foo.pkg` — a renamed executable with
 * no metadata at all, so `apm` could not report what it was about to install,
 * could not detect a truncated download, and could not tell two versions
 * apart.
 *
 * The format is deliberately boring: line-oriented text, no compression, no
 * nesting. The kernel has no decompressor, and this parser reads a file the
 * user was told to obtain from somewhere else — every byte of it is
 * attacker-controlled. It has to be small enough to audit by reading.
 *
 * `crc32` detects corruption. It is NOT a signature and does not pretend to
 * be: anyone who can alter the payload can recompute the checksum. Real
 * tamper-resistance needs a key story this OS does not have, and claiming
 * otherwise would be worse than claiming nothing.
 */

#define APKG_MAGIC        "AURAPKG1"
#define APKG_MAGIC_LEN    8

#define APKG_NAME_MAX     64
#define APKG_VERSION_MAX  32
#define APKG_DESC_MAX     128

/* A header is rejected beyond this; it bounds the parser's scan. */
#define APKG_HEADER_MAX   1024

/* Sanity ceiling on the payload.  The kernel refuses to spawn an image above
 * SPAWN_MAX_IMAGE (1 MiB); allowing a package to declare more than that would
 * only produce a file that installs and then cannot run. */
#define APKG_PAYLOAD_MAX  (1024 * 1024)

struct apkg_header {
    char     name[APKG_NAME_MAX];
    char     version[APKG_VERSION_MAX];
    char     desc[APKG_DESC_MAX];
    uint64_t size;          /* payload length in bytes */
    uint32_t crc32;         /* CRC-32 of the payload */
    uint64_t payload_off;   /* offset of the payload within the file */
};

/* Parse result.  Distinct causes rather than a single failure, because
 * "this file is not a package" and "this package is corrupt" need different
 * things from the user. */
enum apkg_result {
    APKG_OK = 0,
    APKG_ERR_MAGIC,        /* not a package at all */
    APKG_ERR_TRUNCATED,    /* header does not end within the file */
    APKG_ERR_FIELD,        /* malformed or unknown field */
    APKG_ERR_MISSING,      /* a required field is absent */
    APKG_ERR_SIZE,         /* declared size is absurd or exceeds the file */
    APKG_ERR_CRC,          /* payload does not match its checksum */
};

/* Human-readable form of a result. */
const char *apkg_strerror(int result);

/*
 * Parse the header of @buf (@len bytes).
 *
 * Only the header is examined; the payload is not read, so this is usable on
 * a file that has only been partially loaded. The CRC is NOT checked here —
 * use apkg_verify() once the payload is present.
 *
 * Returns APKG_OK, or an apkg_result describing what is wrong.
 */
int apkg_parse(const void *buf, size_t len, struct apkg_header *out);

/*
 * Verify the payload of an already-parsed package.  @buf must hold the whole
 * file. Returns APKG_OK or APKG_ERR_CRC/APKG_ERR_SIZE.
 */
int apkg_verify(const void *buf, size_t len, const struct apkg_header *h);

/* CRC-32 (IEEE 802.3, the zlib polynomial), exposed so the writer and the
 * reader cannot disagree about which one is meant. */
uint32_t apkg_crc32(const void *data, size_t len);

#endif /* AURALITE_APKG_H */
