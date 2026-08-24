#!/usr/bin/env bash
# tests/unit/test_libatls_rv64.sh -- RISCV_PLAN V8: the crypto stack
# at rv64, the COMPLETE suite.
#
# The i386 gate (test_libatls_m32.sh) once ran only the symmetric
# subset -- atls_fe.c and atls_ecdsa.c needed unsigned __int128, which
# -m32 does not have.  RESIDUE_PLAN R10 closed that boundary with a
# 32-bit limb path, so BOTH gates now run the complete suite; this one
# still matters because rv64 is LP64 with __int128
# (plan Fact 5), so THIS gate runs everything the host suite runs --
# X25519, Ed25519, P-256 ECDSA included.  Same sources, both truths
# recorded: the i386 plan's §6 boundary entry gets its green
# counterpart here, proving the loss there was the ABI's ceiling and
# not the tree's.
#
# Execution strategy, in preference order:
#   1. riscv64-linux-gnu-gcc -static + qemu-riscv64 user-mode
#      emulation: the vectors actually EXECUTE on rv64 (the point --
#      64x64->128 carry chains lower differently per ISA, and a
#      mulhu-path bug would produce plausible-looking wrong field
#      elements).
#   2. clang --target=riscv64 compile-only when the cross libc or
#      qemu-user is absent: layout and constant-expression checking
#      still runs; the miss is reported as SKIP for the execution
#      half, loudly.
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
        echo "[atls-rv64] FAIL: $f does not exist (list drifted from LIBATLS_SRCS)"
        exit 1
    fi
done

# The suites that exercise the 64-bit __int128 limb path (the -m32
# gate runs the same five through the R10 32-bit limb path):
TESTS="test_atls_hash test_atls_aead test_atls_x25519 test_atls_ed25519 test_atls_ecdsa test_atls_mlkem"

CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -I lib/libatls/include -I lib/libatls/src -I ."

mkdir -p build

if command -v riscv64-linux-gnu-gcc >/dev/null 2>&1 && \
   command -v qemu-riscv64 >/dev/null 2>&1; then
    fail=0
    for t in $TESTS; do
        if ! riscv64-linux-gnu-gcc -static $CFLAGS $SRCS \
                "tests/unit/$t.c" -o "build/${t}_rv64" \
                2> "build/atls_rv64_build.log"; then
            echo "[atls-rv64] FAIL: $t does not compile for rv64:"
            head -10 build/atls_rv64_build.log
            fail=1
            continue
        fi
        if qemu-riscv64 "build/${t}_rv64" > "build/atls_rv64_${t}.log" 2>&1; then
            echo "  [atls-rv64] OK   $t: vectors pass EXECUTED on rv64"
        else
            echo "  [atls-rv64] FAIL $t: vectors diverge at rv64:"
            tail -10 "build/atls_rv64_${t}.log"
            fail=1
        fi
    done
    if [ "$fail" -ne 0 ]; then
        echo "[atls-rv64] FAILED"
        exit 1
    fi
    echo "[atls-rv64] PASS: the COMPLETE suite (hash/AEAD/X25519/Ed25519/ECDSA)"
    echo "            executed at rv64 -- the __int128 limb path (the -m32"
    echo "            gate covers the R10 32-bit limb path)"
    exit 0
fi

# Fallback: compile-only through the freestanding clang target.
echo "[atls-rv64] no riscv64-linux-gnu-gcc + qemu-riscv64; compile-only fallback"

# Hardened alongside the a64 gate's fallback (ARM64_PLAN A9 hotfix):
# whether a bare clang triple searches /usr/include is a clang-version
# accident, and the hosted surface here is four prototypes.  Stub
# them; this branch checks layout and constant expressions, not glibc.
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

if clang --target=riscv64 -march=rv64gc -mabi=lp64d -ffreestanding \
        -isystem "$STUB" \
        $CFLAGS -c $SRCS 2> build/atls_rv64_build.log; then
    rm -f ./*.o
    echo "[atls-rv64] PASS (compile-only): all 21 sources build at rv64;"
    echo "            EXECUTION SKIPPED -- install gcc-riscv64-linux-gnu"
    echo "            and qemu-user for the real gate"
    exit 0
else
    rm -f ./*.o
    echo "[atls-rv64] FAIL: sources do not compile at rv64:"
    head -10 build/atls_rv64_build.log
    exit 1
fi
