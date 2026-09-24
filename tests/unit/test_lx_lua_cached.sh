#!/usr/bin/env bash
# Offline LX builds must use an already-built Lua or a checksum-verified
# cached source archive; neither path may be replaced by a zero-byte stub.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p build
TMP=$(mktemp -d "$ROOT/build/lua-cache.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/stubs" "$TMP/archive/lua-5.4.9/src" "$TMP/lua-src"
for tool in curl wget; do
    cat > "$TMP/stubs/$tool" <<'STUB'
#!/bin/sh
printf 'unexpected network request\n' >> "$FETCH_MARKER"
exit 1
STUB
    chmod +x "$TMP/stubs/$tool"
done
export FETCH_MARKER="$TMP/fetched"

# A precompiled Lua must not be discarded just because the tarball is absent.
printf 'already-built Lua\n' > "$TMP/lua"
cp "$TMP/lua" "$TMP/expected"
PATH="$TMP/stubs:$PATH" make -s --no-print-directory "$TMP/lua" \
    LX_LUA_BIN="$TMP/lua" LX_LUA_STAMP="$TMP/prebuilt-ok" \
    LX_LUA_DIR="$TMP/lua-src" LX_LUA_TGZ="$TMP/missing.tar.gz"
cmp "$TMP/lua" "$TMP/expected"
test -f "$TMP/prebuilt-ok" && test ! -e "$FETCH_MARKER"
echo 'PASS: precompiled Lua survives offline stamp generation'

# A verified cache should be used without even trying curl/wget.  Supply a
# tiny fake Lua makefile so this unit checks the recipe without a network or
# cross-project compilation; override its digest to the fixture's hash.
printf 'linux:\n\t@printf "cached Lua\\n" > src/lua\n' > "$TMP/archive/lua-5.4.9/Makefile"
tar -czf "$TMP/cached.tar.gz" -C "$TMP/archive" lua-5.4.9
sha=$(sha256sum "$TMP/cached.tar.gz" | cut -d ' ' -f 1)
PATH="$TMP/stubs:$PATH" make -s --no-print-directory "$TMP/cache-lua" \
    LX_LUA_BIN="$TMP/cache-lua" LX_LUA_STAMP="$TMP/cached-ok" \
    LX_LUA_DIR="$TMP/lua-src" LX_LUA_TGZ="$TMP/cached.tar.gz" \
    LX_LUA_SHA="$sha"
test "$(cat "$TMP/cache-lua")" = 'cached Lua'
test -f "$TMP/cached-ok" && test ! -e "$FETCH_MARKER"
echo 'PASS: verified cached Lua archive builds without network access'
