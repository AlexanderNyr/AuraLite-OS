#!/usr/bin/env bash
# test_w32_peinfo.sh — WIN32_PLAN.md phase W32-2 cross-check gate.
#
# The plan requires that `peinfo` agree with an independent PE reader on every
# field it reports, using the project's own BOOTX64.EFI as the fixture.  This
# script is the mechanical form of that requirement: it parses both outputs and
# compares them, rather than eyeballing them once.
#
# It skips cleanly (exit 0) when the EFI binary has not been built or when no
# reference reader is installed, following the convention of
# tests/unit/test_userlibs.sh.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EFI="build/boot/BOOTX64.EFI"
PEINFO="build/w32_peinfo"

if [ ! -f "$EFI" ]; then
    echo "[w32] SKIP: $EFI not built"
    exit 0
fi
if [ ! -x "$PEINFO" ]; then
    echo "[w32] SKIP: $PEINFO not built"
    exit 0
fi

# Find a reference reader.  llvm-readobj is preferred; any versioned binary
# will do.
READOBJ=""
for c in llvm-readobj llvm-readobj-19 llvm-readobj-18 llvm-readobj-17; do
    if command -v "$c" >/dev/null 2>&1; then READOBJ="$c"; break; fi
done
if [ -z "$READOBJ" ]; then
    echo "[w32] SKIP: no llvm-readobj available for cross-check"
    exit 0
fi

ours="$("$PEINFO" "$EFI")"
ref="$("$READOBJ" --file-headers "$EFI")"

fail=0
check() {           # check <label> <ours> <reference>
    if [ "$2" = "$3" ]; then
        echo "  ok   $1: $2"
    else
        echo "  FAIL $1: peinfo='$2' readobj='$3'"
        fail=1
    fi
}

# peinfo prints hex; llvm-readobj mixes hex and decimal.  Normalise to decimal.
ours_field() { echo "$ours" | grep -m1 "^$1:" | awk '{print $2}'; }
to_dec() { printf '%d\n' "$1" 2>/dev/null || echo "?"; }

o_machine=$(to_dec "$(ours_field machine)")
r_machine=$(to_dec "$(echo "$ref" | grep -m1 'Machine:' | grep -o '0x[0-9A-Fa-f]*')")
check machine "$o_machine" "$r_machine"

o_sections=$(ours_field sections)
r_sections=$(echo "$ref" | grep -m1 'SectionCount:' | awk '{print $2}')
check section_count "$o_sections" "$r_sections"

o_entry=$(to_dec "$(ours_field entry_point_rva)")
r_entry=$(to_dec "$(echo "$ref" | grep -m1 'AddressOfEntryPoint:' | awk '{print $2}')")
check entry_point "$o_entry" "$r_entry"

o_base=$(to_dec "$(ours_field image_base)")
r_base=$(to_dec "$(echo "$ref" | grep -m1 'ImageBase:' | awk '{print $2}')")
check image_base "$o_base" "$r_base"

o_simg=$(to_dec "$(ours_field size_of_image)")
r_simg=$(echo "$ref" | grep -m1 'SizeOfImage:' | awk '{print $2}')
check size_of_image "$o_simg" "$r_simg"

o_shdr=$(to_dec "$(ours_field size_of_headers)")
r_shdr=$(echo "$ref" | grep -m1 'SizeOfHeaders:' | awk '{print $2}')
check size_of_headers "$o_shdr" "$r_shdr"

o_salign=$(to_dec "$(ours_field section_align)")
r_salign=$(echo "$ref" | grep -m1 'SectionAlignment:' | awk '{print $2}')
check section_alignment "$o_salign" "$r_salign"

o_falign=$(to_dec "$(ours_field file_align)")
r_falign=$(echo "$ref" | grep -m1 'FileAlignment:' | awk '{print $2}')
check file_alignment "$o_falign" "$r_falign"

o_sub=$(echo "$ours" | grep -m1 '^subsystem:' | awk '{print $2}')
r_sub=$(to_dec "$(echo "$ref" | grep -m1 'Subsystem:' | grep -o '0x[0-9A-Fa-f]*')")
check subsystem "$o_sub" "$r_sub"

# The property the whole exercise exists to prove: a firmware image parses
# correctly and is still refused as a user process.
if echo "$ours" | grep -q '^loadable as w32:  unsupported'; then
    echo "  ok   EFI image refused as a w32 process"
else
    echo "  FAIL EFI image was not refused: $(echo "$ours" | grep '^loadable')"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "[w32] test_w32_peinfo: FAIL"
    exit 1
fi
echo "[w32] test_w32_peinfo: PASS (cross-checked against $READOBJ)"
