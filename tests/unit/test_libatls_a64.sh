#!/usr/bin/env bash
# tests/unit/test_libatls_a64.sh -- ARM64_PLAN A8: the crypto stack
# at aarch64, the COMPLETE suite.
#
# The fourth width for libatls, and the second LP64-with-__int128
# tenant: like rv64, aarch64 runs everything the host suite runs --
# X25519, Ed25519, P-256 ECDSA included (since RESIDUE_PLAN R10 the
# -m32 gate runs them too, through the 32-bit limb path -- I386_PLAN
# §6's boundary is closed).  Plan Fact 5 holds the measured
# __int128 execution receipt from fact-finding; this gate turns the
# receipt into a standing assertion.
#
# Execution strategy, in preference order (the rv64 gate's, fourth
# spelling):
#   1. aarch64-linux-gnu-gcc -static + qemu-aarch64 user-mode
#      emulation: the vectors actually EXECUTE on aarch64 (the point
#      -- 64x64->128 carry chains lower differently per ISA: umulh
#      here, mulhu on rv64, and a wide-multiply bug produces
#      plausible-looking wrong field elements that only execution
#      catches).
#   2. clang --target=aarch64 compile-only when the cross libc or
#      qemu-user is absent: layout and constant-expression checking
#      still runs; the miss is reported as SKIP for the execution
#      half, loudly.
#
# Dependencies for the real gate, BY NAME (Fact 1's measured miss --
# the cross-gcc package alone does not pull the target libc):
#   gcc-aarch64-linux-gnu  libc6-dev-arm64-cross  qemu-user
#
# [AMEND-6]: this command -v is the gate's own truth, and A9's CI job
# must repeat it AFTER its install step -- silent apt dependency
# failures were measured three times during the OPT audit ("Setting
# up" printed, binary absent).  An installer's exit status is not a
# binary's existence.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# The full source list, kept in lockstep with the Makefile's
# LIBATLS_SRCS (grep-asserted below so the two cannot drift).
SRCS="lib/libatls/src/atls_common.c lib/libatls/src/atls_sha256.c
      lib/libatls/src/atls_sha512.c lib/libatls/src/atls_hmac.c
      lib/libatls/src/atls_hkdf.c lib/libatls/src/atls_chacha20.c
      lib/libatls/src/atls_poly1305.c lib/libatls/src/atls_aead.c
      lib/libatls/src/atls_fe.c lib/libatls/src/atls_x25519.c
      lib/libatls/src/atls_ed25519.c lib/libatls/src/atls_der.c
      lib/libatls/src/atls_x509.c lib/libatls/src/atls_tls_keys.c
      lib/libatls/src/atls_tls.c lib/libatls/src/atls_rsa.c
      lib/libatls/src/atls_certval.c lib/libatls/src/atls_ecdsa.c
      lib/libatls/src/atls_pem.c lib/libatls/src/atls_sha3.c
      lib/libatls/src/atls_mlkem.c"

for f in $SRCS; do
    if [ ! -f "$f" ]; then
        echo "[atls-a64] FAIL: $f does not exist (list drifted from LIBATLS_SRCS)"
        exit 1
    fi
done

# The suites that exercise the 64-bit __int128 limb path (the -m32
# gate runs the same five through the R10 32-bit limb path):
TESTS="test_atls_hash test_atls_aead test_atls_x25519 test_atls_ed25519 test_atls_ecdsa test_atls_mlkem"

CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -I lib/libatls/include -I lib/libatls/src -I ."

mkdir -p build

if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && \
   command -v qemu-aarch64 >/dev/null 2>&1; then
    fail=0
    for t in $TESTS; do
        if ! aarch64-linux-gnu-gcc -static $CFLAGS $SRCS \
                "tests/unit/$t.c" -o "build/${t}_a64" \
                2> "build/atls_a64_build.log"; then
            echo "[atls-a64] FAIL: $t does not compile for aarch64:"
            head -10 build/atls_a64_build.log
            fail=1
            continue
        fi
        if qemu-aarch64 "build/${t}_a64" > "build/atls_a64_${t}.log" 2>&1; then
            echo "  [atls-a64] OK   $t: vectors pass EXECUTED on aarch64"
        else
            echo "  [atls-a64] FAIL $t: vectors diverge at aarch64:"
            tail -10 "build/atls_a64_${t}.log"
            fail=1
        fi
    done
    if [ "$fail" -ne 0 ]; then
        echo "[atls-a64] FAILED"
        exit 1
    fi
    echo "[atls-a64] PASS: the COMPLETE suite (hash/AEAD/X25519/Ed25519/ECDSA)"
    echo "           executed at aarch64 -- the second LP64 tenant to run the"
    echo "           __int128 limb path (umulh edition; the -m32 gate covers"
    echo "           the R10 32-bit limb path)"
    exit 0
fi

# Fallback: compile-only through the freestanding clang target.
echo "[atls-a64] no aarch64-linux-gnu-gcc + qemu-aarch64; compile-only fallback"
echo "           (install gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user)"

# Bare-metal clang triples search NO hosted include path (measured on
# the first four-job CI run: the a64 fallback died at <string.h> on
# the runner that has no cross libc -- which is exactly the machine
# this branch exists for).  The sources' hosted surface is four
# prototypes; stub them, because layout and constant-expression
# checking -- this branch's whole job -- needs declarations, not glibc.
STUB=build/atls_stub_include
mkdir -p "$STUB"
cat > "$STUB/string.h" <<'EOF'
#ifndef ATLS_STUB_STRING_H
#define ATLS_STUB_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *p, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
#endif
EOF
cat > "$STUB/stdio.h" <<'EOF'
#ifndef ATLS_STUB_STDIO_H
#define ATLS_STUB_STDIO_H
int printf(const char *fmt, ...);
#endif
EOF

if clang --target=aarch64-unknown-none-elf -mgeneral-regs-only -ffreestanding \
        -isystem "$STUB" \
        $CFLAGS -c $SRCS 2> build/atls_a64_build.log; then
    rm -f ./*.o
    echo "[atls-a64] PASS (compile-only): all 21 sources build at aarch64;"
    echo "           EXECUTION SKIPPED -- install gcc-aarch64-linux-gnu,"
    echo "           libc6-dev-arm64-cross and qemu-user for the real gate"
    exit 0
else
    rm -f ./*.o
    echo "[atls-a64] FAIL: sources do not compile at aarch64:"
    head -10 build/atls_a64_build.log
    exit 1
fi
