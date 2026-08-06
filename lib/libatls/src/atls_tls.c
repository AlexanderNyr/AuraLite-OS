/* atls_tls.c — TLS 1.3 client state machine (INTERNET_PLAN.md N3).
 *
 * Full client handshake against a compliant server:
 *   ClientHello → ServerHello → EncryptedExtensions → Certificate →
 *   CertificateVerify → Finished → [client Finished] → application data.
 *
 * Signature verification: Ed25519 (N1) for CertificateVerify.  RSA and
 * ECDSA chains are refused with unsupported_certificate (N5 closes the gap).
 * HelloRetryRequest, alerts, and close_notify are all handled.
 *
 * The recv path uses a buffered adapter (the guest TCP stack delivers
 * per-segment and discards data beyond the requested size).
 *
 * All heap state lives in the atls_tls struct (one malloc at _new,
 * one free at _free); the handshake message buffers are fixed-size fields.
 */

#include "atls_tls_int.h"
#include "atls/atls.h"
#include "atls/tls.h"
#include "atls/x509.h"
#include <string.h>

/* Provided by the process's libc / test harness. */
extern int getentropy(void *buffer, size_t length);
extern void *malloc(size_t);
extern void  free(void *);

/* ---- Client state (defined early so helpers can reference it) ---- */

#define MAX_HN 255
#define MAX_ALPN 63
#define MAX_COOKIE 256
#define MAX_HS_BUF 65536
#define MAX_CERT 8192

struct atls_tls {
    atls_send_fn snd;
    atls_recv_fn rcv;
    void *io;

    char hostname[MAX_HN + 1];
    char alpn_proto[MAX_ALPN + 1];

    uint8_t x_priv[32];
    uint8_t x_pub[32];

    uint8_t session_id[32];
    uint8_t cookie[MAX_COOKIE];
    size_t  cookie_len;

    atls_tls_transcript transcript;

    atls_tls_keys hs_rx, hs_tx, app_rx, app_tx;
    int hs_keys_set;
    int app_keys_set;
    uint8_t client_hs_secret[32];
    uint8_t server_hs_secret[32];
    uint8_t master[32];
    uint8_t client_app_secret[32];
    uint8_t server_app_secret[32];

    uint8_t leaf_cert[MAX_CERT];
    size_t  leaf_cert_len;

    char alpn_selected[MAX_ALPN + 1];

    /* Receive buffer for one record */
    uint8_t rec_hdr[5];
    uint8_t rec_buf[ATLS_TLS_MAX_RECORD];

    /* Handshake reassembly buffer */
    uint8_t hs_buf[MAX_HS_BUF];
    size_t  hs_len;

    int alert_sent;
    int alert_recv;
    int done;
    int closed;
};

/* ---- Helpers ---- */

static int fill_random(uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t take = n - off;
        if (take > 256) take = 256;
        if (getentropy(p + off, take) != 0) return ATLS_ERR_NO_ENTROPY;
        off += take;
    }
    return ATLS_OK;
}

static int send_all(atls_tls *t, const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = t->snd(t->io, data + off, len - off);
        if (n <= 0) return ATLS_ERR_INPUT;
        off += (size_t)n;
    }
    return ATLS_OK;
}

static int read_exact(atls_tls *t, uint8_t *buf, size_t n) {
    size_t off = 0;
    int zero_count = 0;
    while (off < n) {
        int r = t->rcv(t->io, buf + off, n - off);
        if (r < 0) return ATLS_ERR_INPUT;
        if (r == 0) {
            /* The guest TCP recv returns 0 on timeout (no data yet),
             * not necessarily EOF.  Retry a few times before giving up.
             * A real EOF from the peer arrives as a FIN, which tcp_recv
             * handles by returning 0 AND changing conn_state — but we
             * can't detect that here.  Use a retry counter as a pragmatic
             * timeout: if we get 10 consecutive zeros (~10s with the guest's
             * 1s recv timeout), treat it as EOF. */
            if (++zero_count > 10) return ATLS_ERR_PEER_EOF;
            continue;
        }
        zero_count = 0;
        off += (size_t)r;
    }
    return ATLS_OK;
}

static int send_record(atls_tls *t, uint8_t type,
                       const uint8_t *frag, size_t fraglen) {
    uint8_t hdr[5];
    hdr[0] = type;
    atls_wr16(hdr + 1, ATLS_TLS_LEGACY);
    atls_wr16(hdr + 3, (uint16_t)fraglen);
    int rc = send_all(t, hdr, 5);
    if (rc != ATLS_OK) return rc;
    if (fraglen > 0) return send_all(t, frag, fraglen);
    return ATLS_OK;
}

static int send_alert(atls_tls *t, uint8_t level, uint8_t desc) {
    t->alert_sent = desc;
    uint8_t body[2] = { level, desc };
    if (t->app_keys_set) {
        uint8_t rec[ATLS_TLS_MAX_RECORD];
        size_t rlen;
        int rc = atls_tls_encrypt_record(&t->app_tx, ATLS_CT_ALERT,
                                         body, 2, rec, &rlen);
        if (rc == ATLS_OK) send_all(t, rec, rlen);
    } else if (t->hs_keys_set) {
        uint8_t rec[ATLS_TLS_MAX_RECORD];
        size_t rlen;
        int rc = atls_tls_encrypt_record(&t->hs_tx, ATLS_CT_ALERT,
                                         body, 2, rec, &rlen);
        if (rc == ATLS_OK) send_all(t, rec, rlen);
    } else {
        send_record(t, ATLS_CT_ALERT, body, 2);
    }
    t->closed = 1;
    return ATLS_ERR_TLS;
}

static int fail(atls_tls *t, uint8_t desc) {
    return send_alert(t, ATLS_ALERT_FATAL, desc);
}

atls_tls *atls_tls_new(const atls_tls_config *cfg,
                       atls_send_fn snd, atls_recv_fn rcv, void *io) {
    if (!cfg || !cfg->hostname || !snd || !rcv) return NULL;
    atls_tls *t = (atls_tls *)malloc(sizeof(atls_tls));
    if (!t) return NULL;
    atls_wipe(t, sizeof(*t));
    t->snd = snd;
    t->rcv = rcv;
    t->io = io;
    t->alert_sent = -1;
    t->alert_recv = -1;

    size_t hnlen = strlen(cfg->hostname);
    if (hnlen > MAX_HN) { free(t); return NULL; }
    for (size_t i = 0; i < hnlen; i++) t->hostname[i] = cfg->hostname[i];
    t->hostname[hnlen] = 0;

    if (cfg->alpn) {
        size_t alen = strlen(cfg->alpn);
        if (alen > MAX_ALPN) { free(t); return NULL; }
        for (size_t i = 0; i < alen; i++) t->alpn_proto[i] = cfg->alpn[i];
        t->alpn_proto[alen] = 0;
    }
    return t;
}

void atls_tls_free(atls_tls *t) {
    if (t) { atls_wipe(t, sizeof(*t)); free(t); }
}

/* ---- ClientHello construction ---- */

static size_t build_client_hello(atls_tls *t, uint8_t *ch,
                                 const uint8_t legacy_random[32]) {
    uint8_t *p = ch;
    size_t hnlen = strlen(t->hostname);
    size_t alen = t->alpn_proto[0] ? strlen(t->alpn_proto) : 0;

    atls_wr16(p, ATLS_TLS_LEGACY); p += 2;
    for (int i = 0; i < 32; i++) *p++ = legacy_random[i];
    *p++ = 32;
    for (int i = 0; i < 32; i++) *p++ = t->session_id[i];
    atls_wr16(p, 2); p += 2;
    atls_wr16(p, ATLS_CS_CHACHA20_POLY1305_SHA256); p += 2;
    *p++ = 1; *p++ = 0;

    uint8_t *ext_len_ptr = p; p += 2;
    uint8_t *ext_start = p;

    /* server_name */
    atls_wr16(p, ATLS_EXT_SERVER_NAME); p += 2;
    atls_wr16(p, (uint16_t)(hnlen + 5)); p += 2;
    atls_wr16(p, (uint16_t)(hnlen + 3)); p += 2;
    *p++ = 0;
    atls_wr16(p, (uint16_t)hnlen); p += 2;
    for (size_t i = 0; i < hnlen; i++) *p++ = (uint8_t)t->hostname[i];

    /* supported_groups */
    atls_wr16(p, ATLS_EXT_SUPPORTED_GROUPS); p += 2;
    atls_wr16(p, 4); p += 2;
    atls_wr16(p, 2); p += 2;
    atls_wr16(p, ATLS_GROUP_X25519); p += 2;

    /* signature_algorithms */
    atls_wr16(p, ATLS_EXT_SIGNATURE_ALGORITHMS); p += 2;
    atls_wr16(p, 8); p += 2;
    atls_wr16(p, 6); p += 2;
    atls_wr16(p, ATLS_SIG_ED25519); p += 2;
    atls_wr16(p, ATLS_SIG_RSA_PSS_RSAE_SHA256); p += 2;
    atls_wr16(p, ATLS_SIG_ECDSA_SECP256R1_SHA256); p += 2;

    if (alen > 0) {
        atls_wr16(p, ATLS_EXT_ALPN); p += 2;
        atls_wr16(p, (uint16_t)(alen + 3)); p += 2;
        atls_wr16(p, (uint16_t)(alen + 1)); p += 2;
        *p++ = (uint8_t)alen;
        for (size_t i = 0; i < alen; i++) *p++ = (uint8_t)t->alpn_proto[i];
    }

    atls_wr16(p, ATLS_EXT_SUPPORTED_VERSIONS); p += 2;
    atls_wr16(p, 3); p += 2;
    *p++ = 2;
    atls_wr16(p, ATLS_TLS_1_3); p += 2;

    if (t->cookie_len > 0) {
        atls_wr16(p, ATLS_EXT_COOKIE); p += 2;
        atls_wr16(p, (uint16_t)t->cookie_len); p += 2;
        for (size_t i = 0; i < t->cookie_len; i++) *p++ = t->cookie[i];
    }

    atls_wr16(p, ATLS_EXT_KEY_SHARE); p += 2;
    atls_wr16(p, 38); p += 2;
    atls_wr16(p, 36); p += 2;
    atls_wr16(p, ATLS_GROUP_X25519); p += 2;
    atls_wr16(p, 32); p += 2;
    for (int i = 0; i < 32; i++) *p++ = t->x_pub[i];

    atls_wr16(ext_len_ptr, (uint16_t)(p - ext_start));
    return (size_t)(p - ch);
}

/* ---- Record reading and handshake reassembly ---- */

static int read_record(atls_tls *t, uint8_t *rtype, size_t *rlen) {
    int rc = read_exact(t, t->rec_hdr, 5);
    if (rc != ATLS_OK) return rc;
    *rtype = t->rec_hdr[0];
    *rlen = (size_t)atls_rd16(t->rec_hdr + 3);
    if (*rlen > ATLS_TLS_MAX_RECORD) {
        return fail(t, ATLS_ALERT_RECORD_OVERFLOW);
    }
    return read_exact(t, t->rec_buf, *rlen);
}

/* Read one complete handshake message.  For mtype==FINISHED, the caller
 * needs the transcript hash BEFORE the Finished message body is hashed.
 * We return the raw message (header + body) in *msg_out / *msg_len so
 * the caller can snapshot the transcript, then update it. */
static int read_hs_message_raw(atls_tls *t, uint8_t **msg_out,
                               size_t *msg_len) {
    while (1) {
        if (t->hs_len >= 4) {
            uint32_t mlen = atls_rd24(t->hs_buf + 1);
            if (mlen + 4 <= t->hs_len) {
                size_t total = 4 + mlen;
                *msg_out = t->hs_buf;
                *msg_len = total;
                /* Don't remove yet — caller will consume. */
                return ATLS_OK;
            }
        }

        uint8_t rtype;
        size_t rlen;
        int rc = read_record(t, &rtype, &rlen);
        if (rc != ATLS_OK) return rc;

        if (rtype == ATLS_CT_ALERT) {
            if (rlen >= 2) {
                t->alert_recv = t->rec_buf[1];
                if (t->rec_buf[0] == ATLS_ALERT_FATAL) return ATLS_ERR_TLS;
                if (t->rec_buf[1] == ATLS_ALERT_CLOSE_NOTIFY)
                    return ATLS_ERR_PEER_EOF;
            }
            return ATLS_ERR_TLS;
        }
        if (rtype != ATLS_CT_HANDSHAKE && rtype != ATLS_CT_APPLICATION_DATA
            && rtype != ATLS_CT_CHANGE_CIPHER_SPEC) {
            return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);
        }

        uint8_t plain[ATLS_TLS_MAX_RECORD];
        size_t plain_len;
        uint8_t inner_type;

        if (t->hs_keys_set && rtype == ATLS_CT_APPLICATION_DATA) {
            rc = atls_tls_decrypt_record(&t->hs_rx, t->rec_hdr,
                                         rlen + 5, &inner_type,
                                         plain, &plain_len);
            if (rc != ATLS_OK) return fail(t, ATLS_ALERT_BAD_RECORD_MAC);
        } else {
            inner_type = rtype;
            for (size_t i = 0; i < rlen; i++) plain[i] = t->rec_buf[i];
            plain_len = rlen;
        }

        if (inner_type == ATLS_CT_HANDSHAKE) {
            /* TLS 1.3 records may contain zero-padding between the
             * handshake message and the inner content type byte.
             * Only append the actual message bytes (header + body). */
            size_t msg_content_len = plain_len;
            if (plain_len >= 4) {
                uint32_t mlen = atls_rd24(plain + 1);
                size_t full_msg = 4 + mlen;
                if (full_msg <= plain_len)
                    msg_content_len = full_msg;
            }
            if (t->hs_len + msg_content_len > MAX_HS_BUF)
                return fail(t, ATLS_ALERT_INTERNAL_ERROR);
            for (size_t i = 0; i < msg_content_len; i++)
                t->hs_buf[t->hs_len + i] = plain[i];
            t->hs_len += msg_content_len;
        } else if (inner_type == ATLS_CT_ALERT) {
            if (plain_len >= 2) {
                t->alert_recv = plain[1];
                if (plain[0] == ATLS_ALERT_FATAL) return ATLS_ERR_TLS;
                if (plain[1] == ATLS_ALERT_CLOSE_NOTIFY)
                    return ATLS_ERR_PEER_EOF;
            }
        } else if (inner_type == ATLS_CT_CHANGE_CIPHER_SPEC) {
            /* Tolerate for middlebox compat. */
        }
    }
}

/* Consume the first handshake message from hs_buf. */
static void consume_hs_message(atls_tls *t) {
    if (t->hs_len >= 4) {
        uint32_t mlen = atls_rd24(t->hs_buf + 1);
        size_t total = 4 + mlen;
        if (total <= t->hs_len) {
            size_t remain = t->hs_len - total;
            for (size_t i = 0; i < remain; i++)
                t->hs_buf[i] = t->hs_buf[total + i];
            t->hs_len = remain;
        }
    }
}

/* ---- Application data read/write ---- */

int atls_tls_write(atls_tls *t, const uint8_t *data, size_t len) {
    if (!t || !t->done || t->closed) return ATLS_ERR_INPUT;
    if (!t->app_keys_set) return ATLS_ERR_TLS;

    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 16383) chunk = 16383;
        uint8_t rec[ATLS_TLS_MAX_RECORD + 5];
        size_t rlen;
        int rc = atls_tls_encrypt_record(&t->app_tx, ATLS_CT_APPLICATION_DATA,
                                         data + off, chunk, rec, &rlen);
        if (rc != ATLS_OK) return rc;
        rc = send_all(t, rec, rlen);
        if (rc != ATLS_OK) return rc;
        off += chunk;
    }
    return (int)len;
}

int atls_tls_read(atls_tls *t, uint8_t *buf, size_t cap, size_t *out) {
    if (!t || !t->done || t->closed || !out) return ATLS_ERR_INPUT;
    *out = 0;

    while (1) {
        uint8_t rtype;
        size_t rlen;
        int rc = read_record(t, &rtype, &rlen);
        if (rc != ATLS_OK) return rc;

        if (rtype == ATLS_CT_ALERT) {
            if (rlen >= 2) {
                t->alert_recv = t->rec_buf[1];
                if (t->rec_buf[0] == ATLS_ALERT_FATAL) return ATLS_ERR_TLS;
                if (t->rec_buf[1] == ATLS_ALERT_CLOSE_NOTIFY) {
                    t->closed = 1; return ATLS_ERR_PEER_EOF;
                }
            }
            continue;
        }
        if (rtype != ATLS_CT_APPLICATION_DATA)
            return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);

        uint8_t plain[ATLS_TLS_MAX_RECORD];
        size_t plain_len;
        uint8_t inner_type;
        rc = atls_tls_decrypt_record(&t->app_rx, t->rec_hdr,
                                     rlen + 5, &inner_type,
                                     plain, &plain_len);
        if (rc != ATLS_OK) return fail(t, ATLS_ALERT_BAD_RECORD_MAC);

        if (inner_type == ATLS_CT_APPLICATION_DATA) {
            size_t take = plain_len;
            if (take > cap) take = cap;
            for (size_t i = 0; i < take; i++) buf[i] = plain[i];
            *out = take;
            return ATLS_OK;
        } else if (inner_type == ATLS_CT_ALERT) {
            if (plain_len >= 2) {
                t->alert_recv = plain[1];
                if (plain[0] == ATLS_ALERT_FATAL) return ATLS_ERR_TLS;
                if (plain[1] == ATLS_ALERT_CLOSE_NOTIFY) {
                    t->closed = 1; return ATLS_ERR_PEER_EOF;
                }
            }
        } else if (inner_type == ATLS_CT_HANDSHAKE) {
            /* Post-handshake messages. */
            if (plain_len >= 4 && plain[0] == ATLS_HS_KEY_UPDATE
                && t->app_keys_set) {
                /* RFC 8446 §4.6.3: server rotates its sending keys. */
                uint8_t new_ts[32];
                if (atls_tls_update_traffic_secret(t->server_app_secret,
                                                   new_ts) == ATLS_OK) {
                    atls_tls_derive_record_keys(new_ts, &t->app_rx);
                    for (int i = 0; i < 32; i++)
                        t->server_app_secret[i] = new_ts[i];
                    atls_wipe(new_ts, 32);
                }
                /* If the server requested we update too, do it now. */
                if (plain_len >= 2 && plain[1] == 1) {
                    atls_tls_key_update(t, 0);
                }
            }
            /* NST: ignore. */
        }
    }
}

int atls_tls_close(atls_tls *t) {
    if (!t || t->closed) return ATLS_ERR_INPUT;
    return send_alert(t, ATLS_ALERT_WARNING, ATLS_ALERT_CLOSE_NOTIFY);
}

int atls_tls_last_alert_sent(const atls_tls *t) {
    return t ? t->alert_sent : -1;
}
int atls_tls_last_alert_received(const atls_tls *t) {
    return t ? t->alert_recv : -1;
}
const uint8_t *atls_tls_peer_cert(const atls_tls *t, size_t *len) {
    if (!t || t->leaf_cert_len == 0) return NULL;
    if (len) *len = t->leaf_cert_len;
    return t->leaf_cert;
}
const char *atls_tls_negotiated_alpn(const atls_tls *t) {
    if (!t || !t->alpn_selected[0]) return NULL;
    return t->alpn_selected;
}

/* ---- Send a handshake message ---- */

static int send_hs(atls_tls *t, uint8_t type,
                   const uint8_t *body, size_t body_len,
                   int encrypted) {
    uint8_t msg[65536 + 4];
    size_t msg_len = atls_tls_hs_frame(type, body, body_len, msg);
    if (encrypted && t->hs_keys_set) {
        uint8_t rec[ATLS_TLS_MAX_RECORD + 5];
        size_t rlen;
        int rc = atls_tls_encrypt_record(&t->hs_tx, ATLS_CT_HANDSHAKE,
                                         msg, msg_len, rec, &rlen);
        if (rc != ATLS_OK) return rc;
        return send_all(t, rec, rlen);
    }
    return send_record(t, ATLS_CT_HANDSHAKE, msg, msg_len);
}

/* Send a post-handshake message encrypted under app keys. */
static int send_app_hs(atls_tls *t, uint8_t type,
                       const uint8_t *body, size_t body_len) {
    uint8_t msg[65536 + 4];
    size_t msg_len = atls_tls_hs_frame(type, body, body_len, msg);
    if (!t->app_keys_set) return ATLS_ERR_TLS;
    uint8_t rec[ATLS_TLS_MAX_RECORD + 5];
    size_t rlen;
    int rc = atls_tls_encrypt_record(&t->app_tx, ATLS_CT_HANDSHAKE,
                                     msg, msg_len, rec, &rlen);
    if (rc != ATLS_OK) return rc;
    return send_all(t, rec, rlen);
}

/* ---- KeyUpdate (RFC 8446 §4.6.3) ---- */

int atls_tls_key_update(atls_tls *t, int request_update) {
    if (!t || !t->app_keys_set || t->closed) return ATLS_ERR_INPUT;

    /* The KeyUpdate is the LAST record under the old keys. */
    uint8_t body[1];
    body[0] = (uint8_t)(request_update ? 1 : 0);
    int rc = send_app_hs(t, ATLS_HS_KEY_UPDATE, body, 1);
    if (rc != ATLS_OK) return rc;

    /* Now rotate: derive new client traffic secret and keys. */
    uint8_t new_ts[32];
    rc = atls_tls_update_traffic_secret(t->client_app_secret, new_ts);
    if (rc != ATLS_OK) return rc;
    rc = atls_tls_derive_record_keys(new_ts, &t->app_tx);
    for (int i = 0; i < 32; i++) t->client_app_secret[i] = new_ts[i];
    atls_wipe(new_ts, 32);
    return rc;
}

/* ---- Handshake ---- */

int atls_tls_handshake(atls_tls *t) {
    if (!t) return ATLS_ERR_INPUT;
    int rc;

    /* 1. Entropy. */
    uint8_t legacy_random[32];
    rc = fill_random(t->x_priv, 32);
    if (rc != ATLS_OK) return rc;
    rc = fill_random(t->session_id, 32);
    if (rc != ATLS_OK) return rc;
    rc = fill_random(legacy_random, 32);
    if (rc != ATLS_OK) return rc;

    rc = atls_x25519(t->x_pub, t->x_priv, ATLS_X25519_BASEPOINT);
    if (rc != ATLS_OK) return rc;

    atls_tls_transcript_init(&t->transcript);

send_ch: ;
    /* 2. Build and send ClientHello. */
    uint8_t ch[1024];
    size_t ch_body_len = build_client_hello(t, ch + 4, legacy_random);
    ch[0] = ATLS_HS_CLIENT_HELLO;
    atls_wr24(ch + 1, (uint32_t)ch_body_len);
    size_t ch_len = 4 + ch_body_len;
    atls_tls_transcript_update(&t->transcript, ch, ch_len);
    rc = send_record(t, ATLS_CT_HANDSHAKE, ch, ch_len);
    if (rc != ATLS_OK) return rc;

    /* 3. Wait for ServerHello. */
    int got_sh = 0;
    uint8_t server_x25519[32];

    while (!got_sh) {
        uint8_t *msg;
        size_t msg_len;
        rc = read_hs_message_raw(t, &msg, &msg_len);
        if (rc != ATLS_OK) return rc;

        uint8_t mtype = msg[0];
        if (mtype != ATLS_HS_SERVER_HELLO) {
            return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);
        }
        consume_hs_message(t);

        uint8_t *body = msg + 4;
        size_t body_len = msg_len - 4;
        if (body_len < 38) return fail(t, ATLS_ALERT_DECRYPT_ERROR);

        if (atls_rd16(body) != ATLS_TLS_LEGACY)
            return fail(t, ATLS_ALERT_PROTOCOL_VERSION);

        uint8_t sid_len = body[34];
        if (sid_len != 32 || body_len < (size_t)(35 + 32 + 2 + 1 + 2))
            return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
        for (int i = 0; i < 32; i++) {
            if (body[35 + i] != t->session_id[i])
                return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
        }
        if (atls_rd16(body + 67) != ATLS_CS_CHACHA20_POLY1305_SHA256)
            return fail(t, ATLS_ALERT_HANDSHAKE_FAILURE);
        if (body[69] != 0)
            return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);

        size_t ext_off = 70;
        if (ext_off + 2 > body_len)
            return fail(t, ATLS_ALERT_DECRYPT_ERROR);
        uint16_t ext_total = atls_rd16(body + ext_off);
        ext_off += 2;
        if (ext_off + ext_total > body_len)
            return fail(t, ATLS_ALERT_DECRYPT_ERROR);

        /* HRR detection. */
        uint8_t hrr_hash[32];
        atls_sha256("HelloRetryRequest", 17, hrr_hash);
        int hrr_detected = 1;
        for (int i = 0; i < 32; i++) {
            if (body[2 + i] != hrr_hash[i]) { hrr_detected = 0; break; }
        }

        int version_seen = 0, key_share_seen = 0;
        uint16_t selected_group = 0;
        size_t pos = ext_off;
        while (pos + 4 <= ext_off + ext_total) {
            uint16_t etype = atls_rd16(body + pos);
            uint16_t elen = atls_rd16(body + pos + 2);
            pos += 4;
            if (pos + elen > ext_off + ext_total)
                return fail(t, ATLS_ALERT_DECRYPT_ERROR);
            const uint8_t *edata = body + pos;

            if (etype == ATLS_EXT_SUPPORTED_VERSIONS) {
                if (elen != 2 || atls_rd16(edata) != ATLS_TLS_1_3)
                    return fail(t, ATLS_ALERT_PROTOCOL_VERSION);
                version_seen = 1;
            } else if (etype == ATLS_EXT_KEY_SHARE) {
                if (elen != 36 || atls_rd16(edata + 2) != 32)
                    return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
                selected_group = atls_rd16(edata);
                for (int i = 0; i < 32; i++)
                    server_x25519[i] = edata[4 + i];
                key_share_seen = 1;
            } else if (etype == ATLS_EXT_COOKIE && hrr_detected) {
                if (elen > MAX_COOKIE)
                    return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
                for (size_t i = 0; i < elen; i++) t->cookie[i] = edata[i];
                t->cookie_len = elen;
            } else if (!hrr_detected) {
                return fail(t, ATLS_ALERT_UNSUPPORTED_EXTENSION);
            }
            pos += elen;
        }
        if (!version_seen || !key_share_seen)
            return fail(t, ATLS_ALERT_MISSING_EXTENSION);

        atls_tls_transcript_update(&t->transcript, msg, msg_len);

        if (hrr_detected) {
            uint8_t h_ch1[32];
            atls_tls_transcript_snapshot(&t->transcript, h_ch1);
            atls_tls_transcript_init(&t->transcript);
            uint8_t mh_msg[36];
            mh_msg[0] = ATLS_HS_MESSAGE_HASH;
            atls_wr24(mh_msg + 1, 32);
            for (int i = 0; i < 32; i++) mh_msg[4 + i] = h_ch1[i];
            atls_tls_transcript_update(&t->transcript, mh_msg, 36);
            if (selected_group != ATLS_GROUP_X25519)
                return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
            goto send_ch;
        }

        if (selected_group != ATLS_GROUP_X25519)
            return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
        got_sh = 1;
    }

    /* 4. DHE. */
    uint8_t dhe[32];
    rc = atls_x25519(dhe, t->x_priv, server_x25519);
    if (rc != ATLS_OK) return fail(t, ATLS_ALERT_HANDSHAKE_FAILURE);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= dhe[i];
    if (acc == 0) return fail(t, ATLS_ALERT_HANDSHAKE_FAILURE);

    /* 5. Handshake secrets. */
    uint8_t h_ch_sh[32];
    atls_tls_transcript_snapshot(&t->transcript, h_ch_sh);
    rc = atls_tls_derive_handshake_secrets(dhe, h_ch_sh,
                                           t->client_hs_secret,
                                           t->server_hs_secret,
                                           t->master);
    atls_wipe(dhe, 32);
    if (rc != ATLS_OK) return rc;

    rc = atls_tls_derive_record_keys(t->server_hs_secret, &t->hs_rx);
    if (rc != ATLS_OK) return rc;
    rc = atls_tls_derive_record_keys(t->client_hs_secret, &t->hs_tx);
    if (rc != ATLS_OK) return rc;
    t->hs_keys_set = 1;

    /* 6. Encrypted handshake messages. */
    enum { EXPECT_EE, EXPECT_CERT, EXPECT_CV, EXPECT_FINISHED } hs_state
        = EXPECT_EE;
    int got_finished = 0;

    while (!got_finished) {
        uint8_t *msg;
        size_t msg_len;
        rc = read_hs_message_raw(t, &msg, &msg_len);
        if (rc != ATLS_OK) return rc;

        uint8_t mtype = msg[0];
        uint8_t *body = msg + 4;
        size_t body_len = msg_len - 4;

        /* Snapshot the transcript hash BEFORE updating with this message.
         * Needed for both CertificateVerify (signs hash through Certificate)
         * and Finished (verifies hash through the previous message). */
        uint8_t h_before_update[32];
        atls_tls_transcript_snapshot(&t->transcript, h_before_update);

        /* Update transcript with the DECRYPTED handshake message. */
        atls_tls_transcript_update(&t->transcript, msg, msg_len);
        consume_hs_message(t);

        if (mtype == ATLS_HS_ENCRYPTED_EXTENSIONS) {
            if (hs_state != EXPECT_EE)
                return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);

            if (body_len >= 2) {
                uint16_t eetotal = atls_rd16(body);
                size_t epos = 2;
                while (epos + 4 <= (size_t)(2 + eetotal) &&
                       epos + 4 <= body_len) {
                    uint16_t et = atls_rd16(body + epos);
                    uint16_t el = atls_rd16(body + epos + 2);
                    epos += 4;
                    if (epos + el > body_len) break;
                    if (et == ATLS_EXT_ALPN && el >= 3) {
                        uint16_t plen = atls_rd16(body + epos);
                        if (plen >= 1 && epos + 2 + plen <= body_len) {
                            uint8_t proto_len = body[epos + 2];
                            if (proto_len <= MAX_ALPN &&
                                epos + 3 + proto_len <= body_len) {
                                for (size_t i = 0; i < proto_len; i++)
                                    t->alpn_selected[i] =
                                        (char)body[epos + 3 + i];
                                t->alpn_selected[proto_len] = 0;
                            }
                        }
                    }
                    epos += el;
                }
            }
            hs_state = EXPECT_CERT;

        } else if (mtype == ATLS_HS_CERTIFICATE) {
            if (hs_state != EXPECT_CERT)
                return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);

            if (body_len < 7)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            /* Skip certificate_request_context (1 byte). */
            size_t co = 1 + body[0];
            if (co + 3 > body_len)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            /* Read and skip the certificate_list length (3 bytes). */
            uint32_t list_len = atls_rd24(body + co);
            co += 3;
            if (co + list_len > body_len)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);

            /* Read the first CertificateEntry (leaf). */
            if (co + 3 > body_len)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            uint32_t cert_len = atls_rd24(body + co);
            co += 3;
            if (co + cert_len > body_len)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            if (cert_len > MAX_CERT)
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);

            for (size_t i = 0; i < cert_len; i++)
                t->leaf_cert[i] = body[co + i];
            t->leaf_cert_len = cert_len;

            atls_x509_cert x509;
            int xrc = atls_x509_parse(t->leaf_cert, t->leaf_cert_len, &x509);
            if (xrc != ATLS_OK) {
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            }
            hs_state = EXPECT_CV;

        } else if (mtype == ATLS_HS_CERTIFICATE_VERIFY) {
            if (hs_state != EXPECT_CV)
                return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);

            if (body_len < 4)
                return fail(t, ATLS_ALERT_DECRYPT_ERROR);
            uint16_t sig_scheme = atls_rd16(body);
            uint16_t sig_len = atls_rd16(body + 2);
            if ((size_t)sig_len + 4 != body_len)
                return fail(t, ATLS_ALERT_DECRYPT_ERROR);
            const uint8_t *sig = body + 4;

            uint8_t content[64 + 33 + 1 + 32];
            size_t content_len = 0;
            for (int i = 0; i < 64; i++) content[content_len++] = 0x20;
            const char *ctx_str = "TLS 1.3, server CertificateVerify";
            for (size_t i = 0; i < 33; i++)
                content[content_len++] = (uint8_t)ctx_str[i];
            content[content_len++] = 0x00;

            /* Transcript hash through Certificate (saved before CV update). */
            for (int i = 0; i < 32; i++)
                content[content_len + i] = h_before_update[i];
            content_len += 32;

            atls_x509_cert leaf;
            if (atls_x509_parse(t->leaf_cert, t->leaf_cert_len, &leaf)
                != ATLS_OK) {
                return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
            }

            if (sig_scheme == ATLS_SIG_ED25519) {
                if (!atls_oid_eq(&leaf.spki_alg_oid, ATLS_OID_ED25519, 3))
                    return fail(t, ATLS_ALERT_ILLEGAL_PARAMETER);
                if (leaf.spki_key.len != 32 || leaf.spki_key_unused_bits)
                    return fail(t, ATLS_ALERT_BAD_CERTIFICATE);
                rc = atls_ed25519_verify(sig, leaf.spki_key.data,
                                         content, content_len);
                if (rc != ATLS_OK)
                    return fail(t, ATLS_ALERT_DECRYPT_ERROR);
            } else {
                return fail(t, ATLS_ALERT_UNSUPPORTED_CERTIFICATE);
            }
            hs_state = EXPECT_FINISHED;

        } else if (mtype == ATLS_HS_FINISHED) {
            if (hs_state != EXPECT_FINISHED)
                return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);

            /* Verify using the hash BEFORE this Finished was added. */
            rc = atls_tls_verify_finished(t->server_hs_secret,
                                          h_before_update, body);
            if (rc != ATLS_OK)
                return fail(t, ATLS_ALERT_DECRYPT_ERROR);

            /* Transcript now includes server Finished (updated above).
             * The hash for app secret derivation includes server Finished. */
            uint8_t h_transcript_done[32];
            atls_tls_transcript_snapshot(&t->transcript, h_transcript_done);

            /* Client Finished is computed over transcript through server Finished. */
            uint8_t client_finished[32];
            rc = atls_tls_compute_finished(t->client_hs_secret,
                                           h_transcript_done,
                                           client_finished);
            if (rc != ATLS_OK) return rc;

            rc = send_hs(t, ATLS_HS_FINISHED, client_finished, 32, 1);
            if (rc != ATLS_OK) return rc;

            /* Update transcript with client Finished. */
            uint8_t cf_msg[36];
            cf_msg[0] = ATLS_HS_FINISHED;
            atls_wr24(cf_msg + 1, 32);
            for (int i = 0; i < 32; i++) cf_msg[4 + i] = client_finished[i];
            atls_tls_transcript_update(&t->transcript, cf_msg, 36);
            atls_wipe(client_finished, 32);

            /* Derive application secrets. */
            uint8_t cap_secret[32], sap_secret[32];
            rc = atls_tls_derive_app_secrets(t->master, h_transcript_done,
                                             cap_secret, sap_secret);
            if (rc != ATLS_OK) return rc;
            rc = atls_tls_derive_record_keys(cap_secret, &t->app_tx);
            if (rc != ATLS_OK) return rc;
            rc = atls_tls_derive_record_keys(sap_secret, &t->app_rx);
            if (rc != ATLS_OK) return rc;
            /* Store traffic secrets for KeyUpdate (RFC 8446 §4.6.3). */
            for (int i = 0; i < 32; i++) {
                t->client_app_secret[i] = cap_secret[i];
                t->server_app_secret[i] = sap_secret[i];
            }
            t->app_keys_set = 1;
            atls_wipe(cap_secret, 32);
            atls_wipe(sap_secret, 32);

            got_finished = 1;

        } else if (mtype == ATLS_HS_NEW_SESSION_TICKET) {
            /* Tolerate: ignore. */
        } else {
            return fail(t, ATLS_ALERT_UNEXPECTED_MESSAGE);
        }
    }

    t->done = 1;
    return ATLS_OK;
}
