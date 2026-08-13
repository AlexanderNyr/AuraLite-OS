#!/usr/bin/env bash
# test_w32_abi_negctl.sh — negative control for WIN32_PLAN.md phase W32-4.
#
# test_w32_abi passing is only meaningful if it would FAIL when the
# __attribute__((ms_abi)) annotation is missing.  That is the entire point of
# the test: a missing annotation compiles and links cleanly, so nothing else in
# the build would notice.
#
# This rebuilds the same test against a w32_abi.h with W32ABI defined empty and
# asserts the result is a failure.  It follows the pattern the libc drift check
# and tools/check_provenance.sh already use: prove the check can fail before
# trusting that it passed.
#
# Skips cleanly when the compiler cannot build the test at all.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/.."
ROOT="$PWD"

CC="${HOST_CC:-cc}"
SRC="tests/unit/test_w32_abi.c"
HDR="w32/include/w32/w32_abi.h"

if [ ! -f "$SRC" ] || [ ! -f "$HDR" ]; then
    echo "[w32] SKIP: ABI test sources not present"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/w32"

# Same header, with the annotation removed.
sed 's|#    define W32ABI __attribute__((ms_abi))|#    define W32ABI /* removed by the negative control */|' \
    "$HDR" > "$tmp/w32/w32_abi.h"

if ! grep -q 'removed by the negative control' "$tmp/w32/w32_abi.h"; then
    echo "[w32] SKIP: could not patch W32ABI out of $HDR (definition changed?)"
    exit 0
fi

# -I $tmp first so the patched header wins.
if ! $CC -std=c11 -O2 -I "$tmp" -I w32/include "$SRC" -o "$tmp/broken" 2>/dev/null; then
    echo "[w32] SKIP: could not build the un-annotated variant"
    exit 0
fi

echo "[w32] abi negative control: running the ABI test WITHOUT ms_abi..."
if "$tmp/broken" >/dev/null 2>&1; then
    echo "[w32] NEGATIVE CONTROL FAILED: the ABI test passed without ms_abi,"
    echo "      so it would not catch a missing annotation on a real export."
    exit 1
fi

echo "[w32] abi negative control PASS: the test fails without ms_abi, as required"
