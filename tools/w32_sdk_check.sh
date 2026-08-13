#!/usr/bin/env bash
# tools/w32_sdk_check.sh — build the examples AGAINST THE STAGED SDK.
#
# Mirrors tools/sdk_check.sh.  The examples are copied out of the staged SDK
# into a scratch directory and built there, so nothing can reach back into
# the source tree: if it could, they would keep building after the SDK
# stopped being sufficient and this check would prove nothing.
set -uo pipefail
DIR="${1:?usage: w32_sdk_check.sh <w32-sdk-dir>}"
DIR="$(cd "$DIR" && pwd)"

pass=0
fail=0

check() {
    if [ "$1" = "0" ]; then
        echo "  PASS: $2"; pass=$((pass + 1))
    else
        echo "  FAIL: $2"; fail=$((fail + 1))
    fi
}

echo "[w32-sdk-check] checking $DIR ..."

for f in w32.mk README.md win32.md lib/kernel32.lib lib/user32.lib \
         lib/gdi32.lib; do
    [ -e "$DIR/$f" ]; check $? "$f is staged"
done

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    # Not an error: the OS build must not require the cross-compiler.  But
    # say so plainly rather than reporting a pass that did not happen.
    echo "  SKIP: x86_64-w64-mingw32-gcc not installed; examples not built"
    echo "[w32-sdk-check] $pass passed, $fail failed, examples skipped"
    [ "$fail" -eq 0 ] || exit 1
    exit 0
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
cp -r "$DIR/examples/." "$SCRATCH/"

for ex in console-app gui-app unsupported-app; do
    [ -d "$SCRATCH/$ex" ] || { check 1 "$ex is staged"; continue; }
    (cd "$SCRATCH/$ex" && make clean >/dev/null 2>&1; \
     cd "$SCRATCH/$ex" && make >/dev/null 2>&1)
    check $? "$ex builds against the staged SDK"

    # A built example must actually be a PE32+ image, not an empty file a
    # silent failure left behind.
    exe="$(find "$SCRATCH/$ex" -maxdepth 1 -name '*.exe' | head -1)"
    if [ -n "$exe" ] && [ -s "$exe" ]; then
        head -c2 "$exe" | grep -q "MZ"
        check $? "$ex produced a PE image"
    else
        check 1 "$ex produced a PE image"
    fi
done

echo "[w32-sdk-check] $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
