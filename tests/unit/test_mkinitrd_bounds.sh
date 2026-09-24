#!/usr/bin/env bash
# The host packer must never ship an archive larger than kernel/fs/initrd.h's
# file/dir tables.  Hard links consume a separate file slot in the parser.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p build
TMP=$(mktemp -d "$ROOT/build/mkinitrd-bounds.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

limit() {
    awk -v key="$1" '$1 == "#define" && $2 == key { print $3 }' kernel/fs/initrd.h
}
files=$(limit INITRD_MAX_FILES)
dirs=$(limit INITRD_MAX_DIRS)
[[ "$files" =~ ^[0-9]+$ && "$dirs" =~ ^[0-9]+$ ]]

mkdir -p "$TMP/files"
for ((i=0; i<files; i++)); do
    printf -v name 'file%04d' "$i"
    : > "$TMP/files/$name"
done
bash tools/mkinitrd.sh "$TMP/files" "$TMP/files.tar" > "$TMP/packer.log"
test "$(tar tf "$TMP/files.tar" | grep -c '^./file')" -eq "$files"
echo "PASS: host packer accepts exactly $files files"

# One more path to an existing inode must count as an extra tar link entry.
ln "$TMP/files/file0000" "$TMP/files/alias"
printf 'stale archive\n' > "$TMP/overflow.tar"
if bash tools/mkinitrd.sh "$TMP/files" "$TMP/overflow.tar" > "$TMP/packer.log" 2>&1; then
    echo 'FAIL: host packer accepted an over-limit hard link' >&2
    exit 1
fi
grep -q "initrd exceeds kernel tables: $((files + 1))/$files files" "$TMP/packer.log"
test ! -e "$TMP/overflow.tar"
echo 'PASS: extra hard link is rejected and stale output removed'

mkdir -p "$TMP/dirs"
for ((i=1; i<dirs; i++)); do
    printf -v name 'dir%03d' "$i"
    mkdir "$TMP/dirs/$name"
done
bash tools/mkinitrd.sh "$TMP/dirs" "$TMP/dirs.tar" > "$TMP/packer.log"
test "$(tar tf "$TMP/dirs.tar" | grep -c '^./dir')" -eq "$((dirs - 1))"
echo "PASS: host packer accepts exactly $dirs directories including root"
mkdir "$TMP/dirs/extra"
if bash tools/mkinitrd.sh "$TMP/dirs" "$TMP/overflow.tar" > "$TMP/packer.log" 2>&1; then
    echo 'FAIL: host packer accepted an over-limit directory' >&2
    exit 1
fi
grep -q "$((dirs + 1))/$dirs directories" "$TMP/packer.log"
test ! -e "$TMP/overflow.tar"
echo 'PASS: extra directory is rejected before creating an archive'
