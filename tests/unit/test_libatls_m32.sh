#!/usr/bin/env bash
# tests/unit/test_libatls_m32.sh -- I386_PLAN I8: the crypto stack at
# 32-bit width, on the host.
#
# libatls (SHA-256/384, ChaCha20-Poly1305, X25519, Ed25519, P-256) is
# the tree's most width-sensitive portable code: field elements are
# built from 64-bit limb arithmetic with carries that a silent
# truncation would corrupt IN A WAY THAT STILL PRODUCES PLAUSIBLE
# OUTPUT.  Running the full RFC-vector suite compiled -m32 catches
# that class before any i386 TLS work exists to consume it -- and it
# needs no VM in the loop.
#
# Builds tests/unit/test_atls_hash.c (RFC 6234/8439 vectors and the
# suite's own negative controls) at -m32 against the SYMMETRIC subset
# of libatls -- everything test_atls_hash actually calls.
#
# WHAT IS DELIBERATELY EXCLUDED, AND WHY IT IS RECORDED LOUDLY:
# atls_fe.c (the 51-bit-limb field arithmetic under X25519/Ed25519)
# and atls_ecdsa.c use unsigned __int128, which does not exist at
# -m32.  That is a REAL 32-bit portability boundary this gate just
# measured, not an inconvenience to hide: X25519/Ed25519/P-256 cannot
# run on the i386 kernel until someone writes the 25.5-limb (or
# 2^25.5 radix) 32-bit reduction path.  I386_PLAN §6 carries the
# entry; the day TLS work starts on i386, THIS comment is the map.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# The symmetric subset: sources without __int128.  The guard below
# fails this test the day one of them grows a __int128 -- widening
# the excluded set silently is exactly the drift this file exists to
# prevent.
M32_SRCS="lib/libatls/src/atls_common.c
          lib/libatls/src/atls_sha256.c
          lib/libatls/src/atls_sha512.c
          lib/libatls/src/atls_hmac.c
          lib/libatls/src/atls_hkdf.c
          lib/libatls/src/atls_chacha20.c
          lib/libatls/src/atls_poly1305.c
          lib/libatls/src/atls_aead.c"

for f in $M32_SRCS; do
    if [ ! -f "$f" ]; then
        echo "[atls-m32] FAIL: $f does not exist"
        exit 1
    fi
    if grep -q "__int128" "$f"; then
        echo "[atls-m32] FAIL: $f grew a __int128 -- the -m32 symmetric"
        echo "           suite no longer covers it; fix or re-scope."
        exit 1
    fi
done

# -m32 available?
if ! printf 'int main(){return 0;}' | cc -m32 -x c - -o build/m32probe 2>/dev/null; then
    echo "[atls-m32] SKIP: no -m32 host toolchain (install gcc-multilib)"
    exit 0
fi
rm -f build/m32probe

mkdir -p build
if ! cc -m32 -std=c11 -Wall -Wextra -Werror -O2 -I lib/libatls/include \
        $M32_SRCS tests/unit/test_atls_hash.c \
        -o build/test_atls_hash_m32 2> build/atls_m32_build.log; then
    echo "[atls-m32] FAIL: does not compile at -m32:"
    head -20 build/atls_m32_build.log
    exit 1
fi

if ./build/test_atls_hash_m32 > build/atls_m32_run.log 2>&1; then
    echo "[atls-m32] PASS: full hash/AEAD vector suite at 32-bit width"
    exit 0
else
    echo "[atls-m32] FAIL: vectors diverge at 32-bit width:"
    tail -20 build/atls_m32_run.log
    exit 1
fi
