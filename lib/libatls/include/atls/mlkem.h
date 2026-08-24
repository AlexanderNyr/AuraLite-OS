#ifndef LIBATLS_MLKEM_H
#define LIBATLS_MLKEM_H

/* atls/mlkem.h — ML-KEM-768 (FIPS 203), REALINTERNET2 Y5.
 *
 * Parameter set 768 only (D4: hybrid-or-nothing; Y6 consumes this
 * inside X25519MLKEM768).  The internal APIs take the seeds so the
 * host KAT can replay ACVP vectors byte-for-byte.
 *
 *   ek  1184  dk  2400  ct  1088  ss  32
 */

#include "atls/atls.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATLS_MLKEM768_K           3
#define ATLS_MLKEM768_EK_BYTES    1184
#define ATLS_MLKEM768_DK_BYTES    2400
#define ATLS_MLKEM768_CT_BYTES    1088
#define ATLS_MLKEM768_SS_BYTES    32
#define ATLS_MLKEM768_SEED_BYTES  32

/* KeyGen_internal(d, z) → (ek, dk). */
int atls_mlkem768_keygen(uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                         uint8_t dk[ATLS_MLKEM768_DK_BYTES],
                         const uint8_t d[ATLS_MLKEM768_SEED_BYTES],
                         const uint8_t z[ATLS_MLKEM768_SEED_BYTES]);

/* Encaps_internal(ek, m) → (ct, ss).  Rejects a modulus-invalid ek. */
int atls_mlkem768_encaps(uint8_t ct[ATLS_MLKEM768_CT_BYTES],
                         uint8_t ss[ATLS_MLKEM768_SS_BYTES],
                         const uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                         const uint8_t m[ATLS_MLKEM768_SEED_BYTES]);

/* Decaps_internal(dk, ct) → ss.  Implicit rejection on a mismatched
 * re-encrypt: returns J(z || ct), never an error path (FO contract). */
int atls_mlkem768_decaps(uint8_t ss[ATLS_MLKEM768_SS_BYTES],
                         const uint8_t ct[ATLS_MLKEM768_CT_BYTES],
                         const uint8_t dk[ATLS_MLKEM768_DK_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_MLKEM_H */
