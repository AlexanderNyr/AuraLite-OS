#!/usr/bin/env bash
# tests/unit/test_libatls_m32.sh -- the crypto stack at 32-bit width.
#
# HISTORY, kept honestly: I386_PLAN I8 first ran only the SYMMETRIC
# subset here (SHA/HMAC/HKDF/ChaCha20/Poly1305/AEAD) and recorded a
# real boundary: atls_fe.c and atls_ecdsa.c used unsigned __int128,
# which does not exist at -m32.  RESIDUE_PLAN R10 (RES-29) closed that
# boundary: atls_fe.h now selects a packed 8x-uint32 radix-2^32 field
# representation and atls_ecdsa.c an 8x-uint32 limb parameterisation
# whenever __int128 is missing -- so THIS gate now runs the COMPLETE
# suite, the same five test binaries test_libatls_rv64.sh runs:
# hash, AEAD, X25519 (RFC 7748 + Wycheproof low-order + the
# 1000-iteration ladder), Ed25519 (RFC 8032), ECDSA P-256.
#
# TWO lanes, so the 32-bit path executes even where multilib is absent:
#   lane A (always): -DATLS_FE_FORCE32 on the NATIVE host compiler --
#     the same 32-bit limb code, exercised without any cross toolchain.
#   lane B (-m32 when the host can): the real ILP32 ABI -- int is the
#     limb width, size_t is 32 bits, alignment rules differ.  SKIP is
#     honest here because lane A has already executed the path.
#
# The symmetric sources keep their no-__int128 guard: those files have
# no width ifdef, so growing a bare __int128 would silently poison the
# -m32 build.  atls_fe.c/atls_ecdsa.c are exempt BY NAME -- their
# __int128 sits behind the width selector, and lane B compiling them
# IS the guard.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Full source list, same set as test_libatls_rv64.sh.
SRCS="lib/libatls/src/atls_common.c lib/libatls/src/atls_sha256.c
      lib/libatls/src/atls_sha512.c lib/libatls/src/atls_hmac.c
      lib/libatls/src/atls_hkdf.c lib/libatls/src/atls_chacha20.c
      lib/libatls/src/atls_poly1305.c lib/libatls/src/atls_aead.c
      lib/libatls/src/atls_fe.c lib/libatls/src/atls_x25519.c
      lib/libatls/src/atls_ed25519.c lib/libatls/src/atls_der.c
      lib/libatls/src/atls_x509.c lib/libatls/src/atls_tls_keys.c
      lib/libatls/src/atls_tls.c lib/libatls/src/atls_rsa.c
      lib/libatls/src/atls_certval.c lib/libatls/src/atls_ecdsa.c
      lib/libatls/src/atls_pem.c"

# Sources that must never grow a bare __int128 (no width ifdef there).
NO128_SRCS="lib/libatls/src/atls_common.c
            lib/libatls/src/atls_sha256.c
            lib/libatls/src/atls_sha512.c
            lib/libatls/src/atls_hmac.c
            lib/libatls/src/atls_hkdf.c
            lib/libatls/src/atls_chacha20.c
            lib/libatls/src/atls_poly1305.c
            lib/libatls/src/atls_aead.c"

for f in $SRCS; do
    if [ ! -f "$f" ]; then
        echo "[atls-m32] FAIL: $f does not exist (list drifted from LIBATLS_SRCS)"
        exit 1
    fi
done
for f in $NO128_SRCS; do
    if grep -q "__int128" "$f"; then
        echo "[atls-m32] FAIL: $f grew a bare __int128 -- it has no width"
        echo "           ifdef, so the -m32 build just lost it; fix or re-scope."
        exit 1
    fi
done

TESTS="test_atls_hash test_atls_aead test_atls_x25519 test_atls_ed25519 test_atls_ecdsa"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -I lib/libatls/include -I lib/libatls/src -I ."

mkdir -p build

# ---- lane A: forced 32-bit limbs on the native host (always runs) ----
fail=0
for t in $TESTS; do
    if ! cc $CFLAGS -DATLS_FE_FORCE32 $SRCS "tests/unit/$t.c" \
            -o "build/${t}_force32" 2> build/atls_force32_build.log; then
        echo "[atls-m32] FAIL: $t does not compile with ATLS_FE_FORCE32:"
        head -10 build/atls_force32_build.log
        fail=1
        continue
    fi
    if "./build/${t}_force32" > "build/atls_force32_${t}.log" 2>&1; then
        echo "  [atls-m32] OK   $t: 32-bit limb path, native host (FORCE32)"
    else
        echo "  [atls-m32] FAIL $t: 32-bit limb path diverges (FORCE32):"
        tail -10 "build/atls_force32_${t}.log"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "[atls-m32] FAILED (FORCE32 lane)"
    exit 1
fi

# ---- lane B: the real -m32 ABI (when the host toolchain can) ----
if ! printf 'int main(){return 0;}' | cc -m32 -x c - -o build/m32probe 2>/dev/null; then
    echo "[atls-m32] PASS: COMPLETE suite at 32-bit width (FORCE32 lane)"
    echo "[atls-m32] SKIP: no -m32 host toolchain (install gcc-multilib)" \
         "-- the ILP32-ABI half not exercised HERE; CI's x86_64 job runs it"
    exit 0
fi
rm -f build/m32probe

for t in $TESTS; do
    if ! cc -m32 $CFLAGS $SRCS "tests/unit/$t.c" \
            -o "build/${t}_m32" 2> build/atls_m32_build.log; then
        echo "[atls-m32] FAIL: $t does not compile at -m32:"
        head -10 build/atls_m32_build.log
        fail=1
        continue
    fi
    if "./build/${t}_m32" > "build/atls_m32_${t}.log" 2>&1; then
        echo "  [atls-m32] OK   $t: vectors pass EXECUTED at -m32"
    else
        echo "  [atls-m32] FAIL $t: vectors diverge at -m32:"
        tail -10 "build/atls_m32_${t}.log"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "[atls-m32] FAILED (-m32 lane)"
    exit 1
fi

echo "[atls-m32] PASS: the COMPLETE suite (hash/AEAD/X25519/Ed25519/ECDSA)"
echo "           at 32-bit width -- FORCE32 native + real -m32 ABI"
exit 0
