#ifndef LIBATLS_ATLS_TLS_INT_H
#define LIBATLS_ATLS_TLS_INT_H

/* atls_tls_int.h — TLS 1.3 internal primitives (INTERNET_PLAN.md N3).
 *
 * Key schedule, transcript hash, record-layer AEAD, and handshake
 * message framing.  Shared by the client (atls_tls.c) and the host-side
 * tamper/mock-server tests.
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"

/* ---- TLS record types ---- */
#define ATLS_CT_CHANGE_CIPHER_SPEC 20
#define ATLS_CT_ALERT              21
#define ATLS_CT_HANDSHAKE          22
#define ATLS_CT_APPLICATION_DATA   23

/* ---- TLS handshake types ---- */
#define ATLS_HS_CLIENT_HELLO       1
#define ATLS_HS_SERVER_HELLO       2
#define ATLS_HS_NEW_SESSION_TICKET 4
#define ATLS_HS_ENCRYPTED_EXTENSIONS 8
#define ATLS_HS_CERTIFICATE       11
#define ATLS_HS_CERTIFICATE_VERIFY 15
#define ATLS_HS_FINISHED          20
#define ATLS_HS_KEY_UPDATE        24
#define ATLS_HS_MESSAGE_HASH     254

/* ---- Alert levels ---- */
#define ATLS_ALERT_WARNING 1
#define ATLS_ALERT_FATAL   2

/* ---- Alert descriptions ---- */
#define ATLS_ALERT_CLOSE_NOTIFY           0
#define ATLS_ALERT_UNEXPECTED_MESSAGE    10
#define ATLS_ALERT_BAD_RECORD_MAC        20
#define ATLS_ALERT_HANDSHAKE_FAILURE     40
#define ATLS_ALERT_BAD_CERTIFICATE       42
#define ATLS_ALERT_UNSUPPORTED_CERTIFICATE 43
#define ATLS_ALERT_CERTIFICATE_UNKNOWN   46
#define ATLS_ALERT_ILLEGAL_PARAMETER     47
#define ATLS_ALERT_RECORD_OVERFLOW       22
#define ATLS_ALERT_DECRYPT_ERROR         51
#define ATLS_ALERT_PROTOCOL_VERSION      70
#define ATLS_ALERT_INTERNAL_ERROR        80
#define ATLS_ALERT_MISSING_EXTENSION    109
#define ATLS_ALERT_UNSUPPORTED_EXTENSION 110
#define ATLS_ALERT_NO_APPLICATION_PROTOCOL 120

/* ---- TLS 1.3 version ---- */
#define ATLS_TLS_1_3 0x0304
#define ATLS_TLS_LEGACY 0x0303

/* ---- Cipher suite ---- */
#define ATLS_CS_CHACHA20_POLY1305_SHA256 0x1303

/* ---- Signature schemes ---- */
#define ATLS_SIG_ECDSA_SECP256R1_SHA256 0x0403
#define ATLS_SIG_RSA_PSS_RSAE_SHA256   0x0804
#define ATLS_SIG_ED25519               0x0807

/* ---- Named groups ---- */
#define ATLS_GROUP_X25519 0x001d

/* ---- Extension types ---- */
#define ATLS_EXT_SERVER_NAME        0
#define ATLS_EXT_SUPPORTED_GROUPS  10
#define ATLS_EXT_SIGNATURE_ALGORITHMS 13
#define ATLS_EXT_ALPN             16
#define ATLS_EXT_SUPPORTED_VERSIONS 43
#define ATLS_EXT_COOKIE           44
#define ATLS_EXT_KEY_SHARE        51

/* ---- Maximum record sizes ---- */
#define ATLS_TLS_MAX_RECORD 16640  /* 2^14 + 256 */

/* ---- Per-direction AEAD key material ---- */
typedef struct {
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t seq;
} atls_tls_keys;

/* ---- Transcript hash (streaming SHA-256) ---- */
typedef struct {
    atls_sha256_ctx sha;
} atls_tls_transcript;

void atls_tls_transcript_init(atls_tls_transcript *t);
void atls_tls_transcript_update(atls_tls_transcript *t,
                                const void *data, size_t len);
void atls_tls_transcript_hash(const atls_tls_transcript *t,
                              uint8_t out[32]);
/* Snapshot: get the hash without destroying the context. */
void atls_tls_transcript_snapshot(const atls_tls_transcript *t,
                                  uint8_t out[32]);

/* ---- HKDF-Expand-Label (RFC 8446 §7.1) ---- */
int atls_tls_hkdf_expand_label(const uint8_t secret[32],
                               const char *label,
                               const uint8_t *context, size_t ctxlen,
                               uint8_t *out, size_t outlen);

/* ---- Key schedule (RFC 8446 §7.1, DHE-only, no PSK) ---- */
/* Derives handshake_traffic_secret and master_secret from the ECDHE
 * shared secret and the hash of ClientHello..ServerHello. */
int atls_tls_derive_handshake_secrets(const uint8_t dhe[32],
                                      const uint8_t h_ch_sh[32],
                                      uint8_t chs[32],
                                      uint8_t shs[32],
                                      uint8_t master[32]);

/* Derives application traffic secrets from the master secret and the
 * hash of the full handshake transcript through server Finished. */
int atls_tls_derive_app_secrets(const uint8_t master[32],
                                const uint8_t h_transcript[32],
                                uint8_t cap[32],
                                uint8_t sap[32]);

/* Derive record-layer keys from a traffic secret. */
int atls_tls_derive_record_keys(const uint8_t ts[32],
                                atls_tls_keys *out);

/* ---- Finished verify data ---- */
int atls_tls_compute_finished(const uint8_t ts[32],
                              const uint8_t transcript_hash[32],
                              uint8_t verify_data[32]);

int atls_tls_verify_finished(const uint8_t ts[32],
                             const uint8_t transcript_hash[32],
                             const uint8_t expected[32]);

/* ---- AEAD record encrypt/decrypt ---- */
/* Encrypt: inner_type byte is appended as the TLS 1.3 inner content type.
 * out must be at least 5 + ptlen + 1 + 16 (record header + plaintext +
 * inner type + AEAD tag).  Returns ATLS_OK and writes the record length
 * into *out_len. */
int atls_tls_encrypt_record(atls_tls_keys *k,
                            uint8_t inner_type,
                            const uint8_t *pt, size_t ptlen,
                            uint8_t *out, size_t *out_len);

/* Decrypt: strip AEAD, return the plaintext and inner type.
 * pt_out must be at least reclen - 5 - 16 bytes.
 * Returns ATLS_OK or ATLS_ERR_AUTH. */
int atls_tls_decrypt_record(atls_tls_keys *k,
                            const uint8_t *record, size_t reclen,
                            uint8_t *inner_type,
                            uint8_t *pt_out, size_t *pt_len);

/* ---- Key update (RFC 8446 §4.6.3) ---- */
int atls_tls_update_traffic_secret(const uint8_t current[32],
                                   uint8_t updated[32]);

/* ---- Byte helpers ---- */
static inline uint16_t atls_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}
static inline uint32_t atls_rd24(const uint8_t *p) {
    return (uint32_t)((uint32_t)p[0] << 16 |
                      (uint32_t)p[1] << 8  | p[2]);
}
static inline void atls_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void atls_wr24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}
static inline void atls_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- Handshake message framing ---- */
/* Frame a handshake message (add 4-byte header).  out must be at least
 * 4 + body_len bytes.  Returns total length written. */
static inline size_t atls_tls_hs_frame(uint8_t type,
                                       const uint8_t *body, size_t body_len,
                                       uint8_t *out) {
    out[0] = type;
    atls_wr24(out + 1, (uint32_t)body_len);
    if (body_len > 0) {
        for (size_t i = 0; i < body_len; i++) out[4 + i] = body[i];
    }
    return 4 + body_len;
}

#endif /* LIBATLS_ATLS_TLS_INT_H */
