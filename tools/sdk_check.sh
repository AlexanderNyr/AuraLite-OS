#!/usr/bin/env bash
# sdk_check.sh — prove the staged SDK is sufficient and honest.
#
# Usage: sdk_check.sh <sdk_dir>
#
# Two different things are checked, and both matter:
#
#  1. COMPLETENESS — the examples build and link using ONLY the staged SDK.
#     The build runs in a temporary directory with no access to the OS source
#     tree, so an example cannot accidentally reach into lib/libc/include and keep
#     working after the SDK stopped being sufficient.
#
#  2. HONESTY — the flags in auralite.mk match the ones the OS builds its own
#     programs with. An SDK that compiles with different flags than the system
#     is a trap: the application builds, and then behaves differently for
#     reasons nobody can see.
set -euo pipefail

# NOTE ON `set -o pipefail` AND `grep -q`
#
# `grep -q` exits as soon as it matches, which closes the pipe and kills the
# writer with SIGPIPE.  Under `pipefail` the whole pipeline then reports
# failure EVEN THOUGH THE MATCH SUCCEEDED.  That cost real debugging time
# here: `nm ... | grep -qw _start` reported "no _start" for binaries that
# plainly had one.  Every such test below counts matches instead.

SDK="${1:?usage: $0 <sdk_dir>}"
SDK="$(cd "$SDK" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  PASS: %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL: %s\n' "$1"; }

echo "sdk_check: the staged SDK at $SDK"

# ---- 0. the SDK is not stale ------------------------------------------------
#
# `make sdk` removes and rebuilds the tree, so a failed regeneration leaves the
# PREVIOUS SDK in place.  Checking that one proves nothing: the first attempt
# at this check deleted a header, watched make fail, and then happily passed
# against the stale copy that was still on disk.  Compare the staged headers
# against the sources they are assembled from.
missing_hdr=0
while IFS= read -r -d '' src; do
    rel="${src#"$ROOT"/lib/libc/include/}"
    [ -e "$SDK/include/$rel" ] || { bad "stale SDK: include/$rel is not staged"; missing_hdr=1; }
done < <(find "$ROOT/lib/libc/include" -name '*.h' -print0)

extra_hdr=0
while IFS= read -r -d '' staged; do
    rel="${staged#"$SDK"/include/}"
    case "$rel" in
        auragui.h|GL/*|atls/*) continue ;;   # staged from libauragui / libgl / libatls
    esac
    [ -e "$ROOT/lib/libc/include/$rel" ] || { bad "stale SDK: include/$rel has no source"; extra_hdr=1; }
done < <(find "$SDK/include" -name '*.h' -print0)

[ "$missing_hdr" -eq 0 ] && [ "$extra_hdr" -eq 0 ] &&     ok "staged headers match lib/libc/include exactly (SDK is not stale)"

# ---- 1. the layout ----------------------------------------------------------

for f in auralite.mk user.ld README.md \
         lib/libaurac.a lib/libauragui.a lib/libaGL.a lib/libatls.a lib/crt0.o \
         include/stdio.h include/unistd.h include/auragui.h include/GL/gl.h \
         include/atls/atls.h include/atls/x509.h; do
    if [ -e "$SDK/$f" ]; then ok "$f present"; else bad "$f missing"; fi
done

# ---- 2. the flags match the OS build ----------------------------------------
#
# Compare against the Makefile rather than trusting the copy. These are the
# flags that change behaviour rather than just diagnostics; -Werror is
# deliberately NOT required of third-party code, and neither are the OS's
# own -I paths.

sdk_cflags="$(grep -A4 '^AURALITE_CFLAGS' "$SDK/auralite.mk" | tr '\n' ' ')"
for flag in -ffreestanding -fno-stack-protector -fno-pie -fno-pic; do
    if printf '%s' "$sdk_cflags" | grep -q -- "$flag"; then
        ok "auralite.mk carries $flag"
    else
        bad "auralite.mk is missing $flag (the OS builds with it)"
    fi
done

sdk_ldflags="$(grep -A2 '^AURALITE_LDFLAGS' "$SDK/auralite.mk" | tr '\n' ' ')"
for flag in -nostdlib -static max-page-size=4096; do
    if printf '%s' "$sdk_ldflags" | grep -q -- "$flag"; then
        ok "auralite.mk carries $flag"
    else
        bad "auralite.mk is missing $flag"
    fi
done

# The load address in the SDK must be the one in the linker script it ships.
ld_base="$(grep -oE 'USER_BASE[[:space:]]*=[[:space:]]*0x[0-9A-Fa-f]+' "$SDK/user.ld" \
           | grep -oE '0x[0-9A-Fa-f]+' || true)"
mk_base="$(grep -oE 'AURALITE_LOAD_ADDR[[:space:]]*:=[[:space:]]*0x[0-9A-Fa-f]+' "$SDK/auralite.mk" \
           | grep -oE '0x[0-9A-Fa-f]+' || true)"
if [ -n "$ld_base" ] && [ "$ld_base" = "$mk_base" ]; then
    ok "documented load address $mk_base matches user.ld"
else
    bad "load address mismatch: user.ld says '$ld_base', auralite.mk says '$mk_base'"
fi

# crt0.o must not be inside the archive — see auralite.mk for why.
if ar t "$SDK/lib/libaurac.a" | grep -qx 'crt0.o'; then
    bad "crt0.o is inside libaurac.a; programs would link with no entry point"
else
    ok "crt0.o is shipped as a standalone object"
fi

# ---- 3. the examples build against the staged SDK ONLY ----------------------

if [ ! -d "$ROOT/examples" ]; then
    echo "  SKIP: no examples/ directory yet (added in SDK_PLAN phase S2)"
    echo "  $pass passed, $fail failed"
    [ "$fail" -eq 0 ]
    exit $?
fi

command -v ld.lld >/dev/null 2>&1 || { echo "  SKIP: ld.lld unavailable"; exit 0; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for ex in "$ROOT"/examples/*/; do
    name="$(basename "$ex")"
    [ -f "$ex/Makefile" ] || continue

    # Copy the example somewhere with no path back into the OS tree.
    #
    # Copy SOURCES ONLY.  A plain `cp -r` also brings any .o and .elf left by
    # a previous local build, make then says "nothing to be done", and the
    # check inspects a stale artefact that the staged SDK never produced --
    # which is exactly the kind of false pass this script exists to prevent.
    mkdir -p "$WORK/$name"
    cp "$ex"/Makefile "$ex"/*.c "$WORK/$name/"

    if make -C "$WORK/$name" AURALITE_SDK="$SDK" >"$WORK/$name.log" 2>&1; then
        ok "example '$name' builds against the staged SDK"
    else
        bad "example '$name' failed to build"
        sed 's/^/      /' "$WORK/$name.log" | head -20
        continue
    fi

    elf="$(find "$WORK/$name" -maxdepth 1 -name '*.elf' | head -1)"
    if [ -z "$elf" ]; then
        bad "example '$name' produced no .elf"
        continue
    fi

    # A program with no entry point links cleanly and fails at run time, so
    # this is checked rather than assumed.
    if [ "$(nm --defined-only "$elf" 2>/dev/null | grep -cw '_start')" -gt 0 ]; then
        ok "example '$name' has an entry point"
    else
        bad "example '$name' has no _start"
    fi

    if [ "$(nm --undefined-only "$elf" 2>/dev/null | grep -c .)" -gt 0 ]; then
        bad "example '$name' has unresolved symbols"
    else
        ok "example '$name' is fully resolved"
    fi

    # The whole point of the SDK is that the result is a loadable AuraLite
    # binary, so check what the kernel checks: ELF64, x86_64, and a load
    # address matching the documented ABI.
    if [ "$(readelf -h "$elf" | grep -c 'ELF64')" -gt 0 ] && \
       [ "$(readelf -h "$elf" | grep -c 'X86-64')" -gt 0 ]; then
        ok "example '$name' is an ELF64 x86-64 image"
    else
        bad "example '$name' is not ELF64 x86-64"
    fi

    if [ "$(readelf -l "$elf" | grep -c "$(printf '0x%016x' $((mk_base)) )")" -gt 0 ]; then
        ok "example '$name' loads at $mk_base"
    else
        bad "example '$name' does not load at $mk_base"
    fi
done

echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
