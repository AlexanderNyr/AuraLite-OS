#!/usr/bin/env bash
# tools/test_sdf_simd.sh — build + run the SSE4.1 vs scalar MSDF guard.
#
# Links the REAL kernel/gui/sdf/sdf.rs (the project's rule: host tests test
# the shipping source, not a copy).  `--cfg test_sdf_host` turns on three
# test hooks that do not exist in the kernel build; the two extern "C"
# blend hooks resolve against an inert stub because no draw entry point is
# reached.  Skips LOUDLY without rustc (D2: a missing toolchain is a skip
# with a sentence, never a silent pass).
set -u
cd "$(dirname "$0")/.."

if ! command -v rustc >/dev/null 2>&1; then
    echo "[sdf-simd] SKIP: rustc absent -- the SSE4.1 guard did not run"
    exit 0
fi

OUT="${1:-build/test_sdf_simd}"
mkdir -p "$(dirname "$OUT")"

mkdir -p build/obj
cc -O2 -c tests/unit/sdf_blend_stub.c -o build/obj/sdf_blend_stub.o || {
    echo "[sdf-simd] FAILED to build the blend stub"; exit 1; }

# rustc takes one input file; the C stub reaches the link through -C linker.
rustc --edition 2021 -O \
      --cfg test_sdf_host \
      --crate-name test_sdf_simd \
      -C linker=cc \
      -C link-arg=build/obj/sdf_blend_stub.o \
      -o "$OUT" \
      tests/unit/test_sdf_simd.rs 2>/tmp/sdf_build.log || {
    echo "[sdf-simd] FAILED to compile"; sed -n 1,30p /tmp/sdf_build.log; exit 1; }

"$OUT"
