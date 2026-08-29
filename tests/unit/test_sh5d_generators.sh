#!/usr/bin/env bash
# test_sh5d_generators.sh -- C header emitters used by the SH5d guest build.
#
# These tools replaced the Makefile's Python-only generators.  Test both the
# stdout compatibility path and the explicit output-file path the current
# AuraLite shell needs (it has no redirection syntax), plus a small byte-array
# fixture that makes the generated C content inspectable.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# build/ is .gitignore'd, so it does not exist on a fresh clone -- and this
# case is wired into `make test-unit`, which a contributor may well run before
# ever running `make iso`.  mktemp -d into a missing parent used to leave TMP
# empty, which turned `-o "$TMP/gen_asm_offsets"` into `-o /gen_asm_offsets`
# and failed the unit suite on a clean tree.
mkdir -p "$ROOT/build"
TMP=$(mktemp -d "$ROOT/build/sh5d-generators.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
FAILED=0
pass(){ echo "PASS: $*"; }
fail(){ echo "FAIL: $*"; FAILED=1; }

cc -std=c11 -Wall -Wextra -Werror -O2 -I . tools/gen_asm_offsets.c \
    -o "$TMP/gen_asm_offsets" || exit 1
cc -std=c11 -Wall -Wextra -Werror -O2 tools/gen_user_binary.c \
    -o "$TMP/gen_user_binary" || exit 1
cc -std=c11 -Wall -Wextra -Werror -O2 tools/gen_ap_trampoline_inc.c \
    -o "$TMP/gen_ap_trampoline_inc" || exit 1
pass "all three SH5d generators compile as portable host C"

"$TMP/gen_asm_offsets" > "$TMP/offsets.stdout"
"$TMP/gen_asm_offsets" "$TMP/offsets.file"
if cmp -s "$TMP/offsets.stdout" "$TMP/offsets.file" && \
   grep -q '^%define TCB_RSP' "$TMP/offsets.file" && \
   grep -q '^%define CL_SYS_R15' "$TMP/offsets.file"; then
    pass "asm-offset generator preserves stdout and supports output path"
else
    fail "asm-offset output-path mode differs from stdout or lost a define"
fi

printf '\000\001\177\200\377' > "$TMP/input.bin"
"$TMP/gen_user_binary" "$TMP/input.bin" "$TMP/init_bin.h" init_bin
if grep -Fq 'const unsigned char init_bin[5]' "$TMP/init_bin.h" && \
   grep -Fq '0x00, 0x01, 0x7f, 0x80, 0xff,' "$TMP/init_bin.h" && \
   grep -Fq 'const unsigned long init_bin_size = 5;' "$TMP/init_bin.h"; then
    pass "ELF-to-C generator emits the input bytes and size symbol"
else
    fail "ELF-to-C generator output is incomplete"
fi

"$TMP/gen_ap_trampoline_inc" "$TMP/input.bin" > "$TMP/ap.stdout"
"$TMP/gen_ap_trampoline_inc" "$TMP/input.bin" "$TMP/ap.file"
if cmp -s "$TMP/ap.stdout" "$TMP/ap.file" && \
   grep -Fq '#define AP_TRAMPOLINE_SIZE 5' "$TMP/ap.file" && \
   grep -Fq '0x00, 0x01, 0x7f, 0x80, 0xff,' "$TMP/ap.file"; then
    pass "AP-trampoline generator preserves stdout and supports output path"
else
    fail "AP-trampoline output-path mode differs from stdout or lost bytes"
fi

# The normal build must call the C tools, not sneak the old Python commands
# back into generated-header recipes.  Scan commands, not comments elsewhere
# in the Makefile (the plan still legitimately documents the historical tools).
if ! grep -E '^[[:space:]]*[^#].*python3 .*gen_(user_binary|ap_trampoline_inc)\.py' Makefile >/dev/null; then
    pass "Makefile generated-header rules are Python-free"
else
    fail "Makefile still invokes a Python header generator"
fi

if [ "$FAILED" -eq 0 ]; then
    echo '[selfhost] sh5d generators PASS: C emitters support in-guest output paths'
    exit 0
fi
exit 1
