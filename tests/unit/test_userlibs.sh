#!/usr/bin/env bash
# test_userlibs.sh — properties of the user-space static libraries (SDK S0).
#
# WHY THIS TEST EXISTS
#
# Archives change linking semantics in a way that is invisible until it bites.
# Naming an object on the link line links it unconditionally; putting the same
# object in a .a links it only if it resolves an undefined symbol. Members
# that exist for a reason other than "somebody calls this" — POSIX stubs that
# are there so a program *links* — vanish silently, and the resulting binary
# is perfectly valid and simply missing things.
#
# The failure mode is a third-party application that fails to link, or worse,
# an in-tree program that quietly loses a symbol. Neither is visible in a
# build log, so it is checked here.
#
# Run by `make test-unit`. Requires a prior build (the archives must exist);
# it skips rather than fails if they do not, because a bare `make test-unit`
# on a clean tree has no reason to build the whole OS first.

set -u
cd "$(dirname "$0")/../.."

LIBDIR="build/lib"
LIBAURAC="$LIBDIR/libaurac.a"
LIBAURAGUI="$LIBDIR/libauragui.a"
LIBAGL="$LIBDIR/libaGL.a"
CRT0="build/user/crt0.o"

pass=0; fail=0

ok()   { pass=$((pass+1)); printf '  PASS: %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL: %s\n' "$1"; }

echo "test_userlibs: user-space static libraries"

if [ ! -f "$LIBAURAC" ]; then
    echo "  SKIP: $LIBAURAC not built (run 'make libs' or 'make iso' first)"
    exit 0
fi

# --- the archives exist and are archives -------------------------------------

for lib in "$LIBAURAC" "$LIBAURAGUI" "$LIBAGL"; do
    if [ -f "$lib" ] && head -c8 "$lib" | grep -q '^!<arch>'; then
        ok "$(basename "$lib") is an ar archive"
    else
        bad "$(basename "$lib") missing or not an archive"
    fi
done

# --- crt0 is NOT in the archive ----------------------------------------------
#
# crt0.o defines only _start, which nothing references — it is reached through
# the ELF entry point, not a relocation. An archive member that resolves no
# undefined symbol is never pulled in, so archiving crt0 would produce
# programs with no entry code. It must stay a standalone object.

if ar t "$LIBAURAC" | grep -q '^crt0\.o$'; then
    bad "crt0.o is inside libaurac.a — programs would link with no entry point"
else
    ok "crt0.o is kept out of the archive"
fi

if [ -f "$CRT0" ]; then
    ok "crt0.o exists as a standalone object"
else
    bad "crt0.o missing"
fi

# --- the archive carries the whole C library ---------------------------------

count=$(ar t "$LIBAURAC" | wc -l)
if [ "$count" -ge 20 ]; then
    ok "libaurac.a holds $count objects"
else
    bad "libaurac.a holds only $count objects — members are missing"
fi

for member in libc.o malloc.o syscall.o q10_stubs.o sigreturn.o progpath.o; do
    if ar t "$LIBAURAC" | grep -qx "$member"; then
        ok "libaurac.a contains $member"
    else
        bad "libaurac.a is missing $member"
    fi
done

# --- THE property this file exists for ---------------------------------------
#
# Link a trivial program the way the Makefile does and confirm that members
# nothing references are still present. `closelog` is the canary: it is a
# stub, no in-tree program calls it, and it is exactly what plain archive
# semantics would drop.

if command -v ld.lld >/dev/null 2>&1 && [ -f build/user/hello.o ]; then
    probe=$(mktemp /tmp/userlibs_probe.XXXXXX.elf)
    if ld.lld -nostdlib -static -T libc/user.ld -z max-page-size=4096 \
              build/user/hello.o "$CRT0" \
              --whole-archive "$LIBAURAC" --no-whole-archive \
              -o "$probe" 2>/dev/null; then
        ok "a program links against libaurac.a"

        if nm --defined-only "$probe" | grep -qw '_start'; then
            ok "the linked program has an entry point"
        else
            bad "no _start in the linked program"
        fi

        if nm --defined-only "$probe" | grep -qw 'closelog'; then
            ok "an unreferenced stub survives (--whole-archive is in effect)"
        else
            bad "unreferenced stubs were dropped — --whole-archive is missing"
        fi

        if nm --undefined-only "$probe" | grep -q .; then
            bad "the linked program has unresolved symbols"
        else
            ok "the linked program is fully resolved"
        fi
    else
        bad "linking against libaurac.a failed"
    fi
    rm -f "$probe"
else
    echo "  SKIP: ld.lld or build/user/hello.o unavailable"
fi

# --- libauragui is separable -------------------------------------------------
#
# A console program must not be forced to carry the GUI library. Before S0
# every program linked auragui.o unconditionally; the archives made that
# on-demand, which is why non-GUI binaries shrank by ~14.5 KB.

if nm --defined-only build/user/calc.elf 2>/dev/null | grep -q '^.* ag_window_create$'; then
    bad "calc.elf still links auragui — the GUI library is not separable"
else
    ok "a console program does not link auragui"
fi

if nm --defined-only build/user/gcalc.elf 2>/dev/null | grep -q 'ag_window_create'; then
    ok "a GUI program does link auragui"
else
    bad "gcalc.elf is missing auragui symbols"
fi

echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
