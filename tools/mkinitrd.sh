#!/usr/bin/env bash
# mkinitrd.sh — create a USTAR (POSIX tar) initrd image from a directory.
#
# Usage: mkinitrd.sh <input_dir> <output.tar>
#
# Packs the given directory's files into a tarball that the kernel's initrd
# parser can read. Used by the Makefile to embed userspace binaries.
set -euo pipefail

INPUT_DIR="${1:?usage: $0 <input_dir> <output.tar>}"
OUTPUT="${2:?}"

# Create the initrd directory with the files we want in the rootfs.
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

# Copy all files from the input directory (flat — no subdirectories yet).
if [ -d "$INPUT_DIR" ]; then
    cp -v "$INPUT_DIR"/* "$STAGING/" 2>/dev/null || true
fi

# Create the USTAR archive (no compression, POSIX format).
tar --format=ustar -cf "$OUTPUT" -C "$STAGING" .

echo "[mkinitrd] wrote $OUTPUT ($(du -h "$OUTPUT" | cut -f1), $(find "$STAGING" -type f | wc -l) files)"
