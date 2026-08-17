#!/usr/bin/env bash
# mkinitrd.sh — create a USTAR (POSIX tar) initrd image from a directory.
#
# Usage: mkinitrd.sh <input_dir> <output.tar>
#
# Packs the given directory tree into a tarball that the kernel's initrd
# parser can read. Used by the Makefile to embed userspace binaries.
#
# Subdirectories are preserved: a file staged at <input_dir>/apps/probe is
# packed with the USTAR name "./apps/probe" and resolves in the kernel as
# "/apps/probe".  The 100-byte USTAR name field is the limit on how deep a
# path may be; we check it here rather than letting the kernel silently
# truncate.
set -euo pipefail

INPUT_DIR="${1:?usage: $0 <input_dir> <output.tar>}"
OUTPUT="${2:?}"

# Create the initrd directory with the files we want in the rootfs.
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

# Copy the input tree, preserving subdirectories.
if [ -d "$INPUT_DIR" ]; then
    # The trailing "/." copies the *contents* of INPUT_DIR, not the directory
    # itself, which is what the previous flat "cp $INPUT_DIR/*" did.
    cp -a "$INPUT_DIR/." "$STAGING/"
fi

# USTAR stores the path in a 100-byte field.  With the "./" prefix tar adds,
# anything at or beyond 100 characters would be truncated into a wrong (or
# colliding) name.  Fail loudly instead.
while IFS= read -r -d '' f; do
    rel="./${f#"$STAGING"/}"
    if [ "${#rel}" -ge 100 ]; then
        echo "[mkinitrd] ERROR: path too long for USTAR (${#rel} >= 100): $rel" >&2
        exit 1
    fi
done < <(find "$STAGING" -type f -print0)

# Create the USTAR archive (no compression, POSIX format).
#
# ORDER MATTERS, AND NOT FOR THE REASON YOU WOULD GUESS.
#
# When two names are hard links to the same inode, tar writes whichever it
# reaches FIRST as the real entry and the other as a type-'1' link pointing
# back at it.  Under a plain alphabetical sort "./apm" comes before
# "./bin/apm", so the root-level compatibility alias became the real file and
# the canonical location became the link — exactly backwards.  That is
# harmless today, because both names resolve, and a trap for phase F5: drop
# the aliases and every canonical path turns into a dangling link to a file
# that is no longer in the archive.
#
# So: nested paths are archived before root-level ones, which makes the
# canonical location the real entry and the alias the link.  Within each
# group the order is alphabetical, so the archive stays reproducible.
FILELIST="$(mktemp)"
trap 'rm -rf "$STAGING" "$FILELIST"' EXIT
(
    cd "$STAGING"
    find . -mindepth 2 | LC_ALL=C sort      # subdirectories and their contents
    find . -maxdepth 1 | LC_ALL=C sort      # the root and what sits directly in it
) > "$FILELIST"

tar --format=ustar --no-recursion -cf "$OUTPUT" -C "$STAGING" -T "$FILELIST"

# RISCV_PLAN V8: the three-tenant audit.  One tar serves three
# kernels -- /bin (x86_64 ELF64), /bin32 (i386 ELF32), /binrv (rv64
# ELF64) -- and each kernel's loader refuses the other two's
# binaries.  The audit here is the packaging half of that contract:
# when a tenant directory is present, its ELF machine type must match
# its name (a cross-copied binary would boot-loop as a refusal at
# runtime; catching it at pack time names the guilty file instead).
# e_machine: offset 18, little-endian; 62=x86_64, 3=i386, 243=riscv.
audit_tenant() {
    local dir="$1" want="$2" name="$3"
    [ -d "$STAGING/$dir" ] || return 0
    local f m
    for f in "$STAGING/$dir"/*; do
        [ -f "$f" ] || continue
        # Only ELF files are audited (etc/motd-style strays are fine).
        if [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' ')" != "7f454c46" ]; then
            continue
        fi
        m=$(od -An -j18 -N2 -tu2 "$f" | tr -d ' ')
        if [ "$m" != "$want" ]; then
            echo "[mkinitrd] ERROR: $dir/$(basename "$f") has e_machine=$m," >&2
            echo "           expected $want ($name) -- a cross-arch binary" >&2
            echo "           in the wrong tenant directory" >&2
            exit 1
        fi
    done
}
audit_tenant bin   62  x86_64
audit_tenant bin32 3   i386
audit_tenant binrv 243 riscv64

echo "[mkinitrd] wrote $OUTPUT ($(du -h "$OUTPUT" | cut -f1), $(find "$STAGING" -type f | wc -l) files, $(find "$STAGING" -mindepth 1 -type d | wc -l) subdirectories)"
