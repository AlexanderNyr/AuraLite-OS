/* atls_tls_keys.c — TLS 1.3 key schedule and record-layer crypto
 * (INTERNET_PLAN.md phase N3).
 *
 * RFC 8446 §7.1 key schedule (DHE-only, SHA-256) and §5.2 AEAD record
 * encrypt/decrypt (ChaCha20-Poly1305).  Zero allocation.
 */

#include "atls_tls_int.h"
#include <string.h>

/* ---- Transcript hash ---- */

void atls_tls_transcript_init(atls_tls_transcript *t) {
    atls_sha256_init(&t->sha);
}

void atls_tls_transcript_update(atls_tls_transcript *t,
                                const void *data, size_t len) {
    atls_sha256_update(&t->sha, data, len);
}

void atls_tls_transcript_hash(const atls_tls_transcript *t,
                              uint8_t out[32]) {
    atls_sha256_ctx copy = t->sha;
    atls_sha256_final(&copy, out);
}

void atls_tls_transcript_snapshot(const atls_tls_transcript *t,
                                  uint8_t out[32]) {
    atls_tls_transcript_hash(t, out);
}

/* ---- HKDF-Expand-Label (RFC 8446 §7.1) ---- */

int atls_tls_hkdf_expand_label(const uint8_t secret[32],
                               const char *label,
                               const uint8_t *context, size_t ctxlen,
                               uint8_t *out, size_t outlen) {
    /* info = uint16(outlen) || uint8(len("tls13 "+label)) ||
     *        "tls13 " + label || uint8(ctxlen) || context */
    size_t labellen = strlen(label);
    size_t prefix_len = 6 + labellen;  /* "tls13 " */
    size_t info_len = 2 + 1 + prefix_len + 1 + ctxlen;

    uint8_t info[256]; /* max: ~70 bytes for all our labels */
    if (info_len > sizeof(info)) return ATLS_ERR_INPUT;

    size_t pos = 0;
    atls_wr16(info + pos, (uint16_t)outlen); pos += 2;
    info[pos++] = (uint8_t)prefix_len;
    /* "tls13 " */
    info[pos++] = 't'; info[pos++] = 'l'; info[pos++] = 's';
    info[pos++] = '1'; info[pos++] = '3'; info[pos++] = ' ';
    for (size_t i = 0; i < labellen; i++) info[pos++] = (uint8_t)label[i];
    info[pos++] = (uint8_t)ctxlen;
    for (size_t i = 0; i < ctxlen; i++) info[pos++] = context[i];

    return atls_hkdf_expand(secret, info, info_len, out, outlen);
}

/* ---- Key schedule (RFC 8446 §7.1, PSK-less) ---- */

/* Derive-Secret(Secret, Label, Messages) =
 *    HKDF-Expand-Label(Secret, Label,
 *                      Transcript-Hash(Messages), Hash.length) */
static int derive_secret(const uint8_t secret[32],
                         const char *label,
                         const uint8_t *messages_hash,
                         uint8_t out[32]) {
    return atls_tls_hkdf_expand_label(secret, label,
                                      messages_hash, 32, out, 32);
}

int atls_tls_derive_handshake_secrets(const uint8_t dhe[32],
                                      const uint8_t h_ch_sh[32],
                                      uint8_t chs[32],
                                      uint8_t shs[32],
                                      uint8_t master[32]) {
    static const uint8_t zeros[32] = { 0 };
    uint8_t early_secret[32];
    uint8_t derived_early[32];
    uint8_t handshake_secret[32];
    uint8_t derived_hs[32];
    uint8_t empty_hash[32];
    int rc;

    /* EarlySecret = HKDF-Extract(0, 0) */
    rc = atls_hkdf_extract(zeros, 32, zeros, 32, early_secret);
    if (rc != ATLS_OK) return rc;

    /* empty_hash = SHA-256("") */
    atls_sha256(zeros, 0, empty_hash);

    /* derived_early = Derive-Secret(early_secret, "derived", empty_hash) */
    rc = derive_secret(early_secret, "derived", empty_hash, derived_early);
    if (rc != ATLS_OK) return rc;

    /* HandshakeSecret = HKDF-Extract(derived_early, ECDHE) */
    rc = atls_hkdf_extract(derived_early, 32, dhe, 32, handshake_secret);
    if (rc != ATLS_OK) return rc;

    /* client_handshake_traffic_secret = Derive-Secret(handshake_secret,
     *                                                  "c hs traffic", h_ch_sh) */
    rc = derive_secret(handshake_secret, "c hs traffic", h_ch_sh, chs);
    if (rc != ATLS_OK) return rc;

    /* server_handshake_traffic_secret */
    rc = derive_secret(handshake_secret, "s hs traffic", h_ch_sh, shs);
    if (rc != ATLS_OK) return rc;

    /* derived_hs = Derive-Secret(handshake_secret, "derived", empty_hash) */
    rc = derive_secret(handshake_secret, "derived", empty_hash, derived_hs);
    if (rc != ATLS_OK) return rc;

    /* MasterSecret = HKDF-Extract(derived_hs, 0) */
    rc = atls_hkdf_extract(derived_hs, 32, zeros, 32, master);

    /* Wipe intermediates */
    atls_wipe(early_secret, 32);
    atls_wipe(derived_early, 32);
    atls_wipe(handshake_secret, 32);
    atls_wipe(derived_hs, 32);

    return rc;
}

int atls_tls_derive_app_secrets(const uint8_t master[32],
                                const uint8_t h_transcript[32],
                                uint8_t cap[32],
                                uint8_t sap[32]) {
    int rc;
    rc = derive_secret(master, "c ap traffic", h_transcript, cap);
    if (rc != ATLS_OK) return rc;
    rc = derive_secret(master, "s ap traffic", h_transcript, sap);
    return rc;
}

int atls_tls_derive_record_keys(const uint8_t ts[32],
                                atls_tls_keys *out) {
    int rc;
    atls_wipe(out, sizeof(*out));
    rc = atls_tls_hkdf_expand_label(ts, "key", NULL, 0, out->key, 32);
    if (rc != ATLS_OK) return rc;
    rc = atls_tls_hkdf_expand_label(ts, "iv", NULL, 0, out->iv, 12);
    out->seq = 0;
    return rc;
}

/* ---- Finished ---- */

int atls_tls_compute_finished(const uint8_t ts[32],
                              const uint8_t transcript_hash[32],
                              uint8_t verify_data[32]) {
    uint8_t finished_key[32];
    int rc = atls_tls_hkdf_expand_label(ts, "finished", NULL, 0,
                                        finished_key, 32);
    if (rc != ATLS_OK) return rc;
    atls_hmac_sha256(finished_key, 32, transcript_hash, 32, verify_data);
    atls_wipe(finished_key, 32);
    return ATLS_OK;
}

int atls_tls_verify_finished(const uint8_t ts[32],
                             const uint8_t transcript_hash[32],
                             const uint8_t expected[32]) {
    uint8_t computed[32];
    int rc = atls_tls_compute_finished(ts, transcript_hash, computed);
    if (rc != ATLS_OK) return rc;
    int ok = atls_ct_eq(computed, expected, 32);
    atls_wipe(computed, 32);
    return ok ? ATLS_OK : ATLS_ERR_AUTH;
}

/* ---- AEAD record encrypt/decrypt ---- */

/* Build the per-record nonce = IV XOR sequence (big-endian, zero-padded). */
static void build_nonce(const uint8_t iv[12], uint64_t seq,
                        uint8_t nonce[12]) {
    uint8_t seq_bytes[12] = { 0 };
    /* seq as big-endian in the last 8 bytes. */
    for (int i = 0; i < 8; i++) {
        seq_bytes[11 - i] = (uint8_t)(seq >> (i * 8));
    }
    for (int i = 0; i < 12; i++) {
        nonce[i] = (uint8_t)(iv[i] ^ seq_bytes[i]);
    }
}

int atls_tls_encrypt_record(atls_tls_keys *k,
                            uint8_t inner_type,
                            const uint8_t *pt, size_t ptlen,
                            uint8_t *out, size_t *out_len) {
    /* Plaintext = pt || inner_type (no padding for simplicity in N3). */
    size_t plaintext_len = ptlen + 1;
    size_t record_len = 5 + plaintext_len + 16; /* header + plain + tag */

    /* Build the nonce. */
    uint8_t nonce[12];
    build_nonce(k->iv, k->seq, nonce);

    /* AAD = record header (type, version, length) — without the tag. */
    uint8_t aad[5];
    aad[0] = ATLS_CT_APPLICATION_DATA;
    atls_wr16(aad + 1, ATLS_TLS_LEGACY);
    atls_wr16(aad + 3, (uint16_t)plaintext_len + 16);

    /* Build plaintext buffer: pt || inner_type. */
    uint8_t plain[ATLS_TLS_MAX_RECORD + 1];
    if (plaintext_len > sizeof(plain)) return ATLS_ERR_INPUT;
    for (size_t i = 0; i < ptlen; i++) plain[i] = pt[i];
    plain[ptlen] = inner_type;

    /* Encrypt in-place. */
    uint8_t ct[ATLS_TLS_MAX_RECORD + 16];
    uint8_t tag[16];
    int rc = atls_aead_encrypt(k->key, nonce, aad, 5,
                               plain, plaintext_len, ct, tag);
    if (rc != ATLS_OK) return rc;

    /* Assemble the record. */
    out[0] = ATLS_CT_APPLICATION_DATA;
    atls_wr16(out + 1, ATLS_TLS_LEGACY);
    atls_wr16(out + 3, (uint16_t)(plaintext_len + 16));
    for (size_t i = 0; i < plaintext_len; i++) out[5 + i] = ct[i];
    for (int i = 0; i < 16; i++) out[5 + plaintext_len + i] = tag[i];

    *out_len = record_len;
    k->seq++;
    return ATLS_OK;
}

int atls_tls_decrypt_record(atls_tls_keys *k,
                            const uint8_t *record, size_t reclen,
                            uint8_t *inner_type,
                            uint8_t *pt_out, size_t *pt_len) {
    if (reclen < 5 + 16) return ATLS_ERR_TRUNCATED;

    uint16_t rlen = atls_rd16(record + 3);
    if ((size_t)rlen + 5 != reclen) return ATLS_ERR_BAD_LENGTH;

    /* Nonce */
    uint8_t nonce[12];
    build_nonce(k->iv, k->seq, nonce);

    /* AAD = record header */
    const uint8_t *aad = record;

    /* Ciphertext + tag */
    const uint8_t *ct = record + 5;
    size_t ct_tag_len = rlen;
    size_t ct_len = ct_tag_len - 16;
    const uint8_t *tag = ct + ct_len;

    uint8_t plain[ATLS_TLS_MAX_RECORD];
    int rc = atls_aead_decrypt(k->key, nonce, aad, 5,
                               ct, ct_len, tag, plain);
    if (rc != ATLS_OK) return rc;

    /* Strip padding and extract inner type. */
    if (ct_len < 1) return ATLS_ERR_BAD_ENCODING;
    *inner_type = plain[ct_len - 1];
    *pt_len = ct_len - 1;
    for (size_t i = 0; i < *pt_len; i++) pt_out[i] = plain[i];

    k->seq++;
    return ATLS_OK;
}
